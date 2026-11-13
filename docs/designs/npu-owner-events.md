# NPU owners wait for publications and precise deadlines

Status: implemented and host-tested in the integrated OS profile. One physical
512-frame recognition session passes with exact output and no idle deadline
wakes in its outer loops. No-work idle rates and WFI residency are unmeasured.

The NPU service worker receives interrupt 97 and client-request notifications.
Its completion read acknowledges `+0x010`; full idle still precedes output
publication. The [interrupt contract](npu-irq-completion.md) and the
[vendor sequence](../reference-backsolve-npu.md#interrupt-idle-gating-and-combined-image-paths-re-derived-2026-11-01)
define the hardware order. The OS disables the idle-register completion hint:
a missing interrupt reaches the job deadline and retains its resources.

The mailbox owner wakes for application requests, policy changes and final
retirement. A thermal freshness update only extends admission time. The
bootstrap owner wakes for power-service answers, policy changes, worker park
and the client close publication. Each owner rechecks its authoritative state
after a notification, including a notification that precedes its wait.

## Waits preserve completion and failure ownership

The service registers a precise deadline slot before device access. The
bootstrap registers another slot, and the mailbox registers its supervised
service slot. Registered tasks remain suspended or blocked after retirement.
Replies enter their queue before the service notifies the client. A client
collects without blocking, waits until an absolute two-second reply limit,
then checks the queue again. Fault images retain their eight-second window.
The earlier session or stop cutoff bounds each window.

A ready interrupt-bound service waits indefinitely for a request. An active
job waits for its interrupt or controller deadline. GIC propagation, full idle
after a latched event, and cancellation acknowledgment have no further
completion interrupt. Those states use precise 50 microsecond retries.
GIC callback availability has a separate ten-millisecond elapsed budget.
An expired GIC budget retains the source and leaves its worker blocked.
Arm GIC specification IHI 0069G defines the polled `GICD_CTLR.RWP` field;
the cached reference is indexed in [the reference index](../reference-index.md).

The vendor's minimum controller delays, three PLL LOCK samples, sample gaps
and PLL settle time use the same precise deadline facility. The two-millisecond
startup settle returns PENDING until its absolute endpoint. Register values,
power requests, barriers and ownership checks retain their existing order.
The registered [failure-return step](failure-return.md#device-owner-audit)
still requires client closure, worker park, IRQ retirement and local stop.
A timeout supplies no proof that DMA or the NPU stopped.

## Software checks count idle waits and reject tick polling

`checks/event-npu.py` runs the actual finite and runtime service loops with
FreeRTOS and the deadline runtime. Both remain blocked for two seconds without
events, complete after one interrupt, and retain a job at a 1,237 microsecond
deadline. A stuck GIC callback reaches its ten-millisecond budget and remains
blocked afterward. A mutation that restores idle tick waits fails the check.
The actual native-delay and PLL-wait functions also run with early notifications
and a stopped source clock. These are software scheduling and register models,
not measurements of interrupt delivery or electrical timing.

The session's NPU registration record contains outer-loop wait counters for
the service, bootstrap and mailbox. Each row gives waits, event wakes, deadline
wakes, idle event wakes, idle deadline wakes and accumulated idle microseconds.
The client copies them after the stopped proof and before its final report.
The host derives idle wakes per second from completed idle intervals. Native
vendor-hold waits and the direct queue producer's wakes are outside these
outer-loop counters. Incomplete idle intervals have no reported rate.

Legacy diagnostic images without the OS timer backend retain their existing
wait contracts. The integrated OS's interrupt-free compatibility path polls
only active work with a precise deadline; idle requests still notify it.

## Qualification remains limited to the measured profile

The host idle target is zero timer wakes per second for the ready service and
for the mailbox without a policy change. Source counts for the tick-paced
service are 1,000 wakes per second during an active job and one per second
while ready. The bootstrap's 25 millisecond wait permits 40 wakes per second;
thermal freshness can wake the mailbox at the same cadence.

The NPU completion budget remains 1,000 microseconds from interrupt to client
delivery. The resident queue already gives its producer direct interrupt
completion, so removing owner polling predicts little recognition speed change.
Physical conditions, predictions and results are in
[the owner evidence](../evidence/npu-owner-events.jsonl). The session's event
wakes include graph loading and requests; their rates describe those waiting
intervals, not a board with no work.
