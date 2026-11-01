# Driver completion, CPU cost and idle wakeups

**Status: implemented.**

**Board-qualified scope:** Physical driver timing and idle power are unmeasured.
Host models cover the selected ready-step, notification and deadline changes.

This audit is for developers familiar with C, FreeRTOS and DMA ownership.
It covers the I/O paths under `drivers/`, `platform/`, `runtime/` and `apps/os/`.
The linked findings identify loops, service hops, critical regions and copied
buffers that need workload-specific review. Source occurrences establish
neither execution frequency nor a worst-case bound.

Costs below are counted operations or conditional estimates from the code.
One tick means 1 ms at the configured 1 kHz scheduler. Scheduling, device time
and contention add to these estimates. None is a board latency measurement.
Fixed means the named defect has host evidence; the complete device path can
still have open real-time requirements. Current physical support and authority
remain in the linked subsystem status files.

## Rank fixes by the workloads they delay

| Rank | Workload and location | Baseline cost, or current cost for open findings | Fix and current status |
| ---: | --- | --- | --- |
| 1 | UVC MJPEG setup: `apps/os/usb-jpeg-native.c:59`, `drivers/video/a733_video_jpeg_run.c:169` | One of 659 ready programming writes per owner pass; about 659 ms if each pass sleeps one tick, before decode/copy time. | **Fixed for ready programming.** Both JPEG callers use a bounded advancing poll. The model completes all 659 writes in one call, then samples the pending task-end status once. Hardware completion, copies and repeated OS frames remain open. |
| 2 | USB serial/UVC host submissions: `apps/board/host_irq.c`, `drivers/usb/a733_xhci.c` | Ready startup/address/configuration/submission steps previously slept one tick apiece. The delayed-register control counts 19 startup sleeps and 14 sleeps on ready steps. | **Raw class waits fixed in host checks.** At most 32 ready passes precede a precise wait; IRQs notify the owner directly. Noninterruptible startup and deferred GIC acknowledgment use bounded 50 µs retries. Earlier device deadlines remain authoritative. The JPEG profile retains its existing cadence until decoder readiness/IRQ81 is connected. |
| 3 | USB serial TX and storage/report mailboxes: `apps/os/usb-service.c`, `apps/os/client.c` | One serial TX per endpoint; EL0 applications still submit through the owner mailbox. Sector reads copy 512 bytes. | **Partly fixed.** An admitted EL1 caller can submit retained serial buffers directly under bounded native ownership. Storage submission/completion and keyboard consumption/publication notify their waiters. A bounded EL0 privileged submission path and multiple TX tickets remain open. |
| 4 | USB0 device exchange: `apps/board/usb0_service.h:79`, `apps/board/usb0_service.h:109`; legacy `drivers/usb/diagnostic.c:284` | Up to 200 µs of active polling, then a tick sleep; idle service still polls every tick. A packet arriving after a burst can wait nearly a tick. | **Open.** Deliver device IRQs to the owner and notify application-reply publication. A precise deadline must cover protocol and health timers. Measure any short spin against the IRQ path before retaining it. |
| 5 | SD streaming/update worker: `platform/a733-storage-update/worker.c:45`, `drivers/storage/sunxi_mmc.c:355`, `drivers/storage/fatfs_port.c:88` | One storage-service advance per tick. PIO advances at most one 512-byte block per poll: at one poll/tick its ideal ceiling is 512,000 bytes/s. FatFs can busy-poll without an idle callback. | **Open.** Route SMHC command/data/IDMA completion to its owner, wake requests directly, and batch ready state transitions. Use existing multi-sector DMA extents. Storage-write behavior requires separate coordinator review; no write path changes here. |
| 6 | DMA clients: `drivers/dma/dma_service.c:528`, `drivers/dma/dma_service.c:328`, `drivers/dma/a733_dma_controller.c:90`, `drivers/dma/a733_dma_controller.c:260` | Each API request queues to a worker and receives a reply. Submission revalidates mappings/conflicts and maintains complete admitted buffer extents; completion scans channels and invalidates complete outputs. Cost grows with mappings, channels and bytes. | **Open; coordinate with latency owner.** Keep the published 25 µs masked-region and 250 µs DMA delivery targets. Admit immutable profiles once, submit from caller context and deliver completion fences. Measure cache work before changing ownership. `drivers/dma/` is unchanged. |
| 7 | Audio kit service: `apps/os/audio.c:90`, `apps/os/audio.c:207`, `apps/os/capture.c:105` | Nonblocking DMA send/collect is retried by a one-tick owner loop; audio commands add another service hop. Cyclic buffers already avoid an extra capture copy. | **Handed to audio owner; unchanged.** Notify the audio owner on mailbox publication and DMA period/reply completion, preserve serializer drain and use a precise stop deadline. |
| 8 | Display validation: `drivers/display/a733_display_native.c:10`, `drivers/display/a733_display_native.c:176`; replay caller `apps/dsi-scanout/native.c:270` | Each full 720p double-buffer walk reads 8192 uncached table words and checks 1810 CPU pages, requiring 3620 AT translations. Replay repeats that walk across a phase change. Its caller also advances ready DE steps once per tick. | **Duplicate walks fixed.** Validate once per prepare/request/replay; keep live allocation/IOMMU metadata checks per access and revalidate corruption on the next operation. Model prepare/start/stop walks fall from 13 to 3. Direct replay admission performs one walk. Tick-paced replay and per-MMIO checks remain open. |
| 9 | G2D readout: `apps/g2d/native.c:220`, `apps/g2d/native.c:233`, `apps/g2d/memory.c:22` | Output copies 32 KiB per tick. Two 24 KiB guard scans per chunk add at least 48 KiB of uncached reads; a 720p XRGB frame needs 113 copy wakes and at least 5.30 MiB of guard reads. | **Open.** Use task-end IRQ notification, retain the vendor mixer-reset order, and verify immutable guards once at each ownership boundary. Bound and batch copied ranges; preserve IOMMU-fault and tail checks. The driver already emits an eight-header RCQ batch. |
| 10 | Small crypto operations: `drivers/crypto/a733_ce_native.c:97`, `drivers/crypto/a733_ce_native.c:120` | A 16-byte AES task maintains 7872 bytes of maximum-sized cache ranges, 492 times its payload; 32-byte random generation maintains 2560 bytes. Counts exclude stores and copies. | **Open.** Carry authenticated dirty/input/output lengths into native synchronization, preserving key/output scrubbing and cache-line isolation. Use CE completion IRQ notification and resident descriptors; qualify private memory and entropy independently. |

