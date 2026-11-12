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
