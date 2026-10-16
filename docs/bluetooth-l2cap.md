# BR/EDR L2CAP basic-mode channels

The native L2CAP worker opens and configures channels in both directions over
established [Bluetooth ACL links](bluetooth-acl.md). It fragments outgoing packets,
reassembles incoming packets, and retains data until its owner releases it.
L2CAP supplies channel framing for protocols such as SDP and RFCOMM. It does not
authenticate a peer or implement either protocol.

The worker is implemented in [bt_l2cap.c](../drivers/bluetooth/bt_l2cap.c), with
its interface in [bt_l2cap.h](../include/bt_l2cap.h). One task exclusively owns
L2CAP and the underlying ACL, HCI and UART workers. Existing ACL links must be
established before initialization. Firmware, pin, power, clock, generation and
polling prerequisites remain those of the lower workers.

## Channel admission includes an explicit security policy

A caller opens a channel by link generation and protocol/service multiplexer
(PSM), the number identifying its upper-layer protocol. Peer Connection Requests
use the same admission callback. The callback must explicitly authorize the PSM
on the link's actual security state. A callback can permit unsecured service
discovery, refuse an unsupported service, or refuse access for security reasons.

Successful Connection Responses establish paired local and remote channel
identifiers (CIDs). Both directions must then complete Configuration before the
channel becomes OPEN. The local receive limit is advertised to the peer; the
peer's receive limit bounds outgoing data. A positive peer configuration response
must enter the UART before the worker considers that direction configured.
The controller may still own its ACL credit at that point.

The implementation supports basic-mode MTU negotiation, default infinite flush
timeout, and explicit Basic Mode in the retransmission/flow-control option.
Unknown mandatory options receive a negative response containing their bytes
when that response fits. Unknown hints are ignored. Unsupported combinations or
large negative option lists receive a generic Configuration rejection.

Configuration Requests may continue across several commands. Each command must
contain complete options; their combined options have a fixed capacity. A
continued Configuration Response prompts an empty Configuration Request with a
new identifier. At most two adjusted-MTU retries are admitted after unacceptable
parameters; unsuccessful negotiation then requests channel disconnection.

Local and peer Disconnection Requests preserve the paired CIDs. Closing a channel
prevents new application sends. An already-started outgoing L2CAP packet finishes
its fragment sequence while the ACL link remains usable, then reports failure.
This preserves framing for later packets on that link. Its backing remains owned
through every outstanding ACL fragment.

## Protocol timers and caller deadlines preserve separate ownership

The request timer (RTX) starts when the complete request has entered UART. Its
configured interval is 1–60 seconds. The first matching Pending response replaces
RTX with the extended response timer (ERTX), whose interval is 60–300 seconds.
Repeated Pending responses preserve the first ERTX deadline. A valid final
response stops the timer even when packet-pool pressure delays the next action.

The admitted profile uses infinite flush timeout: the controller keeps transmitting
an ACL packet until delivery or link failure. Core section 6.2 recommends zero
L2CAP retransmissions for this profile, which is the implemented policy.
Finite-flush retransmissions require an actual HCI flush-configuration and
readback path before they can be admitted.

A local open or close retains the caller's absolute deadline independently of
RTX and ERTX. The caller can end its wait before the protocol timer expires.
Incoming channel setup has an ERTX-duration operation bound. Caller expiry,
RTX expiry and ERTX expiry have distinct completion reasons and put the channel
in `BT_L2CAP_QUARANTINED`. They prevent further application use while preserving
unresolved request identities and backing.

Each signaling packet has a separate ERTX-duration transport deadline. Partial
receive reassembly uses the RTX duration. A protocol timeout cannot release a
controller-owned fragment or return its credit. Later controller completion can
release fragment backing, while the uncertain channel remains quarantined.
Transport deadline expiry faults the worker and retains all remaining ownership.

## Confirmed closure permits host storage reuse with fresh wire identities

Channel handles are opaque 64-bit generations returned by `bt_l2cap_open`.
A received packet exposes the same generation in its `channel` field. Local and
remote wire CIDs remain available in the channel record for protocol inspection.
An old host handle cannot operate on a subsequently allocated channel.

