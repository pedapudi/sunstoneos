# Radio binding and continuous SDIO receive

SunstoneOS implements a bounded binding exchange and ongoing receive worker for
the source-selected AIC8800D80 SDIO protocol. The worker attaches after the
[firmware loader](radio-firmware.md) receives application start confirmation.
It processes the published binding challenge, submits its fixed reply, checks
binding status, and continues receiving subsequent aggregates.

A host register model passes the implemented paths. Physical binding and radio
traffic remain unqualified. Neither start confirmation nor binding confirmation
establishes Wi-Fi association, Ethernet delivery, Bluetooth HCI operation, or audio.
The selected Bluetooth firmware profile uses UART; this SDIO worker does not
interpret opaque data as HCI packets.

## Binding follows the published challenge algorithm

The [published verifier][verify] accepts a 16-byte challenge and returns a
16-byte reply. Its helper named `hmac_sha256` computes ordinary SHA-256 without
a key. The native implementation follows these operations:

1. Recover four input bytes from the challenge. Each byte combines a three-byte
   majority function with one of the challenge's final four bytes.
2. Hash the recovered four bytes with SHA-256. Interleave the digest with those
   bytes and apply the source's choice/majority functions to reconstruct the
   challenge. Refuse a challenge that does not match all 16 reconstructed bytes.
3. Hash the recovered bytes again, interleave them with the digest, then apply
   the majority and choice functions to produce four transformed bytes.
4. Hash those transformed bytes and apply the challenge-encoding operations to
   produce the 16-byte reply.

The boolean functions operate on bytes: majority is `(a & b) ^ (a & c) ^ (b & c)`;
choice is `(a & b) ^ (~a & c)`. Each hash covers four input bytes, so it
requires one SHA-256 compression block; the worker uses the existing software
SHA-256 in [storage_digest.c](../drivers/storage/storage_digest.c). Each worker step performs
one hash stage, one bounded platform callback, or request encoding. No additional
binary, key file, allocator, or filesystem is required.

Before calculating the reply, the published Allwinner path calls
`sunxi_get_soc_chipid` and requires success. Its returned 16 bytes do not enter
the algorithm. The native configuration requires a bounded platform identity
callback with the same prerequisite: return 1 on success or 0 on failure.
It retains the returned bytes for diagnosis without treating them as a secret.
A real platform callback remains a physical-integration dependency; the model
uses an explicit successful stub and does not read a board identity.

The [source sender][sender] defines these messages:

| Message | ID | Parameters | Native behavior |
| --- | --- | --- | --- |
| Binding indication | `0x0417` | 16 challenge bytes | Copy the first challenge into owned storage before proceeding |
| Binding request | `0x0415` | 16 reply bytes | Encode the fixed command with CRC-8 and zero padding to one 512-byte SDIO block |
| Binding confirmation | `0x0416` | One status byte | Preserve status; accept zero and retain nonzero as a terminal binding failure |

The one-byte confirmation layout follows the source sender's one-byte output
object. The [startup caller][startup] rejects nonzero status. The native worker
requires that layout and accepts a confirmation only while its single request
is awaiting a reply. A confirmation already present before request transmission
remains an unrelated message. Clean exclusive ownership remains necessary to
exclude responses from an earlier device lifetime.

Only one binding operation is admitted per lifetime. An early firmware indication
is imported from the loader's retained challenge; the loader keeps the first
challenge if another indication arrives. Later or duplicate indications remain
consumer-owned records and cannot replace the pending challenge or silently
trigger another exchange.

## Receive preserves data and control ownership

[include/aic8800_runtime.h](../include/aic8800_runtime.h) defines the runtime
object and callback contract. The [transport](radio-transport.md) retains that
object, its firmware worker and artifacts, the card/controller objects, and all
buffers until external disposal after device quiescence. Initialization requires
fresh zero storage and refuses reuse.

The [published SDIO receive path][receive] uses different record lengths for data
and control packets. The runtime validates the complete record before exposing a
borrowed view:

| Record | Source framing | Consumer responsibility |
| --- | --- | --- |
| Data, type bit `0x10` clear | Declared length plus 60-byte hardware header, rounded to four bytes | Retain raw header and payload until a network parser accepts them |
| Command/event, type `0x11` ignoring bit 7 | Four-byte outer header plus declared payload, with four-byte alignment | Binding handles its own exact layouts; other messages remain borrowed records |
| Transmit confirmation, type `0x12` | Same control framing | Future network transmit tracking must interpret the retained bytes |
| Firmware print, type `0x13`, or unknown control subtype | Same control framing | Consumer must copy or process bytes before explicit consumption |

The worker keeps incoming data opaque. It does not infer Ethernet boundaries,
acknowledge network transmissions, parse Bluetooth, or discard unknown messages.
Control routing fields, receive byte 3, IPC pattern, and padding retain the
existing codec's source-defined treatment; no invented receive CRC invariant is
introduced.

`aic8800_transport_peek` supplies a borrowed view. Asynchronous consumers must
copy both identity and required bytes into owned storage before calling
`aic8800_transport_consume`. A held record blocks aggregate reuse and new SDIO
transfers. The output view cannot overlap the runtime, transport, controller,
card, firmware worker, or immutable firmware artifacts.

