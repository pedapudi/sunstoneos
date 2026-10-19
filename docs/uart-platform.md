# A733 UART platform binding

The UART platform binding acquires real clock and pin leases, controls the local
clock gate and reset, masks the selected GIC interrupt, and configures the native
UART controller. It provides task-polled serial service for one designated
FreeRTOS task. The host composition and native compilation establish software
behavior; physical serial operation remains unqualified.

This document is for platform and driver developers. The public interfaces are
[a733_uart_platform.h](../include/a733_uart_platform.h) and
[a733_uart_platform_native.h](../include/a733_uart_platform_native.h).
The [controller contract](uart.md) defines byte transfer, errors and drain.

## Routes use the shared pin manager

The route planner provides the following selections. Pins appear in TX, RX,
RTS, CTS order. Two-pin operation omits RTS and CTS from the lease.

| Controller | Pins | Mux function | Automatic flow control |
| --- | --- | --- | --- |
| UART0 | PB0, PB1 | 3 | No |
| UART0 | PB9, PB10 | 2 | No |
| UART0 | PF2, PF4 | 3 | No |
| UART1, Bluetooth | PG6, PG7, PG8, PG9 | 2 | Yes |
| UART1, header | PE12, PE13 | 8 | No |
| UART2 | PB0, PB1, PB2, PB3 | 2 | Yes |
| UART3 | PH4, PH5, PH6, PH7 | 2 | Yes |
| UART4 | PG6, PG7, PG8, PG9 | 3 | Yes |
| UART5 | PH13, PH14, PH15, PH16 | 2 | Yes |
| UART6 | PE0, PE1, PE2, PE3 | 8 | Yes |
| S_UART0 | PL2, PL3 | 3 | No |
| S_UART1 | PL2, PL3 | 2 | No |

The manual pin-function tables define these routes. A route describes a SoC
capability; it does not establish available board wiring. The platform approves
bank voltage, drive strength, pull state and ownership before admission.
UART1 Bluetooth and header routes reserve the same controller exclusively.
UART6 and SPI3 overlap PE pins and cannot hold conflicting leases.
The binding uses the same bank-qualified pin manager as I2S and GPIO clients.
The existing PB API and its ownership semantics remain applicable.

Expected inherited pin functions may be input, disabled, or the selected UART
function. An inherited GPIO output is refused. Every desired function must
match the route; the caller supplies approved pull and drive values.
The binding does not program bank supply, security or retention voltage.

## Clock admission proves an inherited 24 MHz source

UART0–6 use main CCU base `0x02002000`, APB_UART offset `0x538`, and separate
local words at `0xe00 + 4 × port`. Each local word uses gate bit 0 and reset
bit 16. The binding admits source zero and divider zero only.
APB_UART source zero is SYS_CLK24M. The binding validates the normalized PLL reference with the shared
`a733_clock_reference.h` decoder. A raw 19.2 or 26 MHz oscillator is accepted
when its enabled, locked reference factors produce 24 MHz.

S_UART0–1 use R_CCU base `0x07010000` and APBS1 offset `0x10`.
Their shared local word at `0x18c` uses gate bits 0/1 and reset bits 16/17.
APBS1 source zero is DCXO directly. Source four is SYS_CLK24M in the manual;
the published clock source names alone do not establish its rate.

DCXO admission requires enable and stable indications at `0x07090160`.
S_UART source zero additionally requires a raw 24 MHz oscillator. Main UART
admission requires enabled, locked PLL_REF whose decoded multiplier and divider
normalize the oscillator to 24 MHz. Every register stage rechecks this profile.
The registry holds the bus and its reference ancestry stable while reserving
the selected controller exclusively. Separate UART controllers can coexist.
Parent switching and cold PLL startup require separate implementations.

The widened registry uses `a733_clock_mask_t` throughout its API and tokens.
All embedding consumers must rebuild together; an older 32-bit callback or
output pointer is incompatible. Link the complete registry change and the
bank-qualified pin implementation, including its pin table.

## Startup and close retain authority until completion

Initialization copies configuration and performs no MMIO. Polling first acquires
the clock token and authenticates platform authority. The admission callback
must prove a quiescent peer and exclude console, remote firmware, DMA, pending
handlers and other software users. It also authenticates pin electrical state,
Device mappings, register accessibility and independent recovery.

The local gate and reset must initially be clear. The binding masks its GIC
interrupt, observes completed distributor writes, and requires the selected
source disabled and inactive. It then acquires the real pin lease, enables the
gate while reset remains asserted, and holds reset for at least ten microseconds.
This conservative software interval is not a claimed hardware minimum.
After reset deassertion, the existing UART controller performs bounded divisor,
FIFO and flow-control configuration.

