# Bounded SDIO bootstrap transport

SunstoneOS implements function setup and an asynchronous identity-read
transaction for the published AIC8800D80 SDIO bootstrap protocol. The transport
uses the native [controller and card APIs](sdio.md) and the existing
[identity codec](radio-codec.md). An integrated host register model passes;
physical radio communication, firmware startup, Wi-Fi, and Bluetooth remain
unqualified.

The implementation is a worker component with owned buffers. It has no Linux,
filesystem, network stack, allocator, or firmware-image dependency.
[include/aic8800_sdio_transport.h](../include/aic8800_sdio_transport.h) defines
its admission, polling, and message-consumption API.

## Protocol selection and earlier writes require explicit admission

The caller selects the AIC8800D80 bootstrap variant explicitly. Completed SDIO
enumeration must report function 1 as `c8a1:0182`, at least two functions,
512-byte block capability, and multi-block support. These checks establish
compatibility with the selected source protocol. They do not establish the
installed silicon revision or choose a firmware image.

The [published bootstrap sequence][bootstrap] writes `0x40` to radio memory
address `0x40500058` before reading `0x40500000` and `0x00000004`. The identity
transaction requires trusted caller evidence that this separate write completed
in the current power/reset lifetime. The admission contains that exact address
and value, a nonzero evidence-record identifier, and assertions that the bootstrap
processor is awake and no earlier debug reads or stale confirmations remain.
When the explicit pad-write path was selected, the same source then restores
function-0 `0xf1` to zero and waits one millisecond before reading identity.
The trusted evidence must also cover that restore and settling interval.

The transport validates those fields but cannot establish their truth. It
neither encodes nor executes the earlier memory write. A caller must implement
and admit that operation separately before using this transaction on hardware.
The complete identification sequence therefore includes writes; calling it
read-only would be inaccurate. Address correlation alone cannot distinguish a
stale confirmation from an earlier outstanding read of the same address.
Exclusive bootstrap ownership must remain continuous across the earlier setup
operation and both reads. Power/reset or ownership changes invalidate admission.
External recovery must quiesce and dispose of the complete transport/card lifetime;
it cannot clear an owned object and retry initialization.

## Setup retains card ownership and preserves unrelated enable bits

The caller must own the entire card and prepared controller exclusively.
Shared clock, pin, power, voltage, and recovery prerequisites remain those of
the [SDIO controller](sdio.md). The transport retains its card, controller,
callback context, and buffers through completion or fault. Initialization needs
zero storage and succeeds once; there is no reinitialization or disposal API.

The [published function setup][sdio] and [standard SDIO operations][io] establish
the following sequence. Every register access uses CMD52 through the card API.
The original block size and enable-register bytes remain available for diagnosis.

| Operation | Native behavior |
| --- | --- |
| Function-1 block size, FBR `0x110`/`0x111` | Read the prior size, write 512, then read both bytes to verify it |
| Common IOEx/IORx, `0x02`/`0x03` | Preserve existing IOEx bits, enable function 1, and poll its ready bit |
| Function-0 `0xf2` | Write the source value `0x7f` |
| Function-0 `0xf1` | Caller explicitly selects either preservation or the source write `0x80` |
| Pad settling | Wait at least one millisecond in admitted clock units |
| Function-1 `0x07` | Write `1`, matching the source's byte-mode disable setting |
| Common IEN, `0x04` | Preserve existing bits and enable the source's master/function-1/function-2 bits with mask `0x07` |
| Function-1 interrupt enable, `0x00` | Preserve existing bits and enable the source mask `0x07` |

The source makes the `0xf1` write conditional on a build option and a recovery
flag. The native configuration exposes the choice instead of inferring that
condition from card IDs. It does not raise the controller clock or change bus
width. The one-bit initialization clock remains in use.

Function-ready polling has both an overall setup deadline and the function's
advertised enable timeout. The caller supplies ticks per millisecond, command
timeout, and polling interval. Overflowing time calculations fail without
continuing the operation. The pad writes and interrupt settings intentionally
change device configuration; successful CMD52 completion is the evidence retained
for those source-defined writes.

The transport does not roll back setup. A register write can take effect even
when the controller callback reports failure. Such a failure retains the request,
prior settings, and card ownership for external recovery. Clearing the object
would erase that ownership and is forbidden.

## Identity requests use bounded FIFO transfers

Each identity request uses the existing codec to produce 20 bytes. The transport
zeros the complete 512-byte transmit buffer before encoding the request, leaving
the source's required zero tail and block padding. It reads the function-1 credit
register at `0x03` and waits until one 512-byte request fits within the available
credits. The source defines each credit as 1,536 bytes and uses a strict less-than
comparison for command submission.

CMD53 sends the block to the fixed function-1 FIFO address `0x10`. It never
increments the FIFO address. Receive polling reads function-1 status at `0x04`;
status values select a block count or byte-length register `0x05`. The latter
reports words of four bytes. The source's function-2 status encoding still reads
the function-1 FIFO, and the implementation follows that published route.

