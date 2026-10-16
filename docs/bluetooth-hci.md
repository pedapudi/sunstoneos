# Bluetooth H4 transport and controller identification

SunstoneOS implements bounded Bluetooth packet framing and controller
identification over the [A733 UART controller](uart.md). The composed host model
and native compilation pass. Physical Bluetooth communication remains
unqualified. Pairing, connections, profiles and headset audio are not implemented
by this component. This document is for driver and platform developers.

Incoming ACL handling adds Accept and Reject Connection Request to the bounded
post-identification command set. Both require Command Status and a separate
Connection Complete owned by the [ACL admission worker](bluetooth-incoming.md).
Neither command implements pairing or service authorization.

## The worker requires a live radio and UART generation

[bt_h4.h](../include/bt_h4.h) defines transport storage and ownership.
[bt_hci.h](../include/bt_hci.h) defines the controller initialization worker.
One task owns the worker and UART1 throughout their lifetime. The caller supplies
completed radio bootstrap and UART generation identifiers, a verified baud rate
and a callback that confirms those resources remain owned and unchanged.

Admission requires UART1 configured for eight data bits, no parity, one stop
bit and automatic RTS/CTS. Its actual baud must match the binding. The published
radio firmware defaults to 1,500,000 baud, but that value alone does not prove
firmware startup or H4 readiness. The binding provider must establish a fresh
transport with no stale receive bytes or earlier HCI owner.

The worker is task-polled through `a733_uart_exchange`; it does not create a
FreeRTOS task. Keep the UART GIC interrupt masked and schedule bounded worker
polls. The UART controller's interrupt API is separate; this transport does
not consume modem-status or interrupt-identification registers. A future
interrupt-driven binding must preserve the single reader of every register.

The [radio bootstrap](radio.md) loads Bluetooth patch data through SDIO before
the HCI host starts. HCI Reset initializes the running controller; it does not
replace that bootstrap. Radio reset/wake GPIOs, pin and rail leases, shared
Wi-Fi ownership and actual firmware identity remain external prerequisites.
The existing UART1/header route conflict applies throughout the transport lifetime.

## Packet storage remains owned until explicit release

The [Bluetooth UART transport specification][h4-spec] assigns one-byte indicators
to commands, ACL, synchronous, event and ISO packets. HCI is the host/controller
command and data interface. ACL carries asynchronous connection data; SCO/eSCO
carries synchronous data; ISO carries isochronous data.

The receiver accepts controller-to-host event, ACL, SCO and ISO packets.
The packet type is retained separately from the unchanged HCI header and payload.
Header sizes are two bytes for events, three for SCO and four for ACL and ISO.
ISO payload length uses the low fourteen bits of its length field.
The [pinned Linux wire definitions][wire] provide the reference structures used
by the composition check.

The caller supplies two through eight independent storage slots for the HCI
worker. Each slot must hold at least 257 bytes, enough for a maximum event.
The largest accepted slot is 65,539 bytes, enough for a maximum ACL packet
including its header. A smaller selected capacity imposes a local packet limit;
an oversized declaration causes a fault before writing outside the slot.
SCO and ISO storage does not imply negotiated synchronous or isochronous support.

Each step performs an RX-only UART exchange and a bounded parser advance.
A separate TX-only exchange requires an observed empty receive FIFO and no
staged, partial or worker-unprocessed complete packet. Exhausting the receive
budget does not prove the FIFO empty. Application-owned records allow transmission
when another free slot permits that observation.
The byte budget is one through 64 for receive, transmit and parsing separately.
A partial packet retains its slot and deadline. Complete records carry unique
serials; a release must match the owned record's serial. Consumed serials cannot
release later packets in the same lifetime.

Unsolicited events and ACL/SCO/ISO packets become application-owned records.
The worker can process later command events while those records remain held.
When every slot is occupied, staged bytes remain intact and FIFO reads stop.
Automatic RTS/CTS provides hardware backpressure, but scheduling and prompt
record consumption remain necessary to avoid overflow or command timeout.

Line errors, an invalid direction/type indicator, oversized lengths and partial
packet timeouts cause terminal faults. The transport does not scan payload bytes
for a new packet indicator. A lost generation or uncertain UART transfer also
faults the worker. All packet slots, partial cursors and pending transmit bytes
remain retained. No later step accesses UART, and release is refused after fault.
External recovery must resolve the radio and UART before constructing a new lifetime.