The GIC interrupt stays masked throughout this implementation. Main UART INTIDs
are 34–40; S_UART INTIDs are 233–234. Native writes affect only the selected
interrupt bit. No interrupt handler may service the borrowed UART.
The exclusive controller lease also covers its interrupt authority, while
physical handoff must exclude handlers outside the registry.

The owner task borrows the configured controller for existing protocol workers.
Before each service operation, its guard calls `live` with an absolute deadline.
That call verifies the actual clock token and pin ownership, then checks pin
manager health under the common register mutex. Pin ownership remains queryable
after a manager fault, but failed manager health refuses live service.
A borrowed controller cannot be restarted, reconfigured or drained directly.

Close requires stopped submissions, a quiescent peer and authenticated handler
exclusion. It masks and synchronizes the GIC source again, then drains the UART.
Completion requires empty RX and TX FIFOs, an empty shift register and an idle
controller. Unread RX data prevents teardown. The binding asserts reset with the
gate enabled, waits ten microseconds, removes the gate, clears only its pending
GIC bit, restores its pins and finally releases its clock token.

Uncertain or late access produces a terminal fault. The binding retains every
unreleased real token and retains an owned register mutex when uncertainty
occurs inside it. Fault polling only completes registry retention bookkeeping;
it performs no recovery or further MMIO. Successful pin release remains final
if a later clock step fails. Successful clock release is the final completion
point and has no post-release callback that could fabricate retained ownership.
External recovery must resolve hardware state before object reuse.

## Native access uses the shared lock and actual task identity

The native adapter checks the running EL1 FreeRTOS task, scheduler state,
interrupt nesting, critical nesting and IRQ-mask state. Clock, GIC and pin
writers use the same scheduling-aware register mutex. Exclusive owner-task
UART FIFO service does not acquire that global mutex for every byte.
The adapter orders Device accesses with barriers, restricts addresses to the
selected controller, and preserves other bits in shared local clock words.

Callbacks must be bounded and non-reentrant. They use one monotonic microsecond
clock and cannot invoke the interrupted binding or its borrowed controller.
Configuration, callback storage, tokens and managers remain trusted and live
for the binding lifetime. These contracts provide cooperative ownership;
they do not isolate hostile privileged tasks.

The native FreeRTOS configuration must enable scheduler-state, current-task and
mutex-holder APIs. Both single-core and SMP task-state branches are compiled.
A software deadline cannot interrupt a stalled peripheral transaction, so
independent watchdog recovery remains a prerequisite for board execution.

## Validation composes the real software components

The [focused runner](../checks/a733-uart-platform.py) links the actual UART
controller, shared pin manager and widened clock registry to modeled registers.
It checks configuration and byte transfer, conflicting UART1 routes, coexistence
of separate controllers, parent stability, shared S_UART word preservation,
GIC masking, shift-register drain, completed release and retained faults.
It also checks pin-manager health refusal and a late live transmit access.
The native address restrictions cover every selected route.

The runner pins source hashes, checks local clock declarations, executes the
composition once with AddressSanitizer and UndefinedBehaviorSanitizer, and
compiles the native adapter for single-core and SMP configurations.
Compiler dependency files identify the exact headers preserved with evidence.
The accepted composition and native objects are under
`~/.cache/sunstoneos/evidence/uart-platform-20261016/checked-2/`.
Its manifest preserves 87 inputs. The composed executable SHA-256 is
`1b9aee7e234d157ca6046608b681fc14d6e11ee23e1f02d1f9bf0d033f7b37dc`.
The runtime check passed once; native compiler setup corrections did not repeat it.
Existing UART format/FIFO/interrupt, pin-manager and I2S evidence remains valid;
this binding does not rerun those component workloads.

The [finite header application](uart-application.md) integrates UART2 with a
passive PB0/PB1 loopback, copied USB output and complete UART teardown.
The [native Bluetooth application](bluetooth-native.md) integrates this polled
UART binding with module bootstrap and H4 transport. Physical admission and
serial peer tests remain required. Interrupt-driven platform service, alternate
parent profiles, DMA, RS-485, IrDA, nine-bit modes, runtime format changes and
suspend/resume also remain required. Modeled GIC observations do not qualify
physical interrupt behavior or reset timing.

The source basis is A733 User Manual V1.00 sections 4.1, 4.2, 9.1.7.16,
18.10 and the pin-function tables in 18.14; the board schematic V1.2 supplies
header and Bluetooth wiring. The published kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` supplies the matching main and
always-on clock declarations, SoC device tree and pin-controller tables.
The runner records exact document and source identities in its evidence.
