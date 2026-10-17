# Configure the controller for incoming Bluetooth connections

The HCI worker implements a bounded, one-time configuration sequence for
Bluetooth Classic incoming connections. It sends controller commands through
the actual UART/H4 transport, validates their completions and checks the
readable settings. The [ACL worker](bluetooth-incoming.md) then owns peer
admission. Configuration does not establish pairing or service authorization.

## Firmware and UART ownership precede configuration

The [radio firmware engine](radio-firmware.md) verifies and uploads Bluetooth
ADID data, patches and their configuration. Its start confirmation reports the
published firmware response. It does not prove that the Bluetooth controller
is ready for HCI commands.

The [UART platform binding](uart-platform.md) reserves UART1, PG6–PG9, clocks
and pins, and keeps its GIC source masked. H4 additionally requires a live
binding for completed firmware bootstrap, H4 mode at 1,500,000 baud, hardware
flow control and unchanged firmware/UART generations. The platform must provide
actual Bluetooth reset/wake and task-service authority. Those physical bindings
remain unqualified.

`bt_hci_configure_incoming` follows the six successful Reset and identity-query
commands. It requires fresh command ownership and empty receive storage. No ACL
owner or earlier external command may exist. The worker validates BR/EDR and
every required command capability before accepting the copied configuration.

## Eight commands establish controller settings

Every command has a distinct opcode and requires a matching acquisition tag,
an exact Command Complete return layout and successful status. A successful
Command Status cannot substitute for completion. The sequence stops and retains
ownership after any timeout, transport error, rejected command or wrong readback.

| Command | Required behavior |
| --- | --- |
| Set Event Mask, `0x0c01` | Enable all defined Classic events from Core 5.4; leave reserved and LE bits clear |
| Set Event Filter, `0x0c05` | Clear all filters with the single parameter `0`; this disables automatic acceptance |
| Write Connection Accept Timeout, `0x0c16` | Write the configured timeout in 625-microsecond slots |
| Read Connection Accept Timeout, `0x0c15` | Require the exact written value |
| Write Page Scan Activity, `0x0c1c` | Write the configured interval and window |
| Read Page Scan Activity, `0x0c1b` | Require both exact written values |
| Write Scan Enable, `0x0c1a` | Enable page scan only, value `2` |
| Read Scan Enable, `0x0c19` | Require value `2` before publishing configured state |

The event mask is `0x1dbff807bffb9fff`. It includes link, Classic security and
synchronous-audio events. Command Complete, Command Status and Number Of
Completed Packets cannot be masked by this command. Retaining a security or
audio event does not implement its associated protocol.

Fresh Reset establishes disabled scans before this sequence. Page scan starts
after the event policy and timeout/activity readbacks succeed. Inquiry scan
remains disabled; discoverability, local-name/class policy and pairing modes
require separate implementation.

Timeout values use the controller's mandatory range, `0x00a0` through `0xb540`
slots. Page intervals are even values from `0x0012` through `0x1000`; windows
range from `0x0011` through the selected interval. The worker applies both the
configuration deadline and the existing per-command timeout. It never retries,
resets, disables scan as rollback or discards an uncertain controller lifetime.

## Receive timing remains authenticated through ACL service

The configuration copies a service generation, a receive-latency bound and a
bounded callback that authenticates that bound against the actual firmware/UART
binding. The callback runs at admission and before every subsequent HCI step.
Changed authority faults the worker before UART access. A configured number or
a callback that always returns success is not physical scheduling evidence.

The receive bound starts at controller event emission and ends at actual ACL
request classification. It includes wire/FIFO/staging delays, retained record
backlog, task scheduling, final configuration commands and the ACL handoff.
The provider must establish that complete bound with real task and flow-control
ownership. A native provider for this scheduling guarantee remains required.

Incoming ACL policy is admitted only when:

```
response_timeout_us + receive_latency_us < read_back_accept_timeout_slots * 625
```

The implementation compares differences to avoid overflow. The response bound
includes queued command transmission and terminal connection completion. It
cannot be extended by later polls. Configuration and the receive-time contract
do not authenticate a remote Bluetooth address.

## Preserve requests through the owner transfer

A request can arrive after page scan starts but before final readback completes.
HCI retains its owned record, partial frame or staged bytes. The configured
handoff allows that receive state to remain intact when `bt_acl_init` claims
the controller. Ordinary initialization without configuration still requires
empty receive storage.

All admission checks finish before the HCI owner pointer and ACL object are
published. The serialized task performs both assignments without callbacks or
fallible work between them. HCI records its own address and the exact ACL owner;
byte copies and duplicate ACL claims cannot service the controller.

The caller enables incoming policy and reserves permitted peer addresses before
the first ACL poll. A retained request receives the same peer checks as a later
request. An unreserved peer receives Reject Connection Request. Successful
controller configuration never supplies implicit peer permission, pairing,
encryption or PSM/service authorization.

## Validate configuration and handoff together

The focused check uses actual UART, H4, HCI and ACL code. Controller replies
advertise capabilities through Read Local Supported Commands and complete the
real configuration sequence; fixtures never manufacture configured flags.
It covers complete and partial requests across handoff, accepted and rejected
incoming peers, ACL credits, retained key requests, readback failures, command
ordering, command credits, expired deadlines and lost scheduling authority.

The check compiles HCI, ACL, L2CAP, SDP, RFCOMM, the SDP server and the hands-free
audio gateway against the same headers. Existing incoming fixtures share the
configuration setup helper. Their earlier protocol workloads need no replay
solely because the controller setup changed.

Protocol definitions follow Bluetooth Core 5.4, Volume 4, Part E, sections 6.27,
7.3.1, 7.3.3, 7.3.13–14 and 7.3.17–20 in the
[official specification](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host-controller-interface/host-controller-interface-functional-specification.html).
The check archives the pinned specification, Linux wire definitions, source
inputs, executable, compiler identity and native objects outside the repository.
The focused AddressSanitizer/UndefinedBehaviorSanitizer composition and all seven
native consumer compilations passed. Evidence is retained under
`~/.cache/sunstoneos/evidence/bluetooth-config-20261016/batch1/`.
The earlier incoming ACL and headset workloads were not replayed.
Physical controller configuration and headset operation remain unqualified.

```sh
python3 checks/bt-config.py \
  --source ~/.cache/sunstoneos/sources/bluetooth-hci \
  --spec ~/.cache/sunstoneos/sources/bluetooth-incoming/hci-Core-54.html \
  --output /absolute/new/evidence-directory
```
