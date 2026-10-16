# Incoming Bluetooth ACL connections

The [ACL worker](../drivers/bluetooth/bt_acl.c) accepts explicitly reserved
BR/EDR peers and rejects unreserved peers through actual HCI commands. BR/EDR
is Bluetooth Classic; ACL carries its asynchronous data. This implementation
provides connection admission. Pairing, authenticated peer identity and headset
service authorization require their security owners.

## Reserve a peer before its request arrives

One serialized task owns ACL, HCI and UART state. After fresh controller
identification, `bt_acl_incoming` enables incoming handling once and records a
response timeout in microseconds. The controller must advertise Accept and
Reject Connection Request support. Enabling requires idle command ownership,
no retained receive records and no earlier link attempt.

`bt_acl_listen` copies a nonzero peer address and returns its reserved link
generation. The absolute deadline covers waiting for the request and completing
the connection. The local controller address and every previously reserved or
attempted peer are refused. Caller address storage may change after return.
The reservation consumes one of four lifetime attempt slots immediately.

A matching ACL Connection Request copies its class of device and record serial
into that slot. The worker sends Accept Connection Request with role `1`, which
requests remaining peripheral. A successful Command Status acknowledges the
command. Only the subsequent successful Connection Complete, with the matching
address and an unused ACL handle, makes the reserved generation connected.

Peer addresses and class information are unauthenticated. A reservation records
permission to establish a baseband connection; it supplies no pairing result.
The connection event's encryption field is an observation. Link Key Request and
other security events remain owned for the actual security implementation.
[L2CAP](bluetooth-l2cap.md) and [RFCOMM](bluetooth-rfcomm.md) continue requiring
authorization for the service and current link generation.

## Rejection retains an attempt until its terminal event

An unreserved ACL request consumes a free attempt slot and sends Reject
Connection Request with reason `0x0e`, security rejection. A request using the
local controller address receives `0x0f`, unacceptable address. Both commands
require Command Status followed by Connection Complete. A successful Reject
status leaves the slot pending until a failure Connection Complete arrives.
An unexpected successful connection after rejection faults the worker.

A refused command closes the attempt while retaining its status. An
unknown-command error may use Command Complete; a successful Command Complete
cannot replace Command Status or Connection Complete. The HCI command slot and
ACL command owner must both finish processing the acknowledgement before another
queued command is admitted.

Incoming requests can coexist with an outbound connection attempt and with
other incoming attempts. HCI commands remain serialized. Addresses distinguish
terminal link events, and successful handles remain unique for the controller
lifetime. Existing links can continue ACL transmission and retire controller
credits while connection commands progress.

Every attempt retains its slot, address and generation after refusal or
disconnection. A repeated address, an incoming/outbound address collision,
exhausted incoming capacity or uncertain terminal event faults and retains the
controller lifetime. Timeout also retains ownership. There is no automatic
cancellation, reconnect, reset or slot reclamation. Fresh capacity requires
qualified external recovery and a new controller/UART generation.

Valid SCO and eSCO requests remain retained for the synchronous audio owner.
They do not consume ACL attempt slots. Malformed requests fault the worker.
Copied ACL requests release their receive storage so acknowledgements and credit
events can continue through the bounded H4 receive pool.

## Controller configuration and pairing remain prerequisites

The platform owner must configure page scan, unmasked Connection Request,
Command Status and Connection Complete events, and disable automatic connection
acceptance. This component does not issue those configuration commands.
The controller's connection accept timeout must exceed the bounded UART receive
and classification latency plus the configured response timeout.

The host response deadline begins when the request is classified and is capped
by the reserved peer's absolute deadline. It bounds the entire local operation,
including terminal completion. It does not measure the request's physical
arrival time or change the controller timer. Scheduling, RTS/CTS and prompt
security-event consumption remain platform obligations.

Record tags describe UART acquisition eligibility. They reject statuses already
acquired before command transmission. Bluetooth HCI carries no host generation;
tags cannot distinguish arbitrarily delayed duplicate responses on the wire.
Controller recovery and ordered event delivery remain required assumptions.

## Validation uses the actual UART and link workers

The focused check composes the actual UART, H4 parser, HCI command worker and ACL
worker. Its wire structures come from the pinned Linux HCI header. The protocol
sequence follows Bluetooth Core 5.4, Volume 4, Part E, sections 7.1.8, 7.1.9, 7.7.3 and
7.7.4 in the [official HCI specification](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host-controller-interface/host-controller-interface-functional-specification.html).
The published specification and reference header stay outside the repository;
the check verifies their hashes and archives them with its inputs.

The check covers accepted traffic and credit retirement, rejection and command
refusal, concurrent connection attempts, stale and premature events, retained
security and synchronous requests, handle quarantine, exhaustion and deadlines.
The composed check passed with AddressSanitizer and UndefinedBehaviorSanitizer.
The changed HCI and ACL sources also compiled for AArch64. Inputs, executable,
objects and logs are retained under
`~/.cache/sunstoneos/evidence/bluetooth-incoming-20261016/batch1/`.
These results establish software composition; physical headset operation remains
unqualified.

```sh
python3 checks/bt-acl-incoming.py \
  --source ~/.cache/sunstoneos/sources/bluetooth-hci \
  --spec ~/.cache/sunstoneos/sources/bluetooth-incoming/hci-Core-54.html \
  --output /absolute/new/evidence-directory
```
