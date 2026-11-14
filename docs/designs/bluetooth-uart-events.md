# Bluetooth H4 and its native owner use deferred UART interrupts

Status: implemented and host-tested for UART/H4, active protocol deadlines and
the native OS owner binding. Physical routing, receive latency and headset
operation remain unqualified. No headset is attached. The standalone module's
SDIO bootstrap still uses a precise observation cadence until its SDIO event
provider is composed with this owner.

The transport opts in once with `bt_h4_irq_enable` immediately after HCI
initialization and before any command or interrupt unmask. The Bluetooth
application selects this path when its authenticated UART configuration has
`deferred_irq=1`. The native OS binding additionally requires
`SUNSTONE_A733_OS_HEADSET_IRQ=1`. Polling configurations retain their existing
behavior. The [UART platform](../uart-platform.md) supplies ownership, mask
propagation and handler synchronization.

## The native owner masks and synchronizes before UART service

UART1 uses GIC interrupt 35 and the PG6–PG9 Bluetooth route. Boot code installs
the source's group, level, priority and route before provider startup. The
platform starts with that source masked. The OS exception dispatcher calls
`os_headset_uart_irq` and owns acknowledgment and deactivation.

The handler masks interrupt 35, preserves the earliest pending timestamp and
notifies the actual owner task. It performs no UART access. Without a separate
radio grant, the Bluetooth task owns UART1 and the module. With a shared radio
grant, interrupt 35 notifies the existing radio owner that calls
`os_module_poll`; it does not create another owner task.

The task masks the source for software requests too. Before device service it
calls `a733_uart_platform_irq_sync` to prove disable propagation and handler
completion. Pending synchronization and rearm use precise retry deadlines
spaced by one eighth of the admitted post-interrupt receive budget. Their fixed
operation deadline cannot move later on successive attempts. A successful
rearm must also finish before that bound.

A bounded H4 pass classifies one IIR cause through
`a733_uart_interrupt_rx_first`. Receive data, line status and receive-timeout
causes preserve existing byte budgets and per-byte error flags. The owner
rearms under the same critical section as interrupt publication, only with no
pending publication and `bt_h4_irq_ready` true. A full record pool keeps the
source masked until actual software progress or consumer release.

`bt_h4_irq_ready` describes receive capacity and staging only. Received bytes
below the FIFO trigger do not prohibit rearm: the receive timeout must remain
able to notify the owner. The predicate does not establish GIC synchronization,
a stopped peer, completed transmit data or retirement.

## Short receive bursts precede command transmission

UART1 has a 256-byte receive FIFO with a half-full interrupt trigger. An IIR
value of no interrupt or transmitter empty therefore does not prove that RX
is empty. The controller classifies IIR before observing USR.RFNE, bit 3.
The USR observation preserves RBR bytes and LSR error indications, but also
acknowledges BUSY. Reading it before IIR could hide the existing BUSY fault.

The owner's separate FIFO-empty query reads the receive level register,
UART_RFL at offset `0x84`, bits 8:0. It requires an enabled FIFO and preserves
IIR, BUSY, LSR and received bytes. An upper configuration or deferred provider
step can return without an H4 IIR pass, so this query must preserve BUSY until
the actual interrupt classifier observes it. Both native address filters
permit reads of RFL and refuse writes.

On transmitter-empty service, pending RX withholds TX and disables its
interrupt. A receive-timeout interrupt ends a short burst. After the owner
drains RX, completes framing and releases records, the transport rearms TX
when its eligibility predicates permit it. Header completion rearms the
remaining ACL or SCO payload. Counts advance only the accepted suffix;
uncertain writes retain the frame and prohibit retransmission.

Command, ACL and SCO publication arms transmitter-empty interrupts directly.
A command created after the transport pass can therefore produce its next
owner event. No scheduler tick is needed to discover it.

## Protocol metadata determines additional software work

