# Application completion waits for published events

Status: implemented and host-tested. Physical validation remains open.

The application's main task waits for GPU peer completion, USB diagnostic
readiness and USB application-buffer retirement with its registered precise
deadline. Each wait ends by the earlier of the application's finish limit and
the session's final 10 ms guard. The waiter rechecks the completion predicate
after registering its notification slot, which closes the publication race.

GPU peer completion publishes its release flag before waking the main task.
Each newly observed USB diagnostic checkpoint wakes the main task. The USB
buffer broker publishes one retirement notification after its final lease
release. A completed transfer, controller halt or application exit alone cannot
retire a borrowed USB buffer.

The host check `checks/event-misc-application.py` executes the production wait
functions, the FreeRTOS host port and the precise deadline runtime. Its idle
observer finds the main task blocked throughout the first second without an
event. A publication at two seconds completes the wait. A second case publishes
between the initial predicate test and notification registration. Reintroducing
a tick delay makes the idle case fail. `checks/app-usb-service.py` checks the
single retirement publication after the final lease release.

These checks model event producers and the session clock. They do not qualify
physical USB interrupts, GPU completion timing or idle power.

The public `ss_wait_until` accepts an absolute microsecond deadline in the
grant's counter epoch. It clips work waits to the immutable application finish
limit and fails when that limit is exhausted. Preemption between a caller's
clock observation and the wait cannot move the deadline. Relative `ss_usb_wait`
calls retain their interval contract, which the storage cut-lifetime diagnostic
uses through the session cut point. Private retirement waits use their return
reserve independently of the work limit.

## Periodic invalidations use absolute deadlines

The optional translation lookaside buffer measurement task derives its period
in microseconds from the configured period and waits on the precise deadline
facility. A notification before the deadline cannot issue an invalidation early.
The control form executes the same cadence with no invalidation instruction.
The public measurement snapshot in `include/sunstone_tlbi.h` reports its form,
declared period, wake count and instruction costs without exposing OS storage.

`checks/event-misc-tlbi.py` runs that control form with a 10 ms period. It checks
that the owner remains blocked at 5 ms, executes two periods by 21 ms and stops
at the next deadline after application completion. A delayed first alarm keeps
the second period at 20 ms. A tick-paced retry mutation
fails the check. This periodic measurement task has a declared timer event for
every wake; it has no device interrupt source.

## Background measurement sleeps use their declared time

The optional scheduler background load registers a deadline slot for each of
its 24 tasks. Initial staggering and the one-to-six millisecond sleep intervals
use absolute microsecond deadlines. Stopping the load publishes its stop flag
before notifying every task. A task tagged with an application address space
receives the runtime notification index, preserving the application's own index.
The worker count comes from `include/sunstone_bg_count.h`, shared with deadline
reservation. The final worker publishes a stop acknowledgment after a chain of
release operations. The public measurement caller waits for that publication
and acquires the stopped count before reading statistics.

`checks/event-misc-background.py` checks an idle interval and cancellation for
both tagged and untagged tasks. The owner remains blocked at 5 ms, and a stop
event at 10 ms cancels its 20 ms sleep. A tick-paced retry mutation fails.
The check excludes the load's synthetic CPU bursts, which retain their
counter-bounded busy loop. Those bursts are measurement work rather than a
device-completion wait.

`apps/event-waits` is a returning native workload that selects both measurements.
It verifies the granted CPU clocks, runs two declared background phases, waits
for every worker to stop and records counts through the public measurement APIs.
Its phase and stop waits use their original absolute bounds. The selected native
image links with the declared deadline storage and public measurement calls.
Its CPU grant follows the [vendor operating-point sequence](cpu-top-opp.md#the-driver-and-the-vendor-sequence).
The workload requires every worker's stop acknowledgment before reading its
statistics. Physical validation is recorded in [the owner evidence](../evidence/other-owner-events.jsonl).

## USB return waits preserve each retirement proof

The USB host owner registers its deadline slot before starting. A return claim
that precedes publication of the shared return deadline sleeps until the
supervisor's notification. The session's final 10 ms guard bounds that wait.
Type-C provider and power stop completion notify the host after their local
restoration proves complete.

An unfinished xHCI halt or GIC interrupt retirement uses a 50 µs absolute retry,
capped by the shared return deadline. These status transitions have no separate
completion interrupt. This uses the same retry policy as
`apps/board/host_irq.c`; [USB host interrupt ownership](../usb2-host.md) describes
the mask, deactivation and rearm obligations. Expiry or a failed deadline wait
retains the failure and parks the task. The host still requires controller halt,
interrupt retirement, buffer release, local platform restoration and both
Type-C stop proofs before reporting completion.

`checks/event-misc-usb-return.py` checks a full second with no publication event,
an explicit publication wake and the 50 µs retirement retry. Its tick-wait
mutation fails. `checks/os-quiesce-usb-radio.py` checks the retirement ordering
and retained faults. Physical return qualification remains open.

The USB host configuration without interrupt binding retains a normal-operation
polling reference at a precise 1 ms cadence in `apps/os/client.c`. It has no
idle-wake reduction and remains unqualified. The legacy USB0
runtime in `drivers/usb/a733_usb.c` has no linked precise-deadline backend.
Replacing either path requires its selected interrupt and timer composition;
the host return checks do not qualify those paths.

## Network and SDIO completion use a retained event binding

The socket owner in `drivers/net/sunstone_net.c` blocks on notifications and
precise request deadlines when a transport supplies the
[network event contract](network-owner-events.md). The native radio binding
uses INTID 194 for command, FIFO and card-function events. Its owner preserves
platform admission and waits through the registered deadline facility. Normal
and failed shutdown require interrupt retirement before controller clocks close.
The unbound composition retains its polling reference. Software request,
cancellation, DNS, socket and disposal publications are implemented and
host-tested. Physical SDIO interrupts and the 25 MHz FIFO service margin remain
unqualified. OS images select the generated route and notification waits,
including the shared Bluetooth owner's earliest deadline.