## USB qualification follow-ups preserve the physical-test order

The [USB status](usb.md) records coordinator approval and reviewed empty-port
control, one-second and ten-second diagnostics. The updated runner checks PL8
on every return. These images await coordinator execution through that runner;
they do not start xHCI, PHY or DMA.

Reviewed close/drain commit `13897638` consumes endpoint and command completions,
finishes port resets, excludes enabled Debug Capability and observes HCHalted
within 16 ms. The normative xHCI halt condition covers pending pipelined
transactions; an unspecified additional fabric acknowledgment is unnecessary.
Borrowed pages stay retained. Native memory reclamation and IRQ synchronization
remain to integrate after the empty-port runs.

Reviewed JPEG commit `3d33b607` supplies decoder INTID 81 wakeups and precise
readiness/deadlines. It awaits owner-waiter integration. EL0 serial submission
still needs a safe privileged continuation and nonblocking provider admission.
PHY idle gating and physical timing/power measurements remain open.

## Remaining driver families and shared services

The USB host qualification stream owns `drivers/usb/a733_xhci*`,
`drivers/usb/a733_usb2*`, `drivers/usb/host/`, `apps/board/host_irq.*`,
`apps/os/usb-service.c` and the corresponding host-class integration in
`apps/os/client.c`. The driver real-time stream owns the remaining USB0
findings, including `apps/board/usb0_service.h`, `drivers/usb/diagnostic.c`
and the common precise-deadline service. Host integration uses its committed
[`sunstone_deadline.h`](../../include/sunstone_deadline.h) interface. The host
integration also bounds health after a lost or late timer IRQ in
`apps/os/services.c`; coordinator integration must retain that correction.
USB0 device changes remain with their owner and are not implemented here.