Receive requests use fixed FIFO address `0x0f`. Byte-mode transfers contain at
most 512 bytes; larger transfers must be multiples of 512. Every transfer is
whole-word aligned in length and bounded by the owned 4,096-byte receive buffer.
The controller itself moves at most 16 FIFO words per poll and requires its
completion events in addition to FIFO progress.

The source's miscellaneous interrupt bit `0x80` enters a separate soft-interrupt
path that clears a pending bit. The native transport does not claim that path is
implemented. It retains the raw status and enters a terminal unsupported-interrupt
fault without clearing a device or controller interrupt. Oversized or invalid
receive lengths similarly fail before a FIFO read. These limits are explicit
qualification boundaries, even if ordinary identity replies fit within them.

Card-level interrupt generation is enabled during setup, but controller/GIC
delivery remains disabled. The transport polls status and never acknowledges the
controller's persistent SDIO interrupt latch. A later interrupt binding must
coordinate acknowledgement after servicing device causes; this component does
not install an ISR or acquire interrupt ownership from another worker.

## Unrelated messages apply backpressure without disappearing

A poll parses at most one control record from the receive aggregate. The first
matching confirmation for each requested address is copied into owned scalar
storage. Both reads complete before the codec derives revision, H-variant status,
and subrevision. Unknown revision codes remain visible, with no firmware choice.

A valid unrelated response, including a duplicate confirmation, remains in the
receive buffer. `aic8800_transport_peek` returns a borrowed view of its record and
parameters. The consumer must copy any asynchronous work into its own storage
before calling `aic8800_transport_consume`. Until consumption, polling does not
advance that record, overwrite the aggregate, or submit another transfer.

The remaining aggregate is parsed before the next identity request. A zero outer
length marks the source-defined end of the aggregate. Malformed or unsupported
records retain the aggregate and cursor in a terminal fault. No unimplemented
message is silently acknowledged or dropped.

The operation deadline continues during backpressure. On expiry, the held view
remains available through `peek`; `consume` is refused. Partial FIFO reads retain
the received bytes, transfer count, and controller request. Terminal polls make
no further backend calls. After successful completion, the owned identity and
confirmation scalars remain available in the transport object.

## Integrated evidence and remaining work

[checks/aic8800-sdio-transport.py](../checks/aic8800-sdio-transport.py) builds the
transport with the real controller, enumeration, and codec implementations under
address and undefined-behavior sanitizers. It reuses the existing register model
without running that model's independent test suite. The integrated fixture covers:

- Delayed function readiness, block-size verification, preserved enable bits,
  both pad choices, and the required settling interval.
- Credit backpressure, two identity reads, fixed FIFO addressing, zero transmit
  padding, and retained unrelated responses before a matching confirmation.
- Oversized receive length, an unsupported interrupt reason, malformed byte-mode
  response, partial FIFO failure, and a setup write that takes effect before failure.
- Deadline expiry while waiting for credit or holding an unrelated message,
  with retained state and no backend access after fault.

The integrated fixture passed. Raw evidence, commands, source/header hashes,
and executable identity are under
`~/.cache/sunstoneos/evidence/radio-transport-20261016/model-corrected/`.
The controller, card, and codec component evidence remains applicable because
those implementations were unchanged. The fixture supplies trusted admission
assertions; it does not validate the separate memory-write or pad-restore operation.
Freestanding AArch64 compilation passed once; `aarch64/results.json` records
the command and object identity. Its only runtime dependency beyond the native
SDIO and codec APIs is `memset`. No board operation, firmware selection,
performance run, or unchanged component-suite rerun was performed.

```sh
python3 checks/aic8800-sdio-transport.py \
  --reference-dir /path/to/pinned/radio-transport-references \
  --evidence /path/outside/repository/radio-transport-evidence
```

The reference directory contains `aicsdio.c`, `aicsdio.h`, and `sdio_io.c` with
the hashes below. Physical execution still needs shared-resource admission,
qualified recovery, controller bindings, and a separately implemented memory
setup write. Soft-interrupt handling, normal data traffic, sleep/wake management,
firmware loading, and network/HCI integration remain outstanding. There is no
board executable to submit to the physical test queue.

## Pinned references

All references use kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` from the published Orange Pi source.
Downloaded files and raw artifacts remain outside Git.

| Reference | SHA-256 |
| --- | --- |
| `bsp/drivers/net/wireless/aic8800/aic8800_bsp/aicsdio.c` | `f08bcfa63f9b7226f4b26832ee9b6088c057fd5ee06389f3ad872f13f9246f0c` |
| `bsp/drivers/net/wireless/aic8800/aic8800_bsp/aicsdio.h` | `af520385376df1843a6e24c01cc7128a151b148fea18d73c0d20f1a345bc6571` |
| `drivers/mmc/core/sdio_io.c` | `2f9db1b81f01d359faa689fb7609c04d6a9fdd7d833d16e201f74d7201e51e44` |
| `bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_8800d80.c` | `fcf647395a7f007f22cd19f4e15dcc821e924da55b72528395a9972e5c6290e8` |

[sdio]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aicsdio.c
[io]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/drivers/mmc/core/sdio_io.c
[bootstrap]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_8800d80.c