A confirmed Connection refusal, matched Disconnection exchange or confirmed ACL
link closure marks the channel `BT_L2CAP_CLOSED`. The owner may then call
`bt_l2cap_release_channel`. Release requires every associated receive and
transmit buffer to be released and no partial reassembly on that link.
Timeout alone cannot satisfy those conditions or authorize release.

Released host slots receive fresh wire CIDs. The worker allocates CIDs
monotonically from `0x0040` through `0xffff`, for at most 65,472 channel attempts.
It does not reuse a wire CID within its lifetime. Host generation and packet
serial overflow also cause explicit exhaustion. These finite bounds require
normal recovery or reconnection; indefinite operation is not qualified.
The ACL layer separately retains its four-attempt limit and quarantines
successful handles and attempted peer addresses until controller recovery.

## Identifier rotation separates valid reuse from ambiguous replies

Local requests cycle through identifiers 1–255 in order. Reuse requires a complete
rotation, no pending request or packet with that identifier, and a different wire
CID. Canceling a request before lower submission leaves a hole in the observed
wire rotation. Such cancellation permanently prevents identifier wrap for that
link lifetime; an enqueue does not prove that the peer received an identifier.
An already-started packet continues through its lower ownership and framing rules.

A request records when its last byte entered UART. A response is eligible only
when the worker acquired its first ACL fragment afterward. Link generation,
response type and valid endpoint fields must also match. This is an acquisition
ordering rule; it does not supply physical arrival timestamps or a generation
field absent from the wire protocol.

Core section 4.3 permits invalid CIDs in non-success Connection Responses.
First-use identifiers therefore accept Pending or refusal without checking those
fields. On reused identifiers, such responses and CID-less Command Rejects are
ambiguous and fault with retained ownership. Successful responses still check
the fresh local CID, so an old endpoint cannot complete a new channel request.

The peer request cache has one entry for every nonzero identifier on each ACL
link. An exact duplicate receives the saved response without repeating channel
creation or configuration. If the response is still owned, its existing copy
remains scheduled. A changed request may replace an entry only after every other
identifier has appeared more recently and the previous response has retired.

This rule derives from Core section 4's full-rotation requirement. It assumes
compliant identifier rotation and the ordered ACL transport, without inventing
a packet-age limit. A changed request before full rotation faults and retains
its bytes. Cache entries retain response bytes and host generations independently
of reusable channel slots. Response copies still associated with a channel block
its release until lower ownership ends.

## Bounds are part of the public contract

| Resource | Implemented bound |
| --- | --- |
| Concurrent channel records | Four, reusable after confirmed closure and backing release |
| Wire CIDs | 65,472 per worker lifetime, without reuse |
| Data receive limit | Configurable 48–1,024 bytes; the protocol default is 672 |
| Signaling receive limit | 64 bytes per signaling packet |
| Outgoing signaling commands | At most 48 bytes, compatible with the mandatory peer minimum |
| Configuration option accumulation | 64 bytes per channel |
| Transmit and receive storage | Eight complete packet buffers in each direction |
| Outstanding ACL fragments | Sixteen descriptors, matching the lower transmit pool |
| Cached peer requests | 255 per ACL link, with four link slots and fixed storage |
| Locally issued signaling identifiers | 1–255, with full-rotation and ambiguity restrictions |
| RTX and ERTX | 1–60 seconds and 60–300 seconds, respectively |
| Local retransmissions | Zero for the admitted infinite-flush profile |

Replay storage has 1,020 fixed entries and needs approximately 168 KiB on AArch64,
before other worker fields. No dynamic allocation occurs. Each peer-cache walk
examines at most 255 entries. Local channel or identity exhaustion returns
`BT_L2CAP_EXHAUSTED` without admitting a new operation. Temporary packet-pool
exhaustion applies backpressure. Faulted or uncertain owners require externally
proven recovery before their storage is reused.

The worker validates a signaling packet's entire command-length structure before
processing its first command. Each poll then processes at most one command.
Identifier zero is ignored. Echo and Information requests receive bounded
responses. The advertised optional feature is fixed-channel discovery, whose
bitmap contains only BR/EDR signaling. Echo payloads over 44 bytes receive an
empty Echo Response, which the protocol permits.

