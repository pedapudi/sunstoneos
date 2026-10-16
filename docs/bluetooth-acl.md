# Bluetooth ACL credits and BR/EDR links

The native Bluetooth link worker implements outbound BR/EDR connection requests,
[incoming peer admission](bluetooth-incoming.md), disconnection and ACL packet
transmission through the [HCI worker](bluetooth-hci.md).
ACL is the controller's asynchronous data transport. Applications supply complete
HCI ACL fragments; a later L2CAP layer must construct and reassemble its packets.
This component does not establish authenticated links or headset audio.

The implementation is in [bt_acl.c](../drivers/bluetooth/bt_acl.c), with its
caller contract in [bt_acl.h](../include/bt_acl.h). One serialized task owns the
link worker, HCI worker and UART. Calls from other tasks or interrupt handlers
require an external scheduling boundary. The UART interrupt remains masked;
the platform must schedule bounded task polls and maintain RTS/CTS flow control.

## A controller generation owns its links and buffers

Initialization requires successful Reset and identity queries, no retained receive
records, and no earlier external command or ACL owner. The queried capabilities
must permit BR/EDR, Create Connection and Disconnect. Read Buffer Size supplies
the ACL fragment limit and controller packet capacity. Packet-based flow control
must remain selected. LE, SCO and other transmitters cannot independently consume
this controller's buffers.

The existing H4 binding must attest completed firmware bootstrap, the configured
UART baud and unchanged firmware/UART generations. Reinitializing these structures
cannot establish a new generation. Recovery must externally prove that the old
controller and UART lifetime has ended before any backing storage is reused.

Each controller lifetime permits four connection attempts. Attempts retain their
slot even when refused, so their addresses, status and handles remain inspectable.
A previously attempted peer address and a successful handle cannot be assigned
again during that lifetime. A repeated peer address is refused. A repeated
handle faults the worker before it becomes usable. This bounded quarantine keeps
late credit events from being assigned to another connection. Explicit reservation exhaustion returns busy without changing the caller's output.
An incoming request without a free or matching reserved slot faults and retains
ownership. Larger capacity or reconnection policy requires a separate change.

Sixteen packet descriptors hold immutable borrowed payloads. Terminal descriptors
remain owned until the caller releases their serial. All payloads, receive slots,
worker objects and output arguments must occupy disjoint memory. The public
structures support inspection; callers must not modify their fields.

## Command acceptance and link completion are separate

Create Connection emits a known peer address, caller-supplied page-scan mode and
clock offset, and the conservative DM1 packet selection. The packet-type word is
`0x3306`: the EDR packet types are disabled and optional BR multislot types are
not requested. The controller always permits DM1 for this command.

The HCI worker reserves a command credit before transmission. Matching Command
Status records return the command slot, while the link remains pending. Only a
successful Connection Complete with the requested address, an ACL link type and
an unused handle makes the link usable. Encryption indicators remain observations;
they do not establish authenticated peer identity or application authorization.

Disconnect emits reason `0x13`, local user termination. A successful Command
Status leaves the link disconnecting until Disconnection Complete. Refused
commands preserve the reported status. An unknown-command error may arrive as
Command Complete instead of Status; a successful Complete cannot substitute for
the required terminal link event.

Explicit outbound and disconnect requests serialize terminal link operations.
Incoming requests may coexist with those operations and each other; HCI commands
remain serialized. Existing connections may continue transmitting while another
connection is being established. A local
disconnect waits for an admitted ACL frame to finish entering the UART before
its command can be transmitted. A remote disconnect during partial frame output
faults and retains that frame. A remote disconnect before a local Disconnect
acknowledgement also retains the uncertain command lifetime.

H4 observes an empty receive FIFO before transmitting, after draining staged and
partial records. Eligibility tags record when bytes were acquired from UART.
Serials and generations are host bookkeeping; HCI events carry neither.
Tags do not timestamp physical arrival or distinguish arbitrary delayed duplicate
responses to a reused opcode. Fresh ownership, ordered HCI events and the absence
of prior outstanding commands remain admission requirements.

## Credits release controller capacity without claiming remote delivery

Each admitted ACL fragment reserves one controller packet credit. The H4 sender
writes its five-byte header and borrowed payload in bounded, separate exchanges.
The packet becomes SENT only when every byte has entered the UART FIFO. That
state does not assert that the UART shift register is empty.