| Area and location | Finding, cost and required action | Status |
| --- | --- | --- |
| USB owner checks: `drivers/usb/a733_xhci.c:73`, `drivers/usb/a733_usb2_native.c:96` | Each checked MMIO operation performs two provider guards, four time samples and two full-system barriers before contention retries. Provider BUSY can spin for up to 2 ms. Validate an immutable admission record once and use explicit revocation; measure the weakest correct barriers. Direct DMA leases and serial receive loans already avoid some bounce copies. | Open. |
| USB decoded-frame lifetime: `apps/os/usb-jpeg-native.c:13`, `apps/os/usb-jpeg-native.c:34` | The OS decoder accepts one frame per image, allocates once, clears the full 512 KiB input capacity and gathers slice bytes into uncached input. It cannot pipeline repeated frames. | Open. Reuse admitted guarded mappings for supported repeated geometry and reserve output slots before accepting frames. |
| Codec register path: `drivers/video/a733_video_platform.c:223`, `drivers/video/a733_video_unit.c:327` | JPEG's 659 programming writes take 659 lock pairs and mapping checks, at least 1318 DSB SY barriers, and repeated clock/domain metadata checks. Registry contention can spin for 2 ms. H.264, HEVC and encoders batch ready steps but still poll hardware completion in their callers. | Open. Keep vendor write order and completion checks; establish per-job register ownership before batching accesses. |
| Video/DI retained output: `drivers/deinterlace/a733_deinterlace.c`, `drivers/video/a733_video_jpeg_run.c:185` | Pending finish/readout callbacks preserve buffers. No code inspection establishes independent transaction drain or safe clock/domain release. | Open. Deliver completion events without declaring retained buffers reusable. |
| I2C: `drivers/i2c/a733_twi.c:46`, `drivers/i2c/a733_twi_native.c:57`, legacy `drivers/i2c/a733_i2c.c:40` | TWI is a bounded phase machine, but post-access authority BUSY loops run until the transaction deadline. The legacy read spins for up to 10 ms per phase/STOP deadline. PMIC has a separate provider and optimized ordered accesses. | Open. Add controller completion/STOP notifications and preserve arbitration-loss behavior. Do not collapse hardware flags into successful writes. |
| SPI: `drivers/spi/a733_spi.c:189`, `drivers/spi/a733_spi_platform_native.c:61` | FIFO work has a caller-supplied budget and a DMA path exists. Native per-access owner/mutex checks remain, and the core does not itself schedule pending completion. One transfer occupies each controller. | Open. Keep IRQ/FIFO budgets, retain DMA descriptors, and connect caller fences before expanding queue depth. |
| UART/Bluetooth: `drivers/uart/a733_uart.c:123`, `drivers/uart/a733_uart_platform_native.c:43`, `apps/os/headset.c:93` | FIFO operations batch deadline checks and IRQ/DMA paths exist. Native register access repeats task and mutex-holder checks. Headset's module owner polls every tick; its client also has a 250 ms fallback wait. | Open; headset capture handed to audio owner. Preserve TEMT, RX idle and shared-radio ownership before teardown. |
| RTC: `drivers/rtc/a733_rtc.c`, `drivers/rtc/a733_rtc_initialize.c`, `include/a733_rtc_service.h` | Calendar/alarm core has no tick wait or heap call. Delayed write/clock acknowledgment still needs caller progress. Calendar conversion loops are bounded by the supported date range. Alarm notifications need physical timing evidence. | Reviewed; no new hot-path fix. See [bus/RTC status](buses-rtc.md). |
| PWM: `drivers/pwm/a733_pwm.c:197`, `drivers/pwm/a733_pwm.c:308` | PERIOD_RDY and stop/restart remain explicit pending states; IRQ notice has a distinct path. Steady output needs no periodic rewrite. Fan-off current and pending-state wake bounds are unmeasured. | Reviewed; preserve delayed completion and qualify output/idle behavior. |
| ADC/pins: `drivers/adc/a733_adc.c:96`, `drivers/adc/a733_lradc.c`, `drivers/pinctrl/a733_pins.c` | ADC uses the source-defined sampling divider and interrupt status. Pin ownership is fixed-capacity. No heap or mandatory tick sleep occurs in these cores; owner cadence and electrical behavior remain separate. | Reviewed; LRADC electrical authority is blocked. |
| PCIe/NVMe: `drivers/pci/a733_pcie_nvme.c:61`, `drivers/pci/a733_pcie_nvme.c:150` | The finite queue has one pending command despite its resident queue depth. Completion records exist and data is uncached; the caller must schedule polling/IRQ delivery. | Open. Add multiple admitted read tickets and direct completion fences after endpoint qualification. No endpoint is available; no writes are authorized. |
| Network: `drivers/net/sunstone_net.c:373`, `drivers/net/sunstone_net.c:384`, `drivers/net/sunstone_net.c:438` | Normal worker dequeues one request per tick: at most 1000 requests/s before device work, with a service hop. Owned mode uses the shared radio owner. Supplicant/OpenSSL allocation is in authentication paths; data-path buffer ownership still needs size-specific measurement. | Open. Notify submissions/SDIO completion and batch bounded request rings; keep one shared module owner. |
| Application kit: `apps/os/application.c`, `apps/os/services.c` | USB and host notification waits use the architectural timer. A registered owner's actual wait sequence and absolute operation bound can replace periodic progress. | **Host-checked.** Lost/late timer delivery cannot extend the alarm plus service observation window. Real progress clears the certificate. Physical wake latency and other application wait loops remain unqualified. |
| Platform synchronization: `platform/a733/shared_locks.c:35`, `platform/a733/counter_clock.c:44` | Common register-lock contention retries with one-tick waits; the monotonic counter uses an unbounded CAS loop. Task-context checks mask interrupts briefly. None has a measured board maximum. | Open. Separate unrelated resource locks and measure holders against the latency budget. Preserve first-fault inputs and bounded hardware deadlines. |
| Runtime: `runtime/newlib.c:68`, `runtime/newlib_smp.c:55`, `runtime/new_delete.cc:23`, `runtime/device_memory_pool.c` | `_write` calls the platform output function once per byte. C/C++ allocation remains available to applications; no blanket allocation-free claim applies. Device-pool acquisition belongs at open, and the pool requires caller serialization. | Open for hot logging. Use bounded buffered UART output and keep allocation outside I/O submission. |
| Clock/power/DRAM/watchdog: `drivers/clock/`, `drivers/power/`, `drivers/dram/`, `drivers/watchdog/`, `platform/a733/dma_pool_session.c:152` | Setup and recovery poll pending transitions; clock/provider registries use bounded-size inventories. DMA startup's tick wait is outside normal transfer submission. Retained device lifetimes prevent safe idle gating without drain evidence. | Reviewed. No clock, voltage, domain, reset, storage-write or operating-point change. Idle current and transition latency remain unmeasured. |
| Observation/launch platforms: `platform/a733-smp-observe/usb_service.c:85`, `platform/a733-smp-cached/perf_session.c:157`, `platform/a733-storage-update/usb.c:211` | Observation and storage USB tasks poll each tick. The explicit latency diagnostic masks interrupts for 5500 µs. Boot/launch park loops and terminal fault paths do not represent normal request completion. | Open for polling services; keep the long-mask diagnostic outside production timing qualification. |
| GPU owner: `apps/os/gpu.c:538`, `drivers/gpu/gpu_queue.c:213` | Mailbox/queue paths retry each tick; service dispatch and cache work need owner measurements. | Handed to GPU owner; unchanged. |
| NPU owner: `apps/os/npu-client.c:93`, `runtime/npu_app.c:43`, `drivers/npu/npu_service.c:495` | Mailbox publication lacks an owner notification; normal dispatch can wait nearly one tick. Service fallback/open/close paths also use tick timeouts. | Handed to NPU owner; unchanged. |
| Camera ISP: `drivers/camera/a733_camera_capture.c`, `drivers/camera/a733_isp0.c`, `drivers/camera/a733_camera_dma.c` | Fixed DMA/frame ownership exists. IRQ delivery, ongoing frame scheduling and repeated processing need the camera owner's current evidence. No camera is attached for physical timing qualification. | Handed to ISP owner; unchanged. |
| Thermal: `apps/os/thermal.c:310`, `drivers/thermal/a733_thermal.c` | The OS owner checks the sampling state every tick. Sampling deadlines and hardware conversion time require the thermal owner's current qualification. | Handed to thermal owner; unchanged. |