## ACL fragmentation keeps controller credits separate

Application sends copy one complete basic-mode data unit into private storage.
The worker prepends its L2CAP length and destination CID, then submits fragments
no larger than the controller's ACL limit. A fragment ledger binds each lower
packet serial to its complete L2CAP packet. Fragment order remains contiguous
on the ACL link.

Only the lower ACL worker returns controller credits. L2CAP Connection,
Configuration and Disconnection responses cannot return them. Application
completion requires every lower fragment to retire its credit. That result
reports controller buffer release; upper-layer acknowledgement is still needed
to establish remote application delivery.

Incoming ACL records must have the exact retained handle/link generation,
point-to-point broadcast flags, and either a start or continuation packet flag.
The worker supports a basic header split across ACL fragments, including a
one-byte first fragment. Unexpected starts, orphan continuations, length overflow
and invalid handle ownership fault without overwriting retained data.

Complete application packets remain in receive storage. Their bytes include the
four-byte L2CAP header; payload begins at `bytes + 4`. Unrelated HCI security,
SCO and ISO records remain separately accessible and do not prevent classified
ACL records from being processed. Full receive storage retains the next lower
record and relies on the established UART backpressure and scheduling contract.

## Faults preserve backing and physical prerequisites

Every poll performs one bounded lower-stack step, processes at most one ACL
record and one signaling command, and admits at most one ACL fragment. Copies
and table walks have the fixed limits above. Malformed ownership, framing loss,
transport deadline expiry and lower faults retain buffers, channel state, packet
serials and fragment leases. Public releases are refused after fault. External recovery
must resolve the lower controller/UART lifetime before backing is reused.

The implementation remains unqualified on the board. It requires completed
Bluetooth firmware loading, real resource ownership, UART generation binding,
task polling and recoverable physical testing. Pairing, key management, SDP,
RFCOMM, HFP and SCO/eSCO headset audio remain required work. Enhanced
retransmission, streaming, connectionless and LE channels are also outside this
basic-mode component's implemented scope.

## Sources and focused evidence

The protocol reference is Bluetooth Core 5.4, Volume 3, Part A:
[basic framing and signaling](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/logical-link-control-and-adaptation-protocol-specification.html).
Section 4 defines identifier rotation and duplicate replies. Section 4.3 defines
non-success endpoint validity; sections 4.4–4.7 define configuration and
disconnection. Sections 6.2.1–6.2.2 define RTX and ERTX, including the
infinite-flush recommendation. These primary rules take precedence where the
inspected kernel path cycles a smaller identifier range or uses a different
configuration continuation strategy.

The published kernel reference is
[linux-orangepi revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f).
Its `include/net/bluetooth/l2cap.h` supplies reference structures, while
`net/bluetooth/l2cap_core.c` provides a behavior comparison. Source hashes and
original notices remain in the external cache. Runtime code is original; the
reference fixture preserves the copied header's license notice.

The [timer and identity model](../checks/bt-l2cap-retry.c) passed with address and
undefined-behavior sanitizers against the actual UART, H4, HCI and ACL workers.
It covers timer transitions, caller expiry, retained credits, cancellation after
confirmed close, host-slot reuse, stale endpoint refusal and full identifier
rotations. The 255-command cases exercise the finite identifier boundary.
A negative Configuration Response under a full transmit pool also stops its
timer while its next action waits for capacity.

The [runner](../checks/bt-l2cap-retry.py) archived executed inputs, executable and
native AArch64 object under
`~/.cache/sunstoneos/evidence/bluetooth-l2cap-retry-20261016/model/`.
Existing channel, fragmentation and wire-byte evidence remains in
`~/.cache/sunstoneos/evidence/bluetooth-l2cap-20261016/model2/`.
The earlier fixture has mechanical API adaptations; its obsolete lifetime-bound
case is replaced by the timer and identity model. It was compiled as a helper
source, and its earlier suite was not executed again. No board qualification or
performance measurements are claimed by these models.
