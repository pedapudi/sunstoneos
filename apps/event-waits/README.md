# Returning application and scheduler wait fixture

This application selects the background measurement and the periodic
invalidation control form. It verifies the granted top CPU clocks, runs two
600 ms background phases, stops all 24 load tasks and copies an 80-byte result.
The final worker publishes a stop acknowledgment before statistics are read.
Clock and thermal readiness publications wake the application under its original
finish deadline. The application passes each original absolute deadline to
`ss_wait_until`. This public wait does not require a USB device grant.

The result records elapsed and stopping time, bursts and busy time in each
phase, application waits, stopped tasks and invalidation-task wakes, issued
instructions and CPU. The control form must issue no invalidation. This is a
native workload definition with [finite timer and retirement evidence](../../docs/evidence/other-owner-events.jsonl).
Absolute work limits and preemption interleavings have host cases. Physical
WFI residency remains unmeasured.

Configure with `tools/configure.py`, selecting `SUNSTONE_A733_OS=ON`,
`SUNSTONE_APPLICATION=event-waits`,
`SUNSTONE_APPLICATION_event-waits_ROOT` set to this directory,
`SUNSTONE_BG_LOAD=ON`, `SUNSTONE_TLBI_ISSUER=0` and
`SUNSTONE_TLBI_PERIOD_TICKS=10` and `SUNSTONE_TLBI_CORE=5`. Build target `sunstone-a733-os` and package it
with `tools/session.py build --clock-grant`. The OS deadline capacity must
include all 24 load tasks and the invalidation task, including control value
zero. The background compile definition must propagate to the application.
Pass `apps/event-waits/host.py` as the session decoder. It checks both phases,
the 24 stop acknowledgments, the control form and the declared run bound. The
reported mean interval divides elapsed time by observed control wakes; sampling
overhead and skipped periods prevent an exact wake-count prediction.
