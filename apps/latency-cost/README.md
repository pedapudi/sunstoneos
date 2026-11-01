# Measure optional latency recorder cost

This diagnostic is implemented for paired returning OS runs with latency
recording enabled and disabled. Board timing remains unmeasured until both
images run on the same board at the vendor CPU operating points: 1794 MHz
at 1000 mV for Cortex-A55 and 2002 MHz at 1050 mV for Cortex-A76.

The application first records 96 CPU0 timer-handler durations while its test
tasks are blocked. An image-local wrapper surrounds the unchanged recorder
wrapper and the real interrupt handler. Each sample records the elapsed counter
ticks and the number of OS ticks processed. Separate comparisons cover handlers
that process one tick and handlers that catch up after a delay. Common exception
entry, exception return and a subsequent context restore lie outside the timed
interval. The diagnostic wrapper publishes an acknowledgment after sampling
stops, before the main task copies its arrays.

The application then measures each CPU separately. A sender at priority 3
notifies a receiver at priority 4 on the same CPU. The receiver increments an
acknowledgment and blocks before the sender resumes. Each round trip therefore
forces two context switches. Each CPU records 48 batches of 32 round trips.
A wrapper counts actual scheduler context-switch entries. A sample with more
than 64 entries has scheduler interference. An OS-tick-change flag identifies
additional possible timer interference. Other interrupt types can still affect
a sample without changing either field.

The counter reads, notification API, acknowledgment, benchmark switch counter,
exception paths and both context switches contribute to round-trip duration.
The host divides batch time by 64 to express cost per forced switch. Subtracting
the disabled mean from the enabled mean estimates added cost for this complete
notification workload. It does not isolate context switching from notification
API instrumentation. The difference between observed maxima is reported
separately; it is not a bound on the cost of an individual switch or interrupt.

Both builds use the same benchmark wrappers. The enabled build renames the
existing recorder IRQ wrapper only in this diagnostic image, then calls it
inside the timing wrapper. Neither build overrides the recorder's weak task
readiness or tick callbacks. Costs from the eight wake callbacks in the separate
latency workload are therefore absent. The enabled recorder still exercises
its ordinary maximum-update and first-budget-overrun branches when their input
intervals require them; this benchmark does not guarantee every recorder state.

Configure the root project with `SUNSTONE_APPLICATION=latency-cost`, the
returning CPU-clock profile, admitted Cortex-A76 CPUs, and either value of
`SUNSTONE_LATENCY`. Build `sunstone-a733-os-latency-cost`. The application uses
no devices or input regions, returns 7784 bytes, requires no storage writes,
and uses the existing application return procedure. The coordinator runs both
images through the board tooling and preserves the log, image hash and Git
revision. A clock-readiness wait precedes measurement, and a top-point check
follows it. Any observed thermal reduction rejects the result.

`host.py` decodes a native report. `checks/latency-cost.py` compares two reports
without device access and retains raw samples, per-core minima, means and maxima,
and timer results grouped by OS tick count. No overhead budget is asserted before
measurement. A regression decision needs an explicit budget for its consumer.

The standalone CMake project runs the same task and IRQ paths under QEMU on
eight emulated CPUs. Its model supplies application admission, clock and thermal
responses. `checks/latency-cost-model.py` validates preserved QEMU logs against
the binary report decoder and checks that workload callbacks remain weak.
QEMU counter values establish control-flow coverage and report compatibility;
they establish no physical execution time, power, thermal or recovery behavior.