The native owner compares exact metadata before and after one bounded pass
using `bluetooth_application_work_state`. The snapshot contains protocol
phases, queue ownership and publication serials, cursors and credits.
It excludes payloads, keys, historical replay tables, statistics,
clock observations and the owner heartbeat.

Each snapshot occupies 7,232 bytes. Two snapshots add 14,464 bytes to the
existing ledger allocation. They do not occupy the owner's stack. The native
binding's largest individual compiled stack frame is 256 bytes; that figure
does not include its callees. The owner retains its 32 KiB stack.

Changed metadata gives the owner a software notification for another bounded
pass. Unchanged blocked metadata causes no repeated notification. Pending TX
suppresses that software notification only when THRE is enabled and the GIC
source is actually unmasked. Receive capacity alone cannot establish that TX
has an interrupt: unclassified RX records can still keep THRE disabled.

Accepted segment requests, consumer acknowledgments and the first stop request
notify the deferred owner after publication. Repeated or refused requests do
not. This permits an idle owner to block while a consumer holds a record.

## Active protocol timers supply absolute wake deadlines

`bluetooth_application_next_deadline` reads owner-private state and returns the
minimum of the caller's fixed bound and active timers. It performs no device
or time-source access. Identification and headset modes 1–3 are supported;
playback and A2DP require separate producer cadences and are refused.

The query covers application and HCI startup/stop, HCI commands, H4 partial
frames and TX, security lifetime and commands, ACL attempts and packets,
L2CAP channel/RTX/ERTX/reassembly/packet bounds, SDP query/response/close,
RFCOMM setup/control/data/close, HFP setup/line/codec/response, SCO setup/data/
refusal/stop, capture windows and silence cadence. Terminal states exclude
retained stale deadlines. A full SCO transmit pool resumes on credit events;
a past silence slot alone cannot make it runnable repeatedly.

The standalone owner waits on the minimum protocol, receive, control and
session deadline through `sunstone_deadline_wait_until`. A due result requires
the ordinary protocol pass to classify expiry. In shared mode the pure
`os_module_next_deadline(caller_limit)` supplies the same minimum to
`board_radio_owner_wait(r, caller_limit)`. The radio owner supplies its own SDIO
and network event sources. The deadline query cannot publish a service-health
certificate or prove that hardware drained.

Before H4 is active, the standalone module bootstrap observes its existing
bounded SDIO interface at exact 1,000 µs deadlines. That intermediate path is
not covered by UART interrupts. The steady UART path has no periodic fallback.

## The receive guard preserves the authenticated total bound

The HCI scheduling contract starts at controller event emission and ends at
actual ACL request classification. Its authenticated scheduling authority
covers pre-wire queuing, wire time, receive triggering, interrupt dispatch,
framing and COMPLETE/OWNED backlog. The profile scalar alone is not physical
evidence of this bound.

The local guard reserves a source-derived interval before the first interrupt.
UART1 is 8N1 with a 128-byte trigger. If successive characters arrive just
under the four-character timeout, the trigger can be delayed by nearly four
character times per byte. The guard therefore reserves
`ceil(4 × 128 × 10 × 1,000,000 / actual_baud)` microseconds. At 1,500,000 baud
that reserve is 3,414 µs. A 132-character, 880 µs allowance covers continuous
bursts and short-burst timeout, but does not cover that arrival spacing.

Admission refuses a receive-latency bound less than or equal to the reserve.
The remaining positive interval bounds the earliest pending interrupt through
classification. Repeated interrupts and software wakes cannot refresh its
start. The owner clears that timestamp only when the UART FIFO is empty and
`bluetooth_application_rx_unclassified` reports no staged, partial, complete or
unclassified owned record. Remaining wire bytes, framing passes and backlog
remain within the measured post-interrupt age.

The local guard supplements the existing authenticated total-budget authority.
It does not independently establish the controller's pre-wire queue delay or
physical scheduling behavior. The legacy elapsed-owner check applies to
polling mode. Deferred mode permits idle time without traffic and uses this
interrupt-to-classification guard. Backward time remains a fault in both modes.

