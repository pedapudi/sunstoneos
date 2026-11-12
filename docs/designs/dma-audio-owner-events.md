# Device owners block on notifications and absolute deadlines

Status: implemented and host-tested for native DMA and I2S owner waits.
Physical interrupt and idle-core qualification remains open.

## DMA retirement observes distributor propagation at absolute times

The DMA application adapter already receives service replies without a periodic
owner task. Its idle timer wake count is zero. The retirement callback runs on
the DMA worker after admission closes and every channel is closed.

The callback masks DMA0's interrupt and waits for distributor propagation before
publishing `irq.masked`. The GIC architecture defines the Register Write Pending
bit, `GICD_CTLR.RWP`, as a software-polled completion condition; it supplies no
completion interrupt. See the pinned Arm GIC architecture reference, IHI 0069G,
the `GICD_CTLR` register description, indexed in [the reference
index](../reference-index.md#cached-primary-sources).

The callback retains its 1 ms observation interval and uses
`sunstone_deadline_wait_until()` to program each absolute observation time.
The shared return deadline shortens every wait. A deadline registration failure,
timer failure, late observation or uncertain mask retains the service. A task
notification can end the wait early; the callback rechecks the distributor and
the deadline before publishing retirement.

[The focused host check](../../checks/event-dma.py) executes the callback with
modeled distributor propagation, a notification during a wait, exact deadline
expiry and deadline failures. A mutation that substitutes a scheduler-tick delay
fails. The adapter creates no task and therefore contributes no idle timer
wakes; this source count establishes no DMA worker or physical interrupt timing.
The [DMA return adapter
check](../../checks/os-quiesce-dma.py) separately retains the pending, completed
and faulted quiesce cases.

## Native DMA waits for transfer interrupts and exact expiry

The OS DMA worker waits indefinitely on its notification while idle. Requests
notify the worker independently of interrupt binding. A running transfer with
an armed interrupt waits until that interrupt or its transfer deadline.
The DMA counter and deadline clock share the 24 MHz architectural counter;
the worker rounds a raw-counter deadline upward to a whole microsecond.
Local state transitions run consecutively while they make progress.

Drain and register propagation have no completion interrupt. They retain the
1 ms observation interval through absolute compares, bounded by the controller
or stream operation deadline. The pool bootstrap batches ready startup steps
and schedules the reset-settle sample after its required 20 us hold.
Pool readiness and retirement publish client wake hints. Diagnostic builds
outside the integrated OS retain their existing scheduler-tick waits.

The native worker's source-counted idle timer rate is zero before and after
this change. Its active fallback wakes at 1,000 times per second with the
single-tick I2S binding; an armed native transfer now has only interrupt and
deadline wakes. These counts describe wait arguments, not physical wake counts.

[The worker check](../../checks/event-dma-worker.py) executes the production
worker body with modeled queue and timer boundaries. It covers idle waits,
transfer expiry, precise drain observations, local ready work and an unbound
interrupt profile. [The pool check](../../checks/event-dma-pool.py) executes
the bootstrap with modeled platform results and verifies reset settle,
retirement and exact expiry. Both reject scheduler-tick mutations. These
extracted-function checks do not run the FreeRTOS scheduler. The separate
[retirement check](../../checks/dma-retirement.py) runs the real scheduler and
controller in QEMU with modeled registers.

The optional `dma_service_wake_record` symbol contains six 64-bit counters:
waits, wakes, idle microseconds, idle notifications, idle timeouts and deadline
expiries. The worker writes them; a host reads them only after return.

An interrupt-bound running channel does not poll for completion before its IRQ
publication or transfer deadline. STATUS requests follow the same rule. The
controller retains its existing exact-timeout and drain behavior. The native
RAM-copy diagnostic requires a new interrupt and no deadline expiry for each
copy, so timeout-assisted completion cannot qualify its interrupt path.

Client replies and device progress have separate publications. A reply enters
its queue before notifying the caller. A pending STATUS result waits for a
later device-progress sequence instead of sending another request on its own
reply notification. Caller identity, quiesce and failed-pool checks precede
that sequence test.

The application can wait for CPU readiness and valid thermal admission through
precise runtime waits. The first CPU-ready publication wakes it. Thermal level
or freshness-cause changes wake it after publication; ordinary freshness
renewals supply no hint. An expired thermal reader rechecks the publication
once to close a refresh racing its counter read. The
[readiness check](../../checks/event-owner-readiness.py) covers both expiry
interleavings and rejects missing or periodic hints.

Physical RAM-copy admission remains closed without current nonsecure controller,
descriptor/data, reset-lifetime and writer-exclusion authority. The
[DMA access contract](../dma-pool-session.md) requires those facts; historical
I2S probe provisioning does not establish a later OS session's permissions.
The typed session builder has no DMA record producer. Software wait evidence
does not replace either requirement.

## The I2S owner waits for requests, replies and completed periods

The audio task batches local state transitions and blocks after it exhausts
ready work. Client start and stop requests notify that task. The DMA service
publishes separate reply hints and device-progress hints; phase changes,
completed periods and pool readiness or retirement advance a progress sequence.
A request snapshots that sequence before entering the service queue.
An unchanged or busy reply cannot cause another request until the sequence
advances. This preserves a device change that races the reply and prevents
stale reply notifications from creating an empty-request loop.

Idle audio waits until a request, a quiesce notification or the session deadline.
Active work retains the declared one-second audio progress window as a precise
deadline. Return work uses the supervisor's shared return deadline.
PLL sampling observes the parent's required sample spacing; clock contention
and register propagation retain precise 1 ms availability waits. The DMA worker
still owns DMA and I2S hardware until its retirement publication, after which
the audio task retires the pool and audio parent.

The source-counted idle timer rate falls from 1,000 wakes per second to zero
before session expiry. `os_audio_wake_record` has the same six-counter layout
as the DMA record. Idle notifications count incoming requests; idle timeouts
count expiry of the admitted session wait. Board wake rates remain unmeasured.

The [native capture and return check](../../checks/os-quiesce-audio-headset.py)
runs `os_audio_task` with modeled RTOS boundaries. Its event cases cover an
idle request, a busy period reply with no new period, progress racing a reply,
period publication and exact operation expiry. The same check retains PCM,
stream reuse, drain and retained-fault cases. Mutations that restore a tick
wait or treat reply hints as device progress fail. These owner checks execute
no physical DMA, I2S or FreeRTOS context switch.
