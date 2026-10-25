# Plan: complete NPU jobs on interrupt 97 to measure job time

Status: **plan**. Nothing here is built. It needs its own performance design,
reviewed under section 4 of [performance architecture](../performance-architecture.md),
and a root review of its host before a board run. It extends rank 11 of the
[vendor action plan](../vendor-firmware/action-plan.md).

## Why

The default finite profile polls for completion once per 1 ms tick: the
service worker reads the controller once per tick while a job runs. The image
logs each job's publication and the worker's first completion observation in
counter microseconds ([852 MHz design](npu-852mhz-plan.md)), so a job shorter
than a tick reads as the time to the next poll at 492 and at 852 MHz alike.
Completion on the NPU's interrupt ends the job's measured time within the
interrupt and task-wake latency, which makes the predicted 492/852 = 0.58
compute ratio observable.

## The vendor's interrupt path

The board kernel's NPU driver completes jobs by interrupt:

- polling is off by default (`vpmdENABLE_POLLING 0`, `vip2/inc/vip_lite_config.h:592-594`);
- the device tree gives the source as SPI 65, level high, GIC interrupt 97
  (`sun60iw2p1.dtsi:2353`), and the driver requests it with `drv_irq_handler`
  (`vip2/os/linux/vip_drv_device_driver.c:956`);
- initialization enables every event, `+0x014` = `0xffffffff`
  (`vip2/vip_drv_hardware.c:509`);
- the handler reads the event word `+0x010`, which acknowledges it, records
  the bits and wakes the waiter (`vip_drv_device_driver.c:368-394`);
- the submitting task waits in `vipdrv_os_wait_interrupt`
  (`vip2/task/vip_drv_task_common.c:1053`, `vip2/os/linux/vip_drv_os.c:446`)
  and treats the AXI and MMU error bits as faults (`:1053-1075`).

## What the board run measured

The three-start lifetime (`board-session-20261025-7/port-3-3`) recorded
startup entry to READY as 4,024, 4,023 and 4,023 µs. It recorded no job time:
each job's completion value is 0, because `npu_convolution.c:346` compares the
finish command by pointer with its own copy, while the controller passes its
private copy (`a733_npu_controller.c:162`, `:231`). The job values are in the
NPU adapter's clock, which counts from the adapter's initialization
(`platform/a733/npu_native.c:86`, `:115-120`): the jobs were published 75.0 ms
and 77.0 ms after it at both rates. The 2.04 ms between the two publications
bounds the first job together with the worker's one-tick completion poll
(`npu_service.c:489`), the client's one-tick STATUS polls
(`apps/npu-convolution/main.c:93`), REAP, the output copy and compare, and the
second SUBMIT. The first step of this plan is therefore to compare the finish
command by content (`same_command`) so that the completion is recorded.

The vendor measures job time with the engine's own counters, total cycles at
`+0x078` and idle cycles at `+0x07C`, read around a submission in its
profiling build (`vip2/task/vip_drv_task_debug.c:267-268`, `:315-334`).

## What exists

The optional interrupt-driven finite profile (`apps/npu-interrupt/`,
`apps/npu-finite/build.py --interrupt`) already completes jobs on interrupt
97: an EL3 installer routes the SPI to CPU0 at a fixed priority and saves its
state, the ISR masks the source and notifies the service, and the worker
alone acknowledges `+0x010`, checks the events and full idle, and rearms. The
finite client requires three notified completions and the stopped receipt.
It keeps SunstoneOS's event mask `0xc0000001` (`a733_npu_controller.c:32`)
instead of the vendor's all-ones. That profile predates the production
startup's routing and has not run on a board.

## Planned change

1. **Build.** The interrupt profile with the production startup, at 492 MHz
   and with `--rate-852`; check that `build.py` admits both options together
   and gives each its own build identity.
2. **Timestamp.** Record the counter in the ISR, one `CNTPCT_EL0` read and one
   store, beside the backend's publication time, so the measured job time
   excludes the worker's wake latency. Log it in `NPU_TIME_US`.
3. **Checks.** Run `checks/npu-interrupt-client.py`,
   `checks/npu-interrupt-service.c`, `checks/npu-irq-route.py` and the
   interrupt host's fixtures (`apps/npu-interrupt/host-check.py`) on the new
   images, and the routing and return checks of the finite image.
4. **Host review.** The interrupt profile has its own host
   (`apps/npu-interrupt/host.py`); a root review must freeze it, as
   `application_review_1024` froze the polled host.
5. **Board.** A lifetime like `docs/board-runbooks/npu-starts.md`, with an
   IRQ97 capture before each start, the interrupt images in place of the
   polled ones, and the 852 MHz start last.

## Budgets and predictions

- Job-time resolution: the interrupt latency plus the ISR's counter read,
  predicted under 50 µs against the polled 1 ms.
- Outputs byte-identical to the polled profile at both rates.
- Exactly three notified completions per start, as the client requires.
- The 852 MHz job time at 0.58 of the 492 MHz one where the job is compute
  bound, more where memory bounds it.
- The CPU idles in the worker's notification wait while a job runs.
