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
Its phase and stop waits use their original absolute bounds. Its manifest and
the selected native source objects compile; a complete image still requires
the central deadline reservation and build definitions. It has no physical
qualification.
