# Legacy USB reset holds block on the virtual timer

Status: implemented and host-tested. The selected `a733-usb-runtime` target
builds. Physical timing, interrupt delivery and return remain unqualified.

The USB runtime's only task registers one precise-deadline owner before
TinyUSB initialization. Detach, reset assertion and propagation holds block
until an absolute counter deadline. The target links the existing
`runtime/deadline.c` and single-core A733 backend. No separate timer scheduler
is introduced. Early notifications recheck the counter and preserve the same
hold deadline.

Each hold retains its existing minimum in 24 MHz counter ticks, including the
extra raw tick that covers initial sampling phase. Its microsecond deadline
rounds that target upward. The 150 ms detach, 1 ms reset assertion and 40 µs
propagation minima therefore remain satisfied without a counter spin. MMIO
ordering and the register sequence are unchanged. Overflow, counter reversal
and a failed deadline wait preserve the panic path.

## Virtual-timer ownership has admission and restoration checks

Only the legacy USB target selects `SUNSTONE_USB_PRECISE_DELAY`. Its EL3
monitor refuses a virtual timer that is enabled or whose PPI 27 is pending or
active. It preserves the timer compare and control fields, the redistributor
configuration and the priority word containing PPI 27. The monitor configures
PPI 27 as a level-sensitive Nonsecure Group 1 interrupt; the EL1 platform gives
it the kernel API priority and dispatches it to the existing deadline ISR.
The EL3 recovery timer remains separate.

Return disables the virtual timer before clearing its pending and active
state. Normal cleanup restores its compare, disabled control state, priority
and configuration before restoring interrupt enables. A refusal before GIC
mutation also preserves the acquired timer fields. Failed cleanup retains its
existing non-success verdict. Other image targets do not select this timer
consumer.

The target enables the FreeRTOS idle hook. Its idle task executes DSB SY and
WFI while the USB owner waits. Periodic scheduler ticks remain enabled.

## Host evidence covers the selected branches

`checks/event-misc-usb-delay.py` executes the production hold and reset functions
on the host FreeRTOS kernel and precise timer. With a tick-aligned start, its
modeled MMIO writes follow a 150,001 µs detach hold, a 1,001 µs reset hold and
a 41 µs propagation hold. An observer halfway through detach finds the owner
blocked. A notification before detach expiry does not shorten the hold.
Reintroducing a tick retry fails the check.

`checks/a733-monitor-are.py --usb-deadlines` executes the selected monitor
instructions. Cases cover normal restoration, refusal of an enabled, pending
or active timer, a dropped priority restoration, QEMU setup and early
affinity-routing refusals. These models supply register and timer effects;
they do not establish physical GIC propagation, USB reset behavior or recovery.

`checks/power-write-order.py` preserves the posted-store ordering checks and
injects failed deadline waits, counter reversal and overflow before device
programming. The USB control-request and diagnostic protocol fixtures pass
their existing request, recovery-renewal and resynchronization cases. No check
establishes recovery when the architectural counter itself stops.

The legacy USB diagnostic service retains its separate tick-paced protocol
loop in `drivers/usb/diagnostic.c`. This change replaces reset holds and enables
idle WFI; it does not qualify an interrupt-driven diagnostic service.
