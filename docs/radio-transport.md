# Bounded SDIO bootstrap transport

SunstoneOS implements function setup and an asynchronous bootstrap identification
transaction for the published AIC8800D80 SDIO bootstrap protocol. The transport
uses the native [controller and card APIs](sdio.md) and the existing
[identity codec](radio-codec.md). An integrated host register model passes;
physical radio communication, firmware startup, Wi-Fi, and Bluetooth remain
unqualified.

The implementation is a worker component with owned buffers. It has no Linux,
filesystem, network stack, or allocator dependency. After identity, it can hand
off to the [native firmware engine](radio-firmware.md) under the same owner.
[include/aic8800_sdio_transport.h](../include/aic8800_sdio_transport.h) defines
its admission, polling, and message-consumption API.

## Protocol selection and the fixed setup write require explicit admission

The caller selects the AIC8800D80 bootstrap variant explicitly. Completed SDIO
enumeration must report function 1 as `c8a1:0182`, at least two functions,
512-byte block capability, and multi-block support. These checks establish
compatibility with the selected source protocol. They do not establish the
installed silicon revision or choose a firmware image.

The identification admission authorizes exactly one radio-memory write:
`0x40` to address `0x40500058`, as required by the [published sequence][bootstrap].
A nonzero admission-record identifier tracks that authorization. The address and
value must match the allowlist; the encoder itself accepts no arbitrary address
or value. Admission also requires an awake bootstrap processor, no outstanding
debug operations or stale confirmations, and continuous exclusive ownership.

Polling sends the setup write and waits for its confirmation before advancing.
The [published message definitions][messages] declare write request `0x0402`
and confirmation `0x0403`, each with eight parameter bytes. The confirmation
contains fields named `memaddr` and `memdata`. The [source sender][sender] waits
for the confirmation ID and discards its fields; it does not establish an echo
rule or read the register back. The native decoder therefore requires the
exact eight-byte layout and retains both fields without comparing their values.

One outstanding operation and exclusive bootstrap ownership establish response
correlation. The returned fields are diagnostic data, not independently verified
register readback. Message ID or address matching cannot distinguish a stale
confirmation from an earlier operation. A reset, power change, or ownership loss
invalidates admission; external recovery must quiesce and dispose of the complete
transport/card lifetime before reuse.

After the write confirmation and any remaining aggregate records are consumed,
the explicit pad-write path restores function-0 `0xf1` to zero and waits at
least one millisecond. The preservation path does not modify that register.
The transport then reads `0x40500000` and `0x00000004` in order. It implements
the published identification sequence within its documented protocol limits.

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

## Bootstrap requests use bounded FIFO transfers

The setup-write request contains 24 bytes; each identity-read request contains
20 bytes. The transport zeros the 512-byte transmit block used for each
bootstrap request before encoding it, leaving the source's required zero tail
and block padding. It reads the function-1 credit
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
whole-word aligned in length. Function-1 status values 1 to 112 report that
many 512-byte blocks; values 113 to 127 select byte mode or the function-2
encoding. The owned receive buffer therefore holds 57,344 bytes, the largest
block-mode report.

One CMD53 reads the complete reported aggregate. The [source receive
path][sdio] reads the reported length with a single `sdio_readsb` call, and
the [Linux SDIO helper][io] splits a transfer only above 511 blocks or the host's
block-count limit, 8,192 for the Allwinner controller. Firmware acceptance of an
aggregate drained by several separate CMD53 reads is not established, so the
transport does not split one. The configured command timeout applies to each
started 4,096 bytes of a receive transfer: a 10-block read gets two timeouts.
At one bit and 400 kHz, 4,096 bytes need about 82 ms of bus time. The runtime
lease deadline still bounds every transfer. The controller itself moves at most
16 FIFO words per poll at one bit, or one 512-byte block at four bits, and
requires its completion events in addition to FIFO progress.

The source's miscellaneous interrupt bit `0x80` enters a separate soft-interrupt
path that clears a pending bit. The native transport does not claim that path is
implemented. It retains the raw status and enters a terminal unsupported-interrupt
fault without clearing a device or controller interrupt. A byte-mode length
over 512 bytes and any length beyond the receive buffer similarly fail before a
FIFO read. These limits are explicit qualification boundaries.

Card-level interrupt generation is enabled during setup, but controller/GIC
delivery remains disabled. The transport polls status and never acknowledges the
controller's persistent SDIO interrupt latch. A later interrupt binding must
coordinate acknowledgement after servicing device causes; this component does
not install an ISR or acquire interrupt ownership from another worker.

## Owners repeat calls only while they advance

Each poll still performs at most one host step, one next admission or one
record parse, with clock and deadline observations before and after it. The
transport's `progress` counter increases on a poll that changes its state or
phase, admits or completes a host request, moves the receive cursor or held
record, or makes controller progress such as moving FIFO words. Consuming or
retiring a record also increases it. A poll that only waits leaves it
unchanged: a pending host request without progress, a poll interval or
settling delay, or a held record awaiting its consumer. An owner may therefore
repeat polls while the counter increases and yield when it does not, without
changing the step semantics or deadlines.

[checks/radio-poll-batching.py](../checks/radio-poll-batching.py) runs the setup
and bootstrap identity sequence of the integrated fixture twice: once with one
poll per owner tick and once repeating polls while `progress` increases. Every
call stays within the controller's FIFO budget, and every repeated tick ends on a
waiting call. The fixture's controller model completes bus work instantly, so
only the owner's tick count is meaningful: 172 ticks drop to 42.
This shows the removed per-step tick waits, not physical bus time.

## Unrelated messages apply backpressure without disappearing