Each poll advances at most one controller step, admits one command, or parses
one record. The controller's existing FIFO budget remains in force. Receive
status is polled; controller/GIC interrupt delivery stays disabled. The existing
unsupported soft-interrupt fault remains an explicit bound. One CMD53 reads each
receive aggregate of up to 112 blocks (57,344 bytes), the largest function-1
block count that the status register reports, as described in the
[transport](radio-transport.md). No interrupt source is cleared or acknowledged
by this worker.

## Deadlines retain incomplete work

Runtime admission supplies an absolute receive-lease deadline. Binding has its
own bounded timeout starting when the challenge enters runtime ownership.
Both deadlines apply during message backpressure, command transfers, and
callback execution. The transport checks its shared monotonic clock before and
after bounded work, including the platform callback.

The owner can extend a receive lease only between complete aggregates, with no
pending controller request, held record, or unfinished binding exchange. Renewal
observes the current clock against the old deadline before extending it. A stale
idle lease cannot be revived using an old cached timestamp. Renewal reaffirms
continuous awake device ownership; sleep/wake and ownership transfer are not
implemented here.

Malformed input, nonzero binding status, failed platform identity, callback
failure, partial transfer, and expired deadlines retain the first terminal error.
The runtime keeps its challenge, reply, platform result, request, and status.
The transport keeps partial receive bytes, cursor, pending request, and all
resource ownership. Subsequent polls perform no backend access after fault.
There is no automatic reset, retry, rollback, or disposal.

## Focused model and source evidence

[checks/aic8800-runtime.py](../checks/aic8800-runtime.py) pins the published
verifier, sender, message definitions, startup caller, and receive implementation.
It compiles the unmodified verifier outside Git with a host identity stub and
compares eight challenge/reply vectors against an independent Python SHA-256
calculation. The native worker matches those same vectors.

The integrated register model uses the real controller, card, codec, transport,
firmware handoff, and runtime implementations under address/undefined-behavior
sanitizers. It starts from a simulated completed-start boundary. That checkpoint
assumes the preceding firmware execution; it does not demonstrate firmware
execution or replay the upload model. The focused paths passed:

- Binding after a later receive aggregate and an imported early challenge.
- Raw data framing, print/TX-confirmation retention, stale pre-request
  confirmations, duplicate binding indications, and consumer backpressure.
- Platform callback failure and overrun, invalid challenge, malformed challenge
  or status lengths, truncated data, and nonzero binding status.
- Missing confirmation, partial transmission, retained state after fault,
  idle lease renewal, refusal to revive expiry, and first-challenge retention.

An initial fixture compile used an incorrect request member name; it executed no
native model. Correcting the fixture allowed the single integrated run to pass.
The already-passing source/vector comparison was reused. Evidence, source/header
hashes, compiler commands, and executable identities are under
`~/.cache/sunstoneos/evidence/radio-runtime-20261016/model-corrected/`; the original
source/vector evidence is in the adjacent `model/` directory. Unchanged SDIO,
bootstrap, and firmware upload suites were not rerun.

A focused follow-up passes invalid platform callback returns `-1` and `2`, which
must fail under the callback's exact-success contract. Its evidence is in the
adjacent `invalid-platform/` directory. The corrected condition preserves the
already-tested results for 0 and 1; the complete model was not repeated.

The runtime, transport, and changed firmware worker compile once for freestanding
AArch64 with Clang 21.1.8 and warnings as errors. The adjacent
`aarch64/results.json` records commands, source/object hashes, and symbols.
Dependencies beyond native radio/card/controller APIs are `memcpy` and `memset`.
Final source identities, archived inputs, and the evidence-reuse rationale are
recorded in `final-inputs.json` at the evidence root.

```sh
python3 checks/aic8800-runtime.py \
  --reference-dir /path/to/pinned/aic8800 \
  --evidence /path/outside/repository/radio-runtime-evidence
```

All protocol references use published kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. New inspection inputs have these hashes;
existing source and firmware identities remain recorded in
[radio-firmware.md](radio-firmware.md).

| Source | SHA-256 |
| --- | --- |
| `aic8800_bsp/aic_bsp_verify.c` | `76ccd57c0ddb2f2c5b074575f709d4816a0dd7a0fbb3ba9cfa763d69d1b76ed1` |
| `aic8800_fdrv/aicwf_txrxif.c` | `2c22a400aa25e38fa908d0f43313081811aa8b988641464555ed1643585e8ce4` |
| `aic8800_fdrv/aicwf_txrxif.h` | `d70fb2f1382b5612d1efccb04fb59a7a56183765d05b731637c2c241587ad69a` |

The reference verifier has no individual license header; its containing BSP
module declares GPL. The reference helper includes the downloaded source
unchanged outside Git. Native runtime code implements the documented byte
protocol and uses the project's existing SHA-256 compression arithmetic.
No published binary or reference source is added to the repository.

Physical execution needs qualified recovery, power/pin/clock ownership, native
SDIO bindings, and the platform identity callback. Network configuration,
association, data parsing/transmission, UART/HCI, sleep/wake, and interrupt
service remain separate implementation work. No board operation or performance
measurement was performed, and no physical test executable is available yet.

[verify]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_verify.c
[sender]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_driver.c
[startup]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_8800d80.c
[receive]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/aicwf_txrxif.c
