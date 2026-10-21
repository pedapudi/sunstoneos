# SDIO radio identity codec

The native AIC8800D80 codec serializes fixed bootstrap-write and identity-read
commands and validates control responses from an SDIO receive buffer. It is
implemented and checked on the host. The [SDIO transport](radio-transport.md)
uses the codec with native card enumeration and controller transfers. Physical
radio identification, firmware loading, and interrupt delivery remain unqualified.
The [radio dependency report](radio.md) describes the board wiring and
bootstrap requirements.

The two encoded requests target radio addresses `0x40500000` and `0x00000004`.
Encoding only writes a caller-owned byte buffer. The full board identification
sequence starts with the reference's write of `0x40` to `0x40500058` and SDIO
setup operations. The native transport performs that sequence under explicit
admission; the complete hardware sequence includes writes.

## Commands and responses have different layouts

[The command serializer][commands] emits a four-byte transport header, a
four-byte dummy word, an eight-byte message header, and request parameters.
The [receive path][receive] uses a four-byte transport header followed by a
12-byte message header that includes a pattern word. All integer fields below
use little-endian byte order; input and output buffers need no word alignment.

| Identity read request offset | Field |
| --- | --- |
| 0–1 | Length 16, excluding the four-byte outer header |
| 2 | Command type `0x11` |
| 3 | CRC-8 of bytes 0–2 |
| 4–7 | Zero dummy word |
| 8–9 | `DBG_MEM_READ_REQ`, `0x0400` |
| 10–11 | Debug task destination, `1` |
| 12–13 | Host task source, `100` |
| 14–15 | Parameter length, `4` |
| 16–19 | Requested radio address |

The [D80 header CRC][sdio] uses polynomial `0x107`, initial value zero,
no bit reflection, and no final XOR. The encoder produces exactly 20 bytes
and preserves the remainder of the destination buffer. The codec exports this
CRC as `aic8800_sdio_header_crc`, which the data worker uses for its type-1
transmit header. It also exports `aic8800_sdio_command_header`, which writes
the 16-byte command header above for any message ID and parameter length; the
destination task is the message ID shifted right by ten bits. The firmware,
runtime, Wi-Fi and security workers build their requests with it. CMD53 block padding,
FIFO credit accounting, and transfer submission belong to the native transport.

| Control response offset | Field |
| --- | --- |
| 0–1 | Message bytes after the four-byte outer header |
| 2 | Control subtype; the reference ignores bit 7 |
| 3 | Source-unspecified receive header byte |
| 4–5 | Message ID; read confirmation is `0x0401` |
| 6–9 | Dummy destination/source identifiers |
| 10–11 | Parameter length |
| 12–15 | IPC pattern word |
| 16 onward | Parameters; read confirmation contains address then value |

The decoder checks the complete aligned record before publishing output.
Its consumed length is four bytes plus the outer payload length rounded up
to four bytes. Message parameters must fit the declared payload and the
reference's 1,024-byte parameter limit. A read confirmation requires exactly
eight parameter bytes. Trailing payload bytes and alignment padding remain
uninterpreted because the source supplies no invariant for them.

Receive byte 3, dummy routing fields, and the pattern word are also
uninterpreted. Applying the transmit CRC rule to receive packets would impose
an unsupported constraint. Physical qualification must establish any stronger
receive-integrity rule before it becomes a rejection condition.

## The bootstrap encoder permits one fixed write

`aic8800_sdio_encode_bootstrap_setup` emits a 24-byte request for
`0x40500058=0x40`. It accepts no address or value arguments. The layout matches
the read request except for outer length 20, message ID `0x0402`, parameter
length eight, and two little-endian parameters: the fixed address followed by
the fixed value. The header uses the same CRC rule. Failure leaves output
unchanged, and successful encoding leaves the destination tail untouched.

`aic8800_sdio_decode_bootstrap_setup_response` shares the bounded control-record
parser. It recognizes write confirmation ID `0x0403` only with exactly eight
parameter bytes, as declared in the pinned message header. It retains those
bytes as address/data fields and marks the result as a write confirmation.
The source sender discards the fields; their names do not establish echo
semantics. The decoder therefore accepts non-echo values and does not claim
that a successful confirmation is independently verified register readback.

All other valid control records remain unrelated, including read confirmations.
The transport correlates a write confirmation by ID under its explicit exclusive
bootstrap admission, with one outstanding operation and no stale confirmations.
The identity-read decoder retains its original behavior; a write confirmation
received through that API remains an unrelated control message.

## The caller owns buffers and decides what to do with unrelated responses