A poll parses at most one control record from the receive aggregate. The first
matching confirmation for the setup-write message ID or requested read address
is copied into owned scalar storage. Both reads complete before the codec derives
revision, H-variant status, and subrevision. Unknown revision codes remain visible, with no firmware choice.

A valid unrelated response, including a duplicate confirmation, remains in the
receive buffer. `aic8800_transport_peek` returns a borrowed view of its record and
parameters. The consumer must copy any asynchronous work into its own storage
before calling `aic8800_transport_consume`. Until consumption, polling does not
advance that record, overwrite the aggregate, or submit another transfer.

The remaining aggregate is parsed before the next bootstrap operation. A zero outer
length marks the source-defined end of the aggregate. Malformed or unsupported
records retain the aggregate and cursor in a terminal fault. No unimplemented
message is silently acknowledged or dropped.

The operation deadline continues during backpressure. On expiry, the held view
remains available through `peek`; `consume` is refused. Partial FIFO reads retain
the received bytes, transfer count, and controller request. Terminal polls make
no further backend calls. After successful completion, the owned identity,
write-confirmation fields, and read-confirmation scalars remain available in
the transport object.

## Integrated evidence and remaining work

[checks/aic8800-sdio-transport.py](../checks/aic8800-sdio-transport.py) builds the
transport with the real controller, enumeration, and codec implementations under
address and undefined-behavior sanitizers. It reuses the existing register model
without running that model's independent test suite. The integrated fixture covers:

- Delayed function readiness, block-size verification, preserved enable bits,
  both pad choices, and the required settling interval.
- Credit backpressure, two identity reads, fixed FIFO addressing, zero transmit
  padding, and retained unrelated responses before a matching confirmation.
- A 10-block aggregate holding three 1,500-byte records before the setup-write
  confirmation, which starts at byte 4,512. One CMD53 reads all 5,120 bytes,
  with a deadline of two command timeouts, and every record is parsed in order.
- A byte-mode length over 512 bytes, an unsupported interrupt reason, malformed
  byte-mode response, partial FIFO failure, and a setup write that takes effect
  before failure.
- Deadline expiry while waiting for credit or holding an unrelated message,
  with retained state and no backend access after fault.

[checks/radio-transport-aggregate.py](../checks/radio-transport-aggregate.py)
runs the same model after runtime start, with the native dispatcher as receive
owner. One CMD53 reads an 18-block aggregate holding three 1,500-byte data
records, a 4,604-byte print record and an unrelated control message. The
dispatcher retires all five in order and keeps a diagnostic copy of the print
record truncated to 4,096 bytes.

The function-setup and receive-boundary evidence remains under
`~/.cache/sunstoneos/evidence/radio-transport-20261016/model-corrected/`.
A focused bootstrap integration case passed after adding the fixed memory write:
it exercises an unrelated response, a write confirmation with non-echo fields,
conditional pad restoration, settling, and both identity reads. Additional cases
cover malformed/truncated write confirmations, missing confirmation after a
modeled write effect, an uncertain pad write, and partial transmit failure.

The focused result, commands, source/header hashes, and executable identity are
under `~/.cache/sunstoneos/evidence/radio-bootstrap-20261016/model/`. Controller,
card, and unaffected codec-path evidence is reused. The changed codec and
transport compile for freestanding AArch64; `aarch64/results.json` records both
object identities. Their runtime dependency beyond the native SDIO/codec APIs
is `memset`. No board operation, firmware selection, performance run, or unchanged
component-suite rerun was performed.

```sh
python3 checks/aic8800-sdio-transport.py \
  --reference-dir /path/to/pinned/radio-transport-references \
  --evidence /path/outside/repository/radio-bootstrap-evidence \
  --case bootstrap
```

The runner verifies its pinned reference files, including the message header
and sender. The [firmware engine](radio-firmware.md) extends the same transport with
1,536-byte uploads after completed identity. Physical execution still needs
shared-resource admission, qualified recovery, controller bindings, and verified
current bootstrap ownership. The [runtime worker](radio-runtime.md) supplies
binding exchange and continuous receive with retained raw data/control records.
Soft-interrupt handling, normal data traffic, sleep/wake management,
physical binding verification and network/HCI integration remain
outstanding. There is no board executable to submit to the physical test queue.

## Pinned references

All references use kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` from the published Orange Pi source.
Downloaded files and raw artifacts remain outside Git.

| Reference | SHA-256 |
| --- | --- |
| `bsp/drivers/net/wireless/aic8800/aic8800_bsp/aicsdio.c` | `f08bcfa63f9b7226f4b26832ee9b6088c057fd5ee06389f3ad872f13f9246f0c` |
| `bsp/drivers/net/wireless/aic8800/aic8800_bsp/aicsdio.h` | `af520385376df1843a6e24c01cc7128a151b148fea18d73c0d20f1a345bc6571` |
| `bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_driver.c` | `43439d69cd1c3f537ee90f50d4bc52d2a54379f41092f625ded080daacf7de6e` |
| `bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_driver.h` | `9f72ebdd743ac0af134df50ebc36ea25876b9a361a89b82e17a4a8c2e4d918b3` |
| `drivers/mmc/core/sdio_io.c` | `2f9db1b81f01d359faa689fb7609c04d6a9fdd7d833d16e201f74d7201e51e44` |
| `bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_8800d80.c` | `fcf647395a7f007f22cd19f4e15dcc821e924da55b72528395a9972e5c6290e8` |

[sdio]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aicsdio.c
[io]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/drivers/mmc/core/sdio_io.c
[bootstrap]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_8800d80.c

[messages]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_driver.h
[sender]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_driver.c
