# Review package for NPU internal clock gating

**Status: awaiting code review.** The implementation is host-tested and has no
physical result. No gating image has run on a board. Three independent reviewers
(simplicity, performance and correctness) must approve the code before the
always-on control image and then the gating image run on port 3-3. Their earlier
design approvals do not cover this code.

## Scope under review

The coordinator approved internal clock gating only: the NPU register window at
`0x03600000` and the vendor VIPLite gate helpers. The change touches no rail, no
PCK4 domain, no PLL, no voltage and no top-point clock. The NPU stays at 852 MHz
and 800 mV. Gating is opt-in. A queue that does not call
`npu_runtime_queue_power_start()` creates no owner task and behaves as before.

**Diff range.** Review `383bfb8a..` the branch tip named in the closing report,
on `work/npu-fastpath-1101` (`git diff 383bfb8a..HEAD`). The range includes the
merge of `bringup`. The files that carry the policy are:

| File | Role |
| --- | --- |
| `include/npu_idle.h`, `drivers/npu/npu_idle.c` | State machine, suspend and resume sequences, retention record |
| `drivers/npu/npu_runtime_queue.c`, `include/npu_runtime_queue.h` | Power owner task, admission, completion hook, close path |
| `platform/a733/npu_irq.c`, `include/a733_npu_irq.h`, `drivers/npu/npu_service.c` | Quiet query for the interrupt controller; service support |
| `apps/npu-finite/deadline.c`, `deadline.h`, `monitor.c`, `runtime-irq.c` | Shared deadline timer composition in the finite images |
| `apps/npu-finite/idle-client.h`, `queue-client.c`, `startup.h`, `build.py` | Idle benchmark producer and its two images |
| `apps/npu-finite/host.py`, `idle-compare.py` | Report decoder and acceptance comparison |
| `cmake/sunstone-os-core.cmake` | Links `npu_idle.c` into the NPU OS build; enables the priority query |
| `checks/npu-idle.c`, `npu-idle.py`, `npu-queue-handoff.py`, `npu-idle-report.py` | Host evidence |
| `docs/designs/npu-fast-path.md`, `docs/status/npu.md` | Design and status |

## Invariants

1. **Only two registers change.** The adapter writes only NPU offsets `+0x000`
   (clock gate and load) and `+0x10c` (shader scaler) and reads only `+0x000`,
   `+0x004` and `+0x10c`. It refuses every other offset, including the
   destructive event word at `+0x010`.
2. **No read after the final suspend write.** While the clocks are suspended the
   only read is `+0x000` at resume entry, which must equal the stored gate value
   with bit 0 set. Every other readback follows the ungate pulse and a settle of
   at least 1 µs.
3. **Transitions run under the raw local interrupt lock.** The decision and every
   register access of a transition execute with IRQs masked, so a producer
   observes only ON, SUSPENDED or RETAINED. The settle wait is capped at the
   100 µs resume budget.
4. **Gate only when idle.** Immediately before the first suspend write the owner
   rechecks, under that lock: the queue is empty, completions equal submissions,
   the public ring reports no published successor, STOP is clear, the lifetime
   window is open, the last completion left full idle, the idle generation is
   unchanged and the interrupt controller reports no pending or active NPU
   interrupt. A stale alarm never gates.
5. **Admission never touches a suspended device.** Admission in SUSPENDED records
   one resume request, returns pending and changes no job, command or fence. The
   owner resumes and issues one readiness notification.
6. **Every anomaly is retained.** A failed health check, readback, settle, owner
   operation or interrupt state enters RETAINED with the first site, phase,
   offset, value and expectation. It sets the queue, runtime and service-recovery
   flags and performs no further register access and no retry.
7. **An interrupt while not ON is a fault.** It is retained without any device
   access.
8. **Close resumes first.** The return path stops the owner, which resumes the
   clocks. Return fails and keeps its ownership if the clocks are not running.
9. **The control policy is the same owner without writes.** Wake schedule,
   alarms and the completion hook are identical; only the register writes are
   absent.
10. **The submit and completion hot paths gain no task hop.** A completion adds
    one counter read and stores, and at most one task notification when the owner
    requested a hint while a job was active.

## Questions for each reviewer

- **Simplicity.** Is a dedicated owner task and a separate state machine the
  smallest correct design? The alternative of gating from the completion
  interrupt would add timer work to the interrupt; the deadline facility already
  exists and is shared with the scheduler work.
- **Performance.** The owner wakes about every 10 ms while the queue is busy and
  preempts the producer on the same core. The 105% p99 and 110% maximum limits
  apply to jobs of about 10 µs, so one preemption of a few microseconds can reach
  the limit. The suspend and resume sequences mask interrupts for a few
  microseconds each. Is this acceptable, and is a different pinning or priority
  needed?
- **Correctness.** Review the shared virtual timer composition in the finite
  images (PPI 27 is installed by the EL3 monitor; the FreeRTOS tick uses the
  physical timer on PPI 30), the quiet predicate, the raw-lock boundaries, and
  the assumption that the owner is never deleted.

## Host evidence

Evidence directories are under
`~/.cache/sunstoneos/build/npu-fastpath-1101/review-evidence/`.

| Check | Result |
| --- | --- |
| `npu-idle/` (`checks/npu-idle.py`) | Eight portable transition groups with delayed MMIO, access-limited suspended reads, partial failures and exact failure origin |
| `npu-queue-handoff/` (`checks/npu-queue-handoff.py`) | The production owner, queue, idle and service code executed natively: the existing handoff cases plus thirteen owner cases and eight relink-based regression mutations |
| `npu-idle-report/` (`checks/npu-idle-report.py`) | Report decoder, acceptance comparison, twelve corrupted reports and one mismatched policy; the producer is not executed |
| `../final2/npu-latency/` (`checks/npu-latency.py`) | The latency producer and decoder, 27 cases, unchanged behavior without the idle benchmark |

The thirteen owner cases are: start refusals; gating after the delay and resume
on admission with the register and lock trace; the control policy; a stale alarm
with an active job; a stale alarm after a completed job; a pending interrupt
state; an unreadable interrupt state; partial failure between the two suspend
writes; suspended close; suspended close with a partial resume failure; STOP
cancelling an armed alarm; an interrupt while suspended; and a published
successor in the public ring. The mutations remove, one at a time, the activity
guard, the quiet check, the suspended-interrupt guard and the resume before STOP;
each is caught by its named case.

## Limits of the evidence

- The scheduler, the shared deadline facility and the interrupt controller are
  modeled. Timing is modeled counter ticks, not silicon.
- The producer in `idle-client.h` executes only on the board. Its in-image checks
  retain the run on any violated expectation.
- The operating system image does not yet call `npu_runtime_queue_power_start()`.
  The public ring predicate is a modeled callback. Wiring the EL0 ring into it
  belongs to the EL0 work.
- Safe shared-SRAM access while gated and recovery from a fault while suspended
  have no evidence. The PMIC provider reads no DCDC2 current or power, so there is
  no energy evidence.

## Board plan after approval

Both images are built from `apps/npu-finite/build.py --latency queue --idle off`
and `--idle gate` and share every source. Each runs 1,001 warm jobs, then 100
cold jobs after each of 9, 11 and 100 ms idle gaps. The control runs first. The
gating image runs only if the control returns normally with matching outputs.
`apps/npu-finite/idle-compare.py` applies the acceptance limits and prints the
comparison table.
