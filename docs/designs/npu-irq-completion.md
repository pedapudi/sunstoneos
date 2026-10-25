# NPU job completion on interrupt 97, with cycle-counter job timing

Status: **implemented in the interrupt-driven finite profile; not run on a
board.** The interrupt host (`apps/npu-interrupt/host.py`) needs a root review
before a board run, as `application_review_1024` reviewed the polled host. It
extends rank 11 of the [vendor action plan](../vendor-firmware/action-plan.md)
and follows section 4 of the [performance architecture](../performance-architecture.md).

The finite NPU image runs one convolution twice and compares both outputs with
the reference. This design changes three things in it:

- **Completion record.** The convolution backend records a job's completion
  when the controller finishes it. It recognizes the job by its command's
  content (`same_command`, `drivers/npu/npu_convolution.c:332`), because the
  controller passes its own copy of the command (`a733_npu_controller.c:162`,
  `:231`). The earlier pointer comparison never matched, so the board run of
  `board-session-20261025-7/port-3-3` recorded no completion.
- **Interrupt completion without client polling.** In the interrupt profile
  (`apps/npu-finite/build.py --interrupt`) interrupt 97 ends each job, and the
  client waits on the service's held `WAIT` reply instead of polling `STATUS`
  once per tick. Both profiles use `WAIT` (`apps/npu-convolution/main.c:79`,
  `:96`), as default 1 of the performance architecture requires.
- **Job timing.** Each job is timed with the NPU's own cycle counters and with
  the system counter, and the image logs both.

## Acknowledge and clear order

The board kernel's driver and SunstoneOS acknowledge the device event and
clear the interrupt in different places. Both treat SPI 65 as level high: the
NPU holds the line while its event word `+0x010` is nonzero, and a read of
`+0x010` returns the events and clears them.

| Step | Vendor driver | SunstoneOS interrupt profile |
| --- | --- | --- |
| Enable | `+0x014` = `0xffffffff`, every event (`vip2/vip_drv_hardware.c:509`) | `+0x014` = `0xc0000001`, completion and the two error events (`a733_npu_controller.c:32`, `:152`) |
| Delivery | `request_irq` with `drv_irq_handler` (`vip2/os/linux/vip_drv_device_driver.c:956`) | The SRE port's dispatch acknowledges the GIC and calls `npu_application_irq` (`apps/npu-interrupt/irq.c:142`) |
| In the handler | Reads `+0x010`, which acknowledges the device events, records the bits, counts them and wakes the waiter (`vip_drv_device_driver.c:382`, `:392`) | Records the system counter, masks SPI 65 at the distributor and notifies the service worker; no device access |
| End of interrupt | The GIC end of interrupt after the handler returns; the line is already low | The combined end of interrupt; the line stays high but masked |
| Event read | Done in the handler | The worker reads `+0x010` (`a733_npu_controller.c:213`), which acknowledges the events, then requires full idle at `+0x004` (`:226`) and reads `+0x010` once more for a late event (`:227`) |
| Waiter | `vipdrv_os_wait_interrupt` returns, the task clears its software flag, decrements the count (`vip2/task/vip_drv_task_common.c:1053-1056`) and checks the AXI and MMU error bits (`:1063`) | The controller rejects any event other than completion as an interrupt fault, then finishes the job; the worker answers the held `WAIT` (`npu_service.c:169`) |
| Rearm | The line is unmasked throughout | Unmasked only before the next bootstrap or job wait, and only when no notification is pending (`npu_service.c:78`) |
| Clear at close | No pending clear | CLOSE disables `+0x014` and the MMU with readback and requires full idle (`a733_npu_controller.c:260-266`); the stop callback then masks SPI 65, clears its pending bit at `GICD+0x28c` and requires it not enabled, pending or active (`irq.c:74-90`) |

The worker alone reads `+0x010`, so the device events have one reader, and the
masked line cannot interrupt the worker between the event read and the
finish.

## Fallback and deadline

The worker sleeps on its task notification. The interrupt, a client request
and a one-tick timeout each wake it (`npu_service.c:488`, `:497`). The tick
timeout is the bounded fallback: it lets the worker check the controller's
deadline (`a733_npu_check_timeout`) while no interrupt arrives. The tick
cannot complete a job, because the worker samples `+0x010` only after a
notification. A job whose interrupt never arrives therefore ends at its
deadline, 1 s for each job and for the bootstrap, as a timeout fault that
retains the NPU. The held `WAIT` then answers with the fault.

## Interrupt routing

The routing exists from the earlier interrupt profile and is unchanged:

- At EL3, before the scheduler, the monitor captures INTID 97's group,
  modifier, configuration, priority and route
  (`apps/storage-read-single-core/monitor.c:215`,
  `apps/npu-interrupt/irq-route.c:47`), then installs it as a Nonsecure group 1,
  level-sensitive interrupt at Secure priority `0xd8`, read as Nonsecure
  `0xb0`, routed to CPU 0 (`monitor.c:238`, `irq-route.c:60`).
- At EL1, `npu_application_irq_bind` requires the source disabled, inactive
  and not pending, level-sensitive, at priority `0xb0` and routed to CPU 0
  before it binds the service, whose worker is pinned to CPU 0 (`irq.c:100`).