Number Of Completed Packets returns counts for individual handles. The worker
validates the entire event before changing any packet or credit. Every handle
must identify a live retained connection, appear once, and report no more packets
than that connection owns as fully submitted. Counts retire the oldest submitted
packets for each handle. Their result is CREDIT_RETIRED; remote application
delivery still requires a higher-layer acknowledgement.

Successful Disconnection Complete frees the controller capacity of fully
submitted packets for that handle, according to the HCI flow-control rule.
Their descriptors become FLUSHED and retain their payloads for caller inspection.
Queued fragments become NOT_SENT. A partially emitted H4 frame remains owned and
faults the lifetime; disconnect cannot cancel bytes already entering the UART.
Late completion counts for closed or unknown handles fault without returning
credits. No handle can be rebound to another generation in this worker.

Flush Occurred contains no packet count. It therefore returns no credits.
Submitted packets become uncertain, queued fragments become NOT_SENT, and further
ACL submission on that link is refused. Number Of Completed Packets may still
retire the submitted credits. Disconnect or external recovery must end the
flushed link before a new data stream can begin. An explicit HCI Flush command
and L2CAP recovery from automatic flush remain unimplemented.

## Faults preserve every unresolved owner

The caller supplies absolute microsecond deadlines for link operations and
packets. Packet deadlines include the wait for controller credit retirement.
Each step uses the earliest outstanding deadline, performs one bounded HCI step,
processes at most one newly retained record and admits at most one transmission.
The byte budget is 1–64 for each UART direction and parser pass.

Malformed counts, unexpected successful connections, exhausted serial identity,
UART framing loss, clock reversal, generation loss and expired deadlines fault
the lifetime. A fault retains link state, packet descriptors, receive records,
borrowed payload pointers and partial transmit position. Releases are refused
until external recovery resolves their ownership. No reset or retry is implicit.

Unconsumed ACL, SCO, ISO and unrelated events retain their original bytes in H4
receive slots. The public receive operation exposes records only after the link
worker has classified them. A caller can map a raw ACL handle to the immutable
link generation. Full slots apply transport backpressure; the platform must poll
and drain them fast enough to avoid controller or UART overflow.

## Source and validation boundaries

The wire definitions follow Bluetooth Core, Volume 4, Part E: Create Connection
(section 7.1.5), Disconnect (7.1.6), the corresponding completion events (7.7.3
and 7.7.5), Flush Occurred (7.7.17), Number Of Completed Packets (7.7.19), and
packet-based flow control (section 4.1). The
[Core HCI specification](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host-controller-interface/host-controller-interface-functional-specification.html)
and [UART transport specification](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host-controller-interface/uart-transport-layer.html)
provide the protocol contract. The disconnection buffer-release rule also appears
in [Core 5.2, Volume 4, Part E, page 1882](https://community.renesas.com/cfs-file/__key/communityserver-discussions-components-files/297/Core_5F00_v5.2.pdf).

The published kernel reference is revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` of
[linux-orangepi](https://github.com/orangepi-xunlong/linux-orangepi).
Its `include/net/bluetooth/hci.h` defines the packed command/event structures.
Its `net/bluetooth/hci_event.c` separates command Status from terminal events and
accounts for completed ACL packets. Original runtime code implements the protocol;
it contains no copied kernel implementation. The external reference fixture
preserves the kernel header and its original license notice.

The focused [model](../checks/bt-acl.c) composes the real UART, H4, HCI and link
workers. It compares complete command and ACL frame bytes against the pinned
structures. It covers two-handle credit scheduling, retained receive data,
command acceptance, disconnection flush, partial-frame refusal, atomic malformed
counts, uncertain flush, deadlines, handle quarantine and lifetime exhaustion.
The model passed with address and undefined-behavior sanitizers. The changed
H4, HCI and ACL runtime objects also compiled for AArch64. The
[runner](../checks/bt-acl.py) archived executed inputs and artifacts under
`~/.cache/sunstoneos/evidence/bluetooth-acl-20261016/model/`. Existing bootstrap and UART qualification evidence remains separate.

Physical operation still requires firmware bootstrap, native power/pin/clock
ownership, task scheduling and board recovery qualification. Incoming connections,
inquiry, link authentication, encryption/key management, LE, L2CAP, SDP, HFP and
SCO/eSCO headset audio remain required follow-on work. No RF activity or headset
operation is established by the model.
