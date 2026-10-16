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

## Bounds are part of the public contract

| Resource | Implemented bound |
| --- | --- |
| Channel attempts | Four local CIDs per worker lifetime, beginning at `0x0040` |
| Data receive limit | Configurable 48–1,024 bytes; the protocol default is 672 |
| Signaling receive limit | 64 bytes per signaling packet |
| Outgoing signaling commands | At most 48 bytes, compatible with the mandatory peer minimum |
| Configuration option accumulation | 64 bytes per channel |
| Transmit and receive storage | Eight complete packet buffers in each direction |
| Outstanding ACL fragments | Sixteen descriptors, matching the lower transmit pool |
| Cached peer requests | Sixteen distinct requests per worker lifetime |
| Locally issued signaling identifiers | 1–255 per ACL link, without reuse |
| Peer-operation bound | Configurable 1–60 seconds, shortened by an explicit caller deadline |

Channel slots and remote CIDs are quarantined for this lifetime. Closed channels
remain inspectable. The ACL layer additionally quarantines successful handles
and attempted peer addresses. Reinitializing structures cannot establish a new
controller generation or make an old identity reusable.

Local channel or identifier exhaustion returns `BT_L2CAP_EXHAUSTED` without
admitting a new operation. Exhausting the peer request cache faults with the
separate capacity reason and retains the unprocessed packet. Temporary packet
pool exhaustion applies backpressure. These bounds support a limited set of
concurrent channels; repeated reconnection and sustained signaling require a
broader identity/cache policy before they can be claimed as supported.

The peer-operation bound is an ownership deadline. Pending responses do not
extend it, and no automatic retransmission occurs. Expiry faults and retains the
operation. Full RTX/ERTX retry timing, including an extended pairing wait, remains
future work; this implementation does not claim complete timeout conformance.

## Signaling identities and retained replies prevent duplicate side effects

Each local request has a unique identifier and records when its last byte entered
UART. A matching response is eligible only when its first ACL fragment was
acquired afterward. The worker also checks the link generation, response type
and channel endpoints. Early, duplicate and unmatched responses cannot create
channels or complete another request.

A peer request cache retains the complete command and its response. An exact
duplicate receives the cached response without allocating another channel or
reapplying configuration. A duplicate arriving while the original response is
still owned does not enqueue a second copy. The original remains scheduled.
Changing a cached request while reusing its identifier faults the lifetime.
The cache exhausts before a compliant peer can cycle through every identifier.

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
expired deadlines and lower faults retain buffers, channel state, packet serials
and fragment leases. Public releases are refused after fault. External recovery
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
Sections 4.4–4.5 define configuration continuation; section 4 defines identifier
reuse. These primary rules take precedence where the inspected kernel path uses
a different continuation strategy or cycles a smaller identifier range.

The published kernel reference is
[linux-orangepi revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f).
Its `include/net/bluetooth/l2cap.h` supplies reference structures, while
`net/bluetooth/l2cap_core.c` provides a behavior comparison. Source hashes and
original notices remain in the external cache. Runtime code is original; the
reference fixture preserves the copied header's license notice.

The [composed model](../checks/bt-l2cap.c) passed with address and
undefined-behavior sanitizers. It uses the actual UART, H4, HCI and ACL workers,
compares full protocol bytes, and exercises setup in both directions, fragment
credit recycling, split-header receive and retained security events. It also
covers duplicates, early responses, close, unknown options, malformed command
lists, bounded deadlines and request-cache exhaustion.

The native L2CAP object compiled for AArch64. The
[runner](../checks/bt-l2cap.py) archived executed source and artifacts under
`~/.cache/sunstoneos/evidence/bluetooth-l2cap-20261016/model2/`. An earlier compile
stopped at a reference-header ordering error in the fixture; no runtime executed
before that correction. Existing lower-layer evidence was reused without
replaying its separate suites. No board or performance measurements were run.