## Stop preserves the existing retirement proof

The registered quiesce callback closes upper admission and notifies the
original owner. The owner masks and synchronizes the source, completes the
accepted protocol shutdown and supplies the existing authenticated peer-stop
proof before UART close. The platform drains both FIFOs, the shift register
and controller activity before reset, gate removal, pin restoration and clock
release.

UART TEMT observations and local reset holds lack completion interrupts. Their
existing bounded close operation advances through precise deadlines under the
fixed return deadline. A timeout or empty software queue cannot replace its
retirement proof. Faults mask the source and retain every unreleased resource;
subsequent owner wakes perform no further device service.

The generated image table allocates one Secure Device return snapshot for each
selected interrupt route. The snapshot array and its capacity derive from the
route array itself, including UART1 and optional GPU or USB routes. Save refusal
invalidates the saved count before any later restore can use it. This preserves
the final return barrier's prohibition on writes to Normal memory.

## Host checks cover idle waits and retained failure

The [transport check](../../checks/event-uart-h4.py) executes the actual
UART/H4, FreeRTOS kernel and precise timer with modeled UART and interrupt
effects. It records zero wakes during one idle second and receives a burst on
an interrupt at a 123 µs phase. Seven transport scenarios cover burst ordering,
suffixes, backpressure, deadlines, faults and a FIFO query with pending BUSY.
Forty-one active/retired timer pairs exercise the application query. Five negative controls restore specific
polling or timer defects and fail their associated cases.

The [native binding check](../../checks/os-headset-uart-events.py) executes the
actual OS owner, FreeRTOS kernel, deadline backend and quiesce registry. Its
UART/GIC and application boundaries are modeled. It covers idle, short receive
timeout, copied upper work, software RX backlog before TX, blocked credit,
synchronization/rearm delay and failure, arm races, earliest timestamp,
classification expiry, upper deadlines, stop and the shared radio owner.
Its software-only upper-pass case executes the actual FIFO query and UART
interrupt classifier, preserving BUSY until the following interrupt faults.
Targeted negative controls restore the corresponding defects. Its admitted
5,000 µs test profile leaves 1,586 µs after the receive-trigger reserve.
The standalone active-H4 owner records zero idle wakes per second; the precise
polling profile wakes 1,000 times per second. These host figures do not measure
physical WFI residency or receive latency.

The [wire admission check](../../checks/os-headset-uart-profile.py) starts with
a modeled immutable external profile. It validates the frozen bytes through
the Bluetooth preparation checks and the OS asset adapter, then loads the
adapter's exact upload plan into the native image emulator. The actual native
identity copy, profile validators, SHA-256 implementation and UART plan execute;
only coherence publication is modeled inside that entry. Board EL3 startup and
whole-session admission do not execute. Native `offsetof` and `sizeof` supply
the input layout, including the deferred-mode field.

The wire cases cover deferred mode one, a valid field change with stale and
updated identities, deferred mode zero with a new identity, and the 3,414 µs
budget boundary. Deferred mode one reaches publication with either matching
identity; each invalid input leaves the native copy unavailable.
The host's typed session producer cannot yet grant a headset
session; this external-input check does not implement that producer or supply
physical scheduling authority.

The [UART reference](../uart.md) pins A733 manual V1.00, section 18.10, and
Linux BSP revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.
`sunxi-uart-ng.h` has SHA-256
`20d584972c8d32a2f48f19af15a182f371571be16dc497cbd6a12cd88747c84a`.
The manual's IIR table defines receive timeout after four character times
without received data or an RBR read, with data still in the FIFO; sections
18.10.9.13 and 18.10.9.15 define USR and the read-only receive FIFO level.
These cached references establish modeled register
effects. Board timing, physical interrupt routing, headset interoperability
and fault recovery remain unqualified.