## Command credits and completion have separate meanings

The [HCI functional specification][hci-spec] gives Command Complete and Command
Status distinct roles. Status can acknowledge acceptance without completing
an operation. Their command-credit field limits further submissions; opcode
zero supplies an independent credit update and identifies no submitted command.

Initialization serializes one command at a time, starting with one credit.
It consumes a credit when a command is queued and retains any partial UART
transmit cursor. A matching successful Status records acceptance and continues
waiting. Only the matching successful Complete, with the expected return
length, advances the initialization sequence.

The worker performs these operations once per admitted lifetime:

| Command | Opcode | Successful return bytes, including status | Retained result |
| --- | --- | --- | --- |
| Reset | `0x0c03` | 1 | Reset completion bit |
| Read Local Version Information | `0x1001` | 9 | HCI/LMP versions, revisions and manufacturer |
| Read Local Supported Commands | `0x1002` | 65 | Command bitmap |
| Read Local Supported Features | `0x1003` | 9 | Feature bitmap |
| Read Buffer Size | `0x1005` | 8 | ACL/synchronous lengths and packet counts |
| Read BD_ADDR | `0x1009` | 7 | Controller address |

These commands also appear in the published [controller initialization source][init].
The SunstoneOS sequence identifies the expected dual-mode controller; a refused
or unsupported query faults initialization and retains its response.
IDENTIFIED means every listed command completed and its result was captured.
It establishes no connection, security state, audio path or authenticated device identity.

Each UART receive batch carries the command eligibility tag active when its
bytes arrived. Packets keep that tag through staging and assembly. Already-acquired bytes cannot satisfy a later command merely because their
dispatch occurred later. FIFO reads provide no physical arrival timestamps; bytes
arriving between an empty observation and transmission cannot be ordered by this
mechanism. Fresh ownership with no earlier outstanding commands and unique
initialization opcodes remain required admission guarantees. No opcode is reused during this
initialization lifetime, and timeout prevents further command submissions.

Mismatched or stale command responses remain application-owned events. They
neither finish the current command nor grant it new credits. Valid opcode-zero
updates affect credits without completing a command. Successful Command Complete
with zero credits can finish its command while leaving the next command waiting.
The total initialization deadline also bounds that wait.

Disconnect Complete is retained independently of query completion. This worker
has no connection table and does not infer a link state from the event.
A future link owner must correlate handles, resolve handle reuse and apply
security/profile teardown. Unknown events remain available for that owner.
Hardware Error, malformed command events and command failure retain the event
and fault the current lifetime.

## Evidence covers transport composition

The [focused runner](../checks/bt-hci.py) extracts wire structures from the pinned
Linux header into the external evidence directory. Its model uses the actual
UART runtime, compares complete command bytes and supplies replies using those
reference structures. AddressSanitizer and UndefinedBehaviorSanitizer cover
fragmentation, credits, Status/Complete ordering, stale replies, retained data
records, pool backpressure, generation loss, timeout and uncertain transmission.
A focused scheduling case verifies receive-drain ordering, progress with retained
packets and acceptance of immediate replies after transmission. Both new runtime
files compile as freestanding AArch64 C.

The runtime parser and worker are original Apache-2.0 implementations of the
Core wire formats. Linux supplies separately extracted reference structures for
the model; no Linux function body or implementation expression is copied into
the runtime. The external reference source and fixture retain the Linux notices.
BTstack and its license are not adopted by this component.

Evidence is under `~/.cache/sunstoneos/evidence/bluetooth-hci-20261016/`.
The prior UART and SDIO suites were not replayed. This model establishes software
composition; it does not establish a working radio or measure throughput.

Physical qualification requires independent watchdog recovery, verified radio
firmware, native resource binding and a coordinator-owned serial test.
Link management and ACL credits are implemented by the [ACL worker](bluetooth-acl.md);
upper protocol workers provide L2CAP, SDP and RFCOMM. Further implementation
includes interrupt-driven scheduling, SCO/ISO credit accounting, security and
headset voice transport. LE and ISO capability queries and negotiation remain
required before using those features.

[h4-spec]: https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host-controller-interface/uart-transport-layer.html
[hci-spec]: https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-60/out/en/host-controller-interface/host-controller-interface-functional-specification.html
[wire]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/include/net/bluetooth/hci.h
[init]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/net/bluetooth/hci_sync.c
