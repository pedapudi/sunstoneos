# USB keyboard and mouse input

The HID decoder is implemented and host-tested. TinyUSB's pinned HID host
class performs enumeration and interrupt transfers. The decoder consumes its
report descriptor and copied completed reports on the USB owner task. Board
operation, physical hotplug and electrical timing remain unqualified.

The interface in `include/sunstone_usb_hid.h` accepts caller-owned state and a
synchronous event callback. The callback receives keyboard usages, button
changes, relative X/Y/wheel motion and interface attachment changes. Keyboard
usages are USB HID usage-page-7 numbers; modifiers use usages 224 through 231.
The decoder emits changes once and leaves key repeat to its consumer.
Attachment and detachment events carry capability bits for keyboard and pointer
input. The consumer must copy each event before returning and must report queue
overflow explicitly. Callbacks must not reenter the decoder.

## Report protocol and state ownership

Boot keyboards and three-byte boot mice work through their report descriptors.
TinyUSB must select Report protocol before mounting a boot-capable interface:
call `tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT)` before enumeration.
TinyUSB's default Boot protocol would make a report-ID descriptor disagree with
the received bytes. A failed protocol change must prevent decoder admission.

Each mounted interface binds its USB address, TinyUSB instance and nonzero
attachment generation. The transport supplies a distinct generation when an
address or interface is reused. Reports and detach operations with another
generation are refused. A generation check supplements the transport's DMA
completion and session checks; it cannot establish hardware quiescence.

Parsing a report first computes its complete replacement state. Malformed or
short input publishes no events and preserves held keys and buttons. Keyboard
rollover reports also preserve state and return a separate result. Reports with
different IDs retain independent held states. The decoder publishes the union,
so releasing one report ID cannot release a key still held by another.
Detachment releases every held key and button before publishing the detach
event. The state contains no retained descriptor or report-buffer pointers.

## Supported descriptors and fixed limits

The parser supports short HID items, bounded global Push/Pop, nested
collections, report IDs, explicit usage lists and usage ranges. Keyboard arrays
and variable bitmaps are supported, including modifier bits and bitmap reports
that represent many simultaneously held keys. Mouse input supports variable
buttons and signed relative X, Y and wheel fields, including packed 12-bit axes.
The parser skips fields in other application collections while preserving their
input offsets and report IDs.

The limits are 1,024 descriptor bytes, eight report IDs, 48 input fields,
32 explicit usages per field, eight collection levels and four global stack
entries. Reports contain at most 64 bytes including their report ID. Each field
has at most 32 bits per element; each input item has at most 512 elements.
Keys above usage 255 and buttons above 32 are omitted. Consumer controls,
horizontal pan, absolute pointers, LEDs and output reports have no event API.
Long items, usage delimiters and mouse arrays are refused. These bounds cover
the recorded fixtures below; they do not establish compatibility with every
receiver or keyboard.

The decoder performs no allocation. Work is bounded by descriptor and report
limits. Its caller allocates one state per admitted interface. Admission must
also enforce TinyUSB's enumeration-buffer, interface-count and endpoint-buffer
limits; a larger decoder limit cannot enlarge the transport's buffers.

## Host evidence

Run the focused check with cached public captures:

```sh
cache="$HOME/.cache/sunstoneos"
"$cache/venv/bin/python3" checks/usb-hid.py \
  --output "$cache/build/usb-hid-check" \
  --captures "$cache/sources/usb-host-1031/captures"
```

The check uses Clang 21 with address and undefined-behavior sanitizers. Unit
cases cover modifier and key transitions, duplicate reports, rollover,
independent report IDs, shared keys across IDs, bitmap keys, three-byte mice,
truncated reports, invalid generations and detach releases. Descriptor cases
cover truncation, invalid item sizes, stack underflow/overflow, mixed numbered
and unnumbered reports and deterministic malformed-input fuzzing.

A separate fixture links the actual pinned TinyUSB `hid_host.c`. It exercises
Report protocol selection, enumeration callbacks, receive-buffer completion,
short input and interface closure. Host control and endpoint completion are
modeled. The transport must reject failed USB transfers before TinyUSB publishes
a received report because the pinned HID class does not validate that result.

Public Wireshark recordings supply independent descriptor and traffic evidence:

| Recording | Descriptor | Captured reports | Decoder result |
| --- | --- | --- | --- |
| Microsoft keyboard | Packet 20, 60 bytes | 211 | 18 key changes and attachment/detachment |
| Logitech receiver keyboard | Packet 22, 59 bytes | 106 idle reports | Attachment/detachment; no captured key press |
| Logitech receiver mouse | Packet 29, 148 bytes | 128 | 128 motion events and attachment/detachment |

The replay check extracts descriptors by pairing USB control requests and
completions. It compares every resulting event with a separate decoder of the
recorded fixed report layouts. The receiver mouse uses report ID 2 and packed
12-bit signed axes. Raw captures and extracted fixtures remain outside tracked
source. [The source record](usb-hid-sources.json) identifies their download URLs,
hashes, descriptor packets and redistribution limits.

The transport requires the complete advertised HID report descriptor before
passing it to TinyUSB. A short reply is refused while its control state stays
retained; bytes left by an earlier enumeration cannot complete the descriptor.
The adapter regression covers every short length and exact completion.

The legacy `ss_key_report` API preserves the first unread snapshot and one
pending latest snapshot. Publication and application copying have exclusive
atomic claims; later updates can coalesce. Detach invalidates snapshots from
that attachment. The separate input-event queue preserves its existing contract.
The mailbox regression covers sequential, interrupted and concurrent reads
under address, undefined-behavior and thread sanitizers.

Physical qualification still requires an attached keyboard, mouse and combined
receiver on each supported host route. A supervised board run must establish
interrupt completions, application delivery, held-key and held-button release
on unplug, reuse of an address with a new generation, and bounded refusal after
a transfer failure. Host replay does not establish those properties.