- Before the ROM return, the monitor writes the saved route back before it
  restores any SPI enable (`apps/npu-finite/monitor.c:64`).

## Job timing

The backend records, for the latest job, at publication just before the
trigger and at the worker's completion observation (`npu_convolution.c:334`,
`:351`):

- the NPU's total cycle counter `+0x078` and idle cycle counter `+0x07C`, read
  in the vendor profiling build's order: total then idle at the start, idle
  then total at the end (`vip2/task/vip_drv_task_debug.c:267-268`, `:332-333`).
  The differences, modulo 2^32, are the job's total and idle NPU clock cycles;
  their difference is the active cycles, as the vendor computes
  (`vip2/os/linux/vip_drv_os_debug.c:1708`). At 852 MHz the counters wrap
  after 5.0 s, longer than a job's 1 s deadline. The vendor clears the
  counters before each job (`vip_drv_task_debug.c:390-395`); SunstoneOS
  takes differences and writes nothing;
- the system counter `CNTPCT_EL0`, 24 MHz on the A733;
- the NPU adapter's counter microseconds, as before.

The interrupt handler also records the system counter at each delivery
(`irq.c:139`). After each job the client copies these values. Before CLOSE
the image logs `NPU_TIME_US` (startup entry and READY, then each job's
publication and completion in adapter microseconds) and `NPU_JOB_COUNTERS`
(the counter frequency, then per job the system counter at publication, at
delivery and at completion observation, and the total and idle cycles). The
polled profile logs 0 for the delivery. The host reports both blocks in
`npu_result.timing` (`apps/npu-finite/host.py`, `timing` and `job_counters`):
`job_us`, and per job `start_to_irq_us`, `start_to_done_us` and
`active_cycles`.

The delivery time measures the job from publication to the interrupt,
excluding the worker's wake. The active cycles measure the NPU's own busy
time and exclude the time between completion and the worker's reading, during
which the NPU is idle.

## Measurements in the models

The models run in QEMU or Unicorn on the development host; their times are
emulation times, not board times. Each "before" is the parent revision
`c38a6ea5`, built with the same measurement print.

| Model | Quantity | Before | After |
| --- | --- | --- | --- |
| Convolution backend and polled service, QEMU (`checks/npu-convolution.c`), five runs | Second job's completion observation recorded | 0 in every run | 68–73 µs after publication |
| Same | Client SUBMIT to DONE reply | 523–565 µs, `STATUS` polls | 514–544 µs, one `WAIT` |
| Interrupt service, QEMU (`checks/npu-interrupt-service.c`), five runs of two jobs each | Modelled event delivery to the client's DONE reply | `STATUS` with a one-tick delay: 222–638 µs, median 454 µs | `WAIT`: 127–179 µs, median 159 µs |
| Finite clients, Unicorn (`checks/npu-finite-client.py`, `npu-interrupt-client.py`) | Client requests per run | OPEN, then per job SUBMIT, one or more `STATUS`, REAP; CLOSE | OPEN, then per job SUBMIT, `WAIT`, REAP; CLOSE |
| Host fixtures (`apps/npu-finite/host-check.py`) | Parsed job time from a logged block | none for the board's form | 20 µs per job, active cycles 48,000 |

In the convolution model the device stand-in completes at the worker's first
register read after the trigger, so the client's first `STATUS` already reads
DONE and the client mechanisms differ little. The interrupt model delivers the
event while the worker rearms, so the `STATUS` client pays its tick delay and
the `WAIT` client does not. The convolution model now also checks the
recorded cycle differences across a 32-bit counter wrap.

## Board predictions

For the two-start set of [the interrupt runbook](../board-runbooks/npu-irq-starts.md),
492 MHz then 852 MHz, two jobs per start:

- Completion recorded: every job has a nonzero completion time after its
  publication, and a delivery time between them.
- Job time, publication to delivery: the NPU's compute plus tens of
  microseconds of command fetch, interrupt and counter reads; under 100 µs
  at 492 MHz.
- If the job is compute bound, the active cycles agree within 10% at both
  rates and the 852 MHz job time is about 492/852 = 0.58 of the 492 MHz one.
  Memory-bound work takes more cycles at 852 MHz and a ratio nearer 1.
- Exactly three notified completions per start (bootstrap and two jobs) and
  the service's stopped receipt; outputs byte-identical at both rates.
- Startup entry to READY as in the polled runs, 4.0 ms.

## Checks

`~/.cache/sunstoneos/evidence/npu-irq-1025/irq-checks.sh` runs, at 492 and at
852 MHz, the interrupt image's return check with the installed route
(`apps/npu-finite/check.py`), SRAM routing, adapter, quiescence with the
service's stopped receipt, the interrupt client, the 18 routing cases of
`checks/npu-irq-route.py`, the task profile, the reference-clock pass and the
interrupt host fixtures (`apps/npu-interrupt/host-check.py`, 45 cases). The
interrupt host fixtures are the finite host's, with the interrupt admission
record, the interrupt receipts and the interrupt refusal cases added.

## Limits

- The interrupt host is not root reviewed.
- The worker still wakes once per tick while a job runs, to check the
  deadline; it reads no completion state then.
- The cycle counters are those the vendor's profiling build reads; whether
  they count while the clock is gated between jobs does not affect a job's
  differences.