[include/aic8800_sdio_codec.h](../include/aic8800_sdio_codec.h) defines the API.
The codec allocates no memory, invokes no callbacks, performs no I/O, and holds
no global state. Each call performs fixed-size parsing or serialization.

| Decoder result | Output and caller responsibility |
| --- | --- |
| `OK` | Complete read confirmation matches the requested identity address |
| `UNRELATED` | Complete control record is retained as a borrowed view for later dispatch |
| `END` | Zero outer length marks the aggregate's end; output is unchanged |
| `UNSUPPORTED` | A data record uses a different framing rule; output is unchanged |
| `TRUNCATED` | Declared or required bytes are missing; output is unchanged |
| `MALFORMED` | Message header or confirmation payload violates the checked format; output is unchanged |
| `INVALID` | Invalid selector, NULL pointer, overflowing span, or input/output alias; output is unchanged |

For `OK` and `UNRELATED`, the output includes the raw record, unpadded length,
and consumed length. A recognized message also exposes its ID and parameter
view. A read confirmation for a different address remains `UNRELATED`, with
its scalar address and value available. An unknown control subtype is retained
without interpreting its bytes as a message header.

All record and parameter pointers borrow the caller's receive storage. Copy
bytes into owned storage before deferring dispatch or reusing that buffer.
The native transport retains unrelated records until the consumer accepts them.
Advancing by `consumed` and dropping them would lose events. The codec itself
never advances a queue.
Input and output objects must not overlap.

The identity helper combines two address-correlated confirmations. It retains
both raw words and derives revision, subrevision, and H-variant status using
[the pinned definitions][identity-definitions] and [bootstrap logic][bootstrap].
Revision is bits 16–21 of the chip word. H status requires both bits 22 and 23;
subrevision is the low byte after shifting the second word right by four.
The accepted source revision codes are `1`, `3`, `7`, and `15`.
Other values remain visible with `known_revision=0`.

The helper only reads scalar fields from the response objects, so owned scalar
copies remain usable after the receive buffer expires. The caller must first
confirm SDIO identity `c8a1:0182` and correlate the two completed requests.
The helper does not establish physical identity, choose firmware, or infer
that a controller is safe to start.

## A focused host check covers serialization, bounds, and ownership

[checks/aic8800-sdio-codec.py](../checks/aic8800-sdio-codec.py) verifies the
pinned reference hashes, extracts its CRC function into an external evidence
directory with the original source notice, and compiles one host fixture.
The fixture checks both requests, unaligned input, two consecutive responses,
identity fields, unknown revisions, unrelated response retention, truncated
records, malformed payload sizes, padding, and alias rejection.
Sentinel comparisons verify that rejected calls preserve output.

The fixture passed with address and undefined-behavior sanitizers. Evidence is
in `~/.cache/sunstoneos/evidence/radio-codec-20261016/`; `results.json` records
the compiler, command, input hashes, executable hash, and result. No board
operation, emulator run, performance measurement, or existing suite rerun was
needed for the read codec. The write extension is covered by the focused
[bootstrap integration check](radio-transport.md), including non-echo confirmation
fields, malformed/truncated write confirmations, and unchanged rejected output.
Existing read-path evidence is reused; the complete read-codec suite was not
rerun. There is no board executable until the shared radio lifecycle has concrete
bindings.

```sh
python3 checks/aic8800-sdio-codec.py \
  --reference-dir /path/to/pinned/aic8800_bsp \
  --evidence /path/outside/repository/radio-codec-evidence
```

All source references use kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` under
`bsp/drivers/net/wireless/aic8800/aic8800_bsp/`.

| Reference | SHA-256 |
| --- | --- |
| `aic_bsp_driver.c` | `43439d69cd1c3f537ee90f50d4bc52d2a54379f41092f625ded080daacf7de6e` |
| `aic_bsp_driver.h` | `9f72ebdd743ac0af134df50ebc36ea25876b9a361a89b82e17a4a8c2e4d918b3` |
| `aicsdio.c` | `f08bcfa63f9b7226f4b26832ee9b6088c057fd5ee06389f3ad872f13f9246f0c` |
| `aic_bsp_txrxif.c` | `e8ed777e6c7c26c77e0914f48a89ffde4f6bdaf798445b04489abb7c399d3573` |
| `aic_bsp_export.h` | `10c913d5c5ab6d5f48e70be8a710ffe99416496f1718f12bc2dd901b380c00a5` |
| `aic_bsp_8800d80.c` | `fcf647395a7f007f22cd19f4e15dcc821e924da55b72528395a9972e5c6290e8` |

[commands]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_driver.c
[receive]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_txrxif.c
[sdio]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aicsdio.c
[identity-definitions]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_export.h
[bootstrap]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_8800d80.c
