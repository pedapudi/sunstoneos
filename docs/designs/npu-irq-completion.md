# NPU job completion on interrupt 97, with cycle-counter job timing

Status: **implemented** in the interrupt-driven finite profile and **qualified on the board** for the finite interrupt diagnostic at 852 MHz and 800 mV (startup 4.0 ms, inference 2.91 ms from doorbell to interrupt; [record](../evidence/npu-irq-completion.jsonl), id `npu-irq-completion-board-prediction`). The interrupt host (`apps/npu-interrupt/host.py`) is not root reviewed. The design extends rank 11 of the [vendor action plan](../vendor-firmware/action-plan.md) and follows section 4 of the [performance architecture](../performance-architecture.md).

The finite NPU image runs one convolution twice and compares both outputs with the reference. Three mechanisms govern completion and timing:

- **Completion record.** The convolution backend (`drivers/npu/npu_convolution.c`) records a job's completion when the controller finishes it. It recognizes the job by its command's content (`same_command`), because the controller passes its own copy of the command.
- **Interrupt completion without client polling.** In the interrupt profile (`apps/npu-finite/build.py --interrupt`) interrupt 97 ends each job. The client waits on the service's held `WAIT` reply and does not poll `STATUS` once per tick. Both profiles use `WAIT` (`apps/npu-convolution/main.c`), as default 1 of the performance architecture requires.
- **Job timing.** Each job is timed with the NPU's cycle counters and with the system counter, and the image logs both.

## Acknowledge and clear order

The vendor driver and SunstoneOS acknowledge the device event and clear the interrupt in different places. Both treat SPI 65 as level high: the NPU holds the line while its event word `+0x010` is nonzero, and a read of `+0x010` returns the events and clears them.

| Step | Vendor driver | SunstoneOS interrupt profile |
| --- | --- | --- |
| Enable | `+0x014` = `0xffffffff`, every event | `+0x014` = `0xc0000001`: completion and the two error events (`drivers/npu/a733_npu_controller.c`) |
| Delivery | `request_irq` with `drv_irq_handler` | The SRE port's dispatch acknowledges the GIC and calls `npu_application_irq` (`platform/a733/npu_irq.c`) |
| In the handler | Reads `+0x010`, which acknowledges the device events, records the bits, counts them and wakes the waiter | Records the system counter, masks SPI 65 at the distributor and notifies the service worker; no device access |
| End of interrupt | The GIC end of interrupt after the handler returns; the line is already low | The combined end of interrupt; the line stays high but masked |
| Event read | In the handler | The worker reads `+0x010`, which acknowledges the events, then requires full idle at `+0x004` and reads `+0x010` once more for a late event |
| Waiter | Returns from the wait, clears its software flag, decrements the count and checks the AXI and MMU error bits | The controller rejects any event other than completion as an interrupt fault, then finishes the job; the worker answers the held `WAIT` (`drivers/npu/npu_service.c`) |
| Rearm | The line is unmasked throughout | Unmasked only before the next bootstrap or job wait, and only when no notification is pending |
| Clear at close | No pending clear | CLOSE disables `+0x014` and the MMU with readback and requires full idle; the stop callback then masks SPI 65, clears its pending bit at `GICD+0x28c` and requires it not enabled, pending or active |

The worker alone reads `+0x010`, so the device events have one reader, and the masked line cannot interrupt the worker between the event read and the finish.

## Fallback and deadline

The integrated OS worker sleeps on its task notification until an interrupt,
a client request or the precise controller deadline. It has no idle tick
timeout. A missing interrupt ends at the job deadline and retains the NPU.
The [owner wait contract](npu-owner-events.md) defines publication ordering,
register states without interrupts, vendor holds and host checks.

The standalone finite diagnostic retains its one-tick deadline fallback
because it does not link the OS timer backend. Its tick cannot complete a job:
the worker samples `+0x010` only after a notification. A job whose interrupt
never arrives ends at its deadline, one second for each job and bootstrap,
as a timeout fault that retains the NPU. The held `WAIT` answers with the fault.

## Interrupt routing

- At EL3, before the scheduler, the monitor captures INTID 97's group, modifier, configuration, priority and route (`apps/npu-interrupt/irq-route.c`), then installs it as a Nonsecure group 1, level-sensitive interrupt at Secure priority `0xd8`, read as Nonsecure `0xb0`, routed to CPU 0.
- At EL1, `npu_application_irq_bind` requires the source disabled, inactive and not pending, level-sensitive, at priority `0xb0` and routed to CPU 0 before it binds the service, whose worker is pinned to CPU 0.
- Before the ROM return, the monitor writes the saved route back before it restores any SPI enable (`apps/npu-finite/monitor.c`).

## Job timing

The backend records, for the latest job, at publication just before the trigger and at the worker's completion observation:

- the NPU's total cycle counter `+0x078` and idle cycle counter `+0x07C`, read in the vendor profiling build's order: total then idle at the start, idle then total at the end. The differences, modulo 2^32, are the job's total and idle NPU clock cycles; their difference is the active cycles, as the vendor computes. At 852 MHz the counters wrap after 5.0 s, longer than a job's 1 s deadline. The vendor clears the counters before each job; SunstoneOS takes differences and writes nothing;
- the system counter `CNTPCT_EL0`, 24 MHz on the A733;
- a failed counter read faults the job with `A733_NPU_COUNTER`, which the callbacks report by returning -1, distinct from a memory failure (`A733_NPU_MEMORY`, a return of 0; `include/a733_npu_controller.h`). The convolution model's `counter-fault` mode checks both codes;
- the NPU adapter's counter microseconds.

The interrupt handler also records the system counter at each delivery. After each job the client copies these values. Before CLOSE the image logs `NPU_TIME_US` (startup entry and READY, then each job's publication and completion in adapter microseconds) and `NPU_JOB_COUNTERS` (the counter frequency, then per job the system counter at publication, at delivery and at completion observation, and the total and idle cycles). The polled profile logs 0 for the delivery. The host reports both blocks in `npu_result.timing` (`apps/npu-finite/host.py`): `job_us`, and per job `start_to_irq_us`, `start_to_done_us` and `active_cycles`.

The delivery time measures the job from publication to the interrupt, excluding the worker's wake. The active cycles measure the NPU's own busy time and exclude the time between completion and the worker's reading, during which the NPU is idle.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Startup entry to READY, 852 MHz, 800 mV | 4.0 ms | [npu-irq-completion.jsonl](../evidence/npu-irq-completion.jsonl), `npu-irq-completion-board-prediction` |
| Doorbell to interrupt, 852 MHz | 2.91 ms | same |
| Host model: held `WAIT` from modelled event delivery to DONE reply | median 159 µs (emulation time) | `npu-irq-completion-model-timing` |

## Limits and open work

- The interrupt host is not root reviewed.
- The standalone finite diagnostic wakes once per tick to check its deadline.
  The integrated OS uses precise deadline waits; its physical wake count is unqualified.
- The cycle counters are those the vendor's profiling build reads; whether they count while the clock is gated between jobs does not affect a job's differences.
- The two-start set of [the interrupt runbook](../board-runbooks/npu-irq-starts.md) (492 MHz then 852 MHz, two jobs per start) has no recorded per-rate result for the 492 MHz start; the prediction is in the record above.