## Host checks preserve asynchronous hardware behavior

The JPEG check uses the actual executor with a task-end status bit that remains
stale until explicit model time advances. Completion delays are 1, 17 and
2000 µs; a deferred copy remains pending, and stuck completion retains its token
at the deadline. Restoring one-step programming fails the write-count assertion.
The shared advancing helper has no sleep, allocation or hardware-wait loop.

The USB check executes the actual owner waiter and xHCI core. Reset and run
status remain stale for 3 ms and 2 ms. Command and transfer records arrive after
4.5 ms and 3.5 ms. It covers longer/stuck completion, requests queued after idle,
provider contention, the batch limit and deadline retention. Restoring the old
wait condition fails the ready-step assertion. It does not model physical IRQ
entry latency or qualify the remaining tick timeouts.

The display check executes prepare/start and failure paths through the native
binding, allocator and IOMMU. It checks every frame/table page once per operation,
then directly checks two-frame replay admission across its phase change.
Changed IOMMU metadata, lost CPU mappings and corrupted table words still refuse
the next operation. Compiling the current check against the old phase-keyed
provider fails its walk-count assertion. Hardware replay completion is outside
the added admission case.

Raw source inventory, commands, fixtures, input hashes and model results are
under `~/.cache/sunstoneos/build/driver-rt-1101/`. The focused evidence directories
are `jpeg-advance-1/`, `display-native-replay/`, `usb/ready-check-v2/` and
`review/`. Independent reviews approve all three production changes.
The USB-class/JPEG/IRQ OS configuration builds with external applications cleared
under `usb-jpeg-compile/`; its build log records a peak child RSS of 143548 KiB.
The JPEG host check peaks at 112532 KiB. These jobs use no containers.
The recorded host observations before and after the builds show 114 GiB
available, above the required 32 GiB reserve.

