# The thermal owner waits for interrupts and absolute deadlines

Status: implemented and host-tested. One physical recognition session passes
with a complete temperature window and recorded owner wait counts. Steady idle
rates, alarm delivery and WFI residency remain unqualified.

The thermal owner in [thermal.c](../../apps/os/thermal.c) registers once with the
[scheduler deadline facility](scheduler-realtime.md#high-resolution-deadlines-use-a-separate-compare-consumer).
After each bounded pass it blocks on a notification until the earliest absolute
microsecond deadline. A scheduler tick alone does not make the owner runnable.
The steady path has these deadlines:

- Sensor reads retain their 25 ms phase and skip slots crossed during a read.
- Cached reports retain their separate 125 ms phase. An interrupt can establish
  a report phase between sensor deadlines, so a report can require its own wake.
- Published temperature freshness expires one microsecond after its inclusive
  one-second limit.
- The session ends 10 ms before its declared deadline.

THS interrupt 71 masks its source and gives the owner's notification. The owner
collects sensor and alarm status, acknowledges observed bits and rearms the
source after consuming the publication. A notification between the owner's
predicate check and wait remains pending and ends the wait immediately.
The [thermal reference](../thermal.md#polling-and-deferred-interrupt-service)
defines the five sensor channels, write-one-to-clear status and alarm semantics. It pins the
A733 manual and the vendor source manifest; this change adds no source dependency.

THS admission, GIC distributor propagation and the CPU clock adapter expose no
completion notification to this owner. These paths retry at an absolute deadline
one millisecond later. The GIC's register-write-pending bit has no interrupt
completion in [the SPI adapter](../../drivers/gpu/gicv3_spi.c). Its propagation
availability budget is 100 ms of elapsed time. Additional THS notifications do
not shorten that budget. CPU clock register and voltage sequencing remains in
the [existing owner adapter](../../apps/os/cpu.c).

The [registered thermal stop](failure-return.md#the-failure-return) gives the same
notification. Sampling continues until earlier return classes drain. The task
then masks the interrupt, waits for distributor propagation and executes the
configured THS stop. Only its stop proof completes the registration. A fault
preserves the clock token, masks the interrupt and records the quiesce fault.
After token retention completes, the task suspends.

The optional `os_thermal_wake_record` in image memory contains six 64-bit words:
start time, latest return time, wait returns, notifications, deadline expiries
and errors. The task is its only writer. The host can capture the record after
return through the existing recognition thermal window. Images without that
window omit this optional capture. The record spans startup, active work and
stop, so its total rate does not isolate steady idle sampling. Concurrent
readers use the deadline facility's synchronized owner record.

The [event check](../../checks/event-thermal.py) executes the actual owner task,
FreeRTOS kernel and deadline implementation. It checks 40 wakes in one idle
second at both tick-aligned and 123 µs phases. An alarm that establishes an
off-grid report phase produces 48 wakes: one interrupt and 47 deadline expiries.
It also checks interrupt-time sampling, sample-phase preservation, registered
stop completion and suspension after stop. Replacing the precise wait with a
one-tick wait fails the idle-wake assertions.

The [cadence check](../../checks/os-thermal-cadence.py) covers delayed reads,
missed slots, policy freshness, thermal faults and the elapsed GIC rearm budget.
Its modeled THS and GIC effects establish software behavior only. Board evidence
must measure idle wakes and confirm interrupt delivery at the approved operating
points before physical qualification.

The [owner evidence](../evidence/thermal-owner-events.jsonl) records the
recognition profile, whole-lifetime counters and remaining measurement limits.
