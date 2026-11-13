# The supervisor observes health at precise 20 ms deadlines

Status: implemented and host-tested. The OS supervisor supplies the existing
health, failure and watchdog checks. Physical wake timing remains unqualified.

The integrated OS supervisor observes a raw EL3 fault byte and service health.
The raw byte has no task notification, and the service registry has no query
for its earliest expiry. These inputs require a bounded observation cadence.
The declared interval remains 20,000 µs, or 50 deadline wakes per idle second.
Notifications may request earlier observations.

The [wait helper](../../apps/os/supervisor-wait.h) owns one precise-deadline
slot. Its caller zero-initializes `struct os_supervisor_wait` and calls
`os_supervisor_wait_init` once in the supervisor task, before the first policy
checks. Initialization returns one on success or zero on refusal. The helper
retains the task identity and its slot for the image's lifetime. Registration
failure cannot authorize a later wait or reinitialization.

After each policy pass, the caller invokes `os_supervisor_wait_next`. It returns
`SUNSTONE_DEADLINE_NOTIFIED` for a notification, `SUNSTONE_DEADLINE_EXPIRED` for
a deadline, or `SUNSTONE_DEADLINE_ERROR` on refusal. Both event results require
the caller to perform its ordinary checks. Clock and wait failures latch;
subsequent calls cannot resume waiting. Another task's refused call leaves the
actual owner's state intact.

The first deadline falls 20 ms after initialization. Later deadlines retain
that phase: each belongs to the same fixed 20 ms sequence. An early
notification returns to the policy checks without moving the next deadline.
If a completed policy pass has already crossed one or more deadlines, the
helper skips those elapsed points and blocks until the first future one.
This avoids repeated immediate expiry while preserving the sequence. Deadline
arithmetic refuses values outside the precise backend's counter domain.

The [focused check](../../checks/event-supervisor-wait.py) runs the actual
FreeRTOS kernel, notification API, deadline facility and deterministic timer.
It checks no wake before an event, 50 deadline wakes in one second, two early
notifications without phase drift, and delayed service without an overdue
wait loop. Actual slot exhaustion, an injected wait error and another caller
exercise refusal and retained ownership. Four negative controls introduce
1 ms wakes, phase drift, repeated overdue waits and an unlatched error.
Native compilation covers the helper's AArch64 FreeRTOS declarations.

The OS supervisor registers before its first policy pass. A registration or wait
failure records its site and parks the supervisor, stopping watchdog renewal.
The independent watchdog bounds that failure; failed wait machinery supplies no
safe retirement proof. The helper changes waiting only. It supplies no health
policy, watchdog reload, resource-retirement proof or physical timing evidence.