The supervised, resident and resident-audio baselines pass. The selected
integration gate also passes kernel, one SMP soak trial, shared helpers,
progress, check-index and PMIC/power-domain source checks. Its records are under
`integration/gate-driver-realtime-final/`; the gate takes 20.89 s and records
142584 KiB peak child RSS. These checks establish software integration only.

## Board measurements require admitted complete device paths

Live queue status permits port 3-3 at the initial check; port 3-1 has other work.
An available port does not supply peripheral or source authority.
[USB host status](usb.md) requires an admitted VBUS/source/PHY/rail session,
exclusive DMA memory, attached class devices and a supervised host launch path.
[Video status](video.md) lacks physical decoder/output/drain qualification.
[Display status](display-g2d.md) has no selected MIPI panel and lacks output and
return qualification. No board-run artifact is frozen while these prerequisites
are missing.

| Required comparison | Baseline queue entry | Candidate queue entry | p50 / p99 / maximum latency | Throughput / CPU cost | Idle behavior |
| --- | --- | --- | --- | --- | --- |
| USB serial/UVC ready submissions | Not submitted: source/cutoff, peripherals and returning host lifetime missing | Same prerequisites | Unmeasured | Unmeasured | Raw host uses IRQ/precise deadlines; PHY gating and idle current remain unqualified |
| UVC hardware JPEG decode | Not submitted: admitted camera/decoder lifetime missing | Same prerequisites | Unmeasured | Unmeasured | Retained leases; idle current unmeasured |
| Display operation/replay validation | Not submitted: selected sink/authority/return path missing | Same prerequisites | Unmeasured | Unmeasured | Retained mappings; idle current unmeasured |

After those prerequisites exist, queue a control with the subsystem disabled,
then unchanged-workload baseline and candidate images on the same eligible port.
Use 1000 completed serial transfers, 300 UVC frames and 300 display presents;
report nearest-rank p50/p99, the maximum, bytes or frames per second, caller and
owner CPU cycles per operation, and wakeups/current over a 5 s idle interval.
Pin payloads, clocks, voltage, image hash and Git revision. Timing must compile
out when disabled. Stop on the first transport failure or paused board, and
do not resubmit the same image for the same hypothesis. These proposed counts
are not evidence;
the one-frame OS JPEG lifetime must support repetition before its batch can run.
