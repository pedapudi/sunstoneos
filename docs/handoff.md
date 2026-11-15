# SunstoneOS subsystem index

Device-owner notification changes have passing NPU recognition, thermal capture,
GPU lifecycle, read-only SD and CPU/background records in
[NPU](evidence/npu-owner-events.jsonl), [thermal](evidence/thermal-owner-events.jsonl),
[GPU](evidence/gpu-owner-events.jsonl), [SD](evidence/sd-owner-events.jsonl) and
[other-owner evidence](evidence/other-owner-events.jsonl). Physical DMA and I2S
validation needs current nonsecure DMA access authority. Headset validation
needs an accessory and admitted input; the radio's implemented 25 MHz mode does
not meet the vendor-rate requirement. [Remaining provider waits](designs/remaining-owner-waits.md)
cover Type-C, GPIO power, PMIC and legacy diagnostics.

SunstoneOS runs FreeRTOS on the Orange Pi Zero 3W/A733. The
[accepted objective](goal.md) requires complete usable hardware and native
applications. No subsystem has complete physical qualification.
[The hardware inventory](hardware.md) defines required functions independently
of application use. Update current state in the subsystem files below. Each
subsystem file is a short summary whose board results are machine-readable
records in `docs/evidence/`; [the evidence rules](evidence.md) give the one-command
workflow, and `docs/history/` holds the superseded prose.

<a id="board-state-and-recovery"></a>
<a id="coordinator-launch-and-capture"></a>

The coordinator establishes live identity, permissions, per-port lock and failed
instance state before queue operations. Saved USB descriptors do not prove ROM
execution. Preserve both microSD cards; neither board has eMMC or UFS populated.
[The runner](board-runner.md) owns launch admission and bounded recovery.

## Subsystem status

Implemented means code exists. Board-qualified scope applies only to its named
inputs, operation and cleanup. Blocked functions state the missing prerequisite
in their subsystem file; host evidence establishes no physical qualification.

- [CPU](status/cpu.md): implemented; bounded eight-core workloads pass. Scheduler timing misses its budgets (task-lock wait up to 178 µs, 25 µs budget); the partitioned scheduler is in progress. Repeatability, sustained load and thermal/fault recovery remain open.
- [Memory/DMA](status/memory-dma.md): implemented; DRAM restoration passes on both boards, while full-map memory and device DMA qualification remain open.
- [Power/thermal](status/power-thermal.md): implemented; finite observations and rail normalization pass, while transitions and protection remain unqualified.
- [GPU](status/gpu.md): implemented; six finite queue workloads, restart, injected-timeout recovery and exact recognition pass on port 3-3. Recognition takes 327.476 ms against 135.474 ms CPU-only. Graphics and thermal recovery remain unqualified.
- [NPU](status/npu.md): implemented; startup, inference, model switching, SDK access, a 200-job sustained run, local restart and the resident queue (10.25 µs p50 per job, 26,925 jobs/s) pass. Eight-core concurrency, idle gating and repeated lifecycle remain open.
- [USB](status/usb.md): implemented; returning USB0 exchange passes. With TYPEC24 empty, the source-disabled control, a one-second 5 V pulse and a ten-second hold return normally on port 3-1 (rise 6.8 ms, fall under 2 ms). No USB peripheral is attached.
- [Storage](status/storage.md): implemented; scoped PIO/DMA reads pass, while SDR104, writes and autonomous card boot remain unqualified.
- [Radio](status/radio.md): implemented; fitted wiring, top-rate SDIO, returning shutdown and physical interoperability remain blocked.
- [Display/G2D](status/display-g2d.md): implemented; physical output, watchdog return and recovery remain unqualified.
- [Video](status/video.md): implemented; codec/deinterlace output, drain and numerical device behavior remain unqualified.
- [Audio](status/audio.md): implemented; internal I2S0 loopback, cyclic cancellation and drain, and raw-ring overrun pass on port 3-3. External microphone and headset operation remain unqualified.
- [Camera/ISP](status/camera-isp.md): implemented; no camera is attached. The ISP control with the 324 MHz video PLL passes; IRQ replay and the pixel comparison remain open. Sensors sit behind a class interface (`include/camera_sensor.h`).
- [Buses/RTC](status/buses-rtc.md): partly implemented; PMIC reads, RTC progression and watchdog records pass, while exposed I2C bindings and peripheral qualification remain open.
- [Security](status/security.md): implemented; CE exclusion, private memory, entropy evidence and returning execution remain blocked.
- [PCIe](status/pcie.md): implemented; no endpoint is available for physical qualification.
- [Application](status/application.md): implemented; paced recognition with thermal capture passes six fresh sessions. All 32 corpus recordings run exactly on the CPU at 0.028× real time aggregate; the default source pins reproduce one recording at 0.0332× on the board. Accelerator and autonomous execution remain open.

## Decisions for the owner

Each item below waits for the owner of the boards and the project. Nothing in this list blocks the other work, except
where an item says so. Recorded on 2026-11-15.

1. **How long the SD owner may hold the application's main task without the watchdog noticing.**
   - *Context.* Since 2026-11-15, the SD owner task (`apps/os/sd_owner.c`) exists from OS start and opens the card
     on the first storage call. When the main task waits for a storage request, it declares itself blocked to the
     supervisor until `OS_SD_PIECE_BOUND_US` (`apps/os/sd_hw.h`) has passed. That bound covers one write piece after
     one loader read. Past it, the wait goes on undeclared, and the supervisor stops renewing the watchdog one
     progress window later.
   - *Why it matters.* The write enable re-reads the backup sectors, and the close, or a piece that needs recovery,
     can outlast the bound. Such a request could reset the board with the card in the middle of a write.
   - *Options.* (a) Give each request kind its own bound, taken from its measured worst time, in `post`. The
     watchdog then still catches a hung request. (b) Declare every storage wait blocked until the session deadline.
     That is simpler, but a hung request keeps the board alive until the session ends. The recommendation is (a).
   - *Blocks.* The board run in item 2. The work is item 12 of [storage open work](status/storage.md).
2. **Reconnect port 3-3.** The board on USB port 3-3 is off the bus and needs a physical replug.
   - *Waiting on it.* The board qualification of the SD owner created at OS start: `storage-write-test` mode 6 on
     the scratch card, predicted to match run `1794607200843702122-d2ed7876`
     ([storage status](status/storage.md), item 11). Settle item 1 first, because the run writes to the card.
   - *Unaffected.* The board on port 3-1 runs every other stage.
3. **Old radio results that may hold the names of nearby networks.**
   - *Context.* Before commit `691e349e2` (2026-11-14), the radio image's scan record kept each access point's
     network name (SSID). The image now records only the access point's hardware address, frequency and signal
     strength. `tools/radio_result.py` refuses a record that carries any name bytes, and `tools/evidence.py` keeps
     addresses out of committed evidence. Nothing from those old results is in the repository.
   - *What remains.* 28 radio runs from before that commit keep their results in the local board queue,
     `~/.cache/sunstoneos/board-queue/entries/<run>/run/`: 27 runs from 2026-11-03, submitted by the radio staging
     work, and one from 2026-11-14. No one has opened them to check which of them hold names.
   - *Options.* Delete those runs' result files, or keep them as they are. An earlier report called them
     "the 11-06 results"; the queue holds no radio run from that date.
4. **Whether CPU0 is held to the real-time bar.**
   - *Current ruling (2026-11-14, reversible).* CPU0 is the service core. It runs the scheduler tick, the PMIC,
     RTC and supervisor services, the expiry of precise deadlines, and the USB2 and radio owners when an image
     builds them (`include/os_space.h` gives OS service tasks CPU0 by default). The latency bar applies to cores
     1-7. It takes the worst busy wake over the whole busy phase, including the first wake after a load step, as
     the maximum over three runs. A candidate passes when that maximum is at most the baseline's plus 1 µs. CPU0 is
     still measured and reported.
   - *Why.* The robots' load changes in steps, and with the services on one core the other seven can be judged on
     worst-case wake latency. CPU0's own longest busy wake in the NPU load stage was 44.96 µs with the NPU running
     and 50.79 µs without ([latency status](status/latency.md)).
   - *If reversed.* The services would need to move to other cores or have bounded run times, and the bar would
     include CPU0, which today misses it.

<a id="open-branches"></a>

## Open branches

Development paused on 2026-11-01 when the Codex workers reached their weekly usage limit. Each branch below is committed and clean. Its subsystem status file, on that branch, lists board evidence, unrun images with their README paths, pending approvals, files shared with other branches, and the next steps. A commit titled "Work in progress" is incomplete by design; its message states what is missing. Land a branch only after its listed next steps and a full integration gate pass.

| Work | Branch and tip | State | Next step |
|---|---|---|---|
| Partitioned per-core scheduler with capacity-aware placement, and the scheduler research document | `work/scheduler-rt-1101` `d6a50b42` | Topology wiring is disabled and marked work in progress; several commits await independent review. The baseline board run fails its timing budgets (loaded wake p99 159.958 µs, task-lock wait up to 139.333 µs). | Review the pending commits, finish the FreeRTOS adapter, then compare each stage on the board. |
| Driver real-time fixes: SD, serial, USB0, G2D, crypto, I2C, precise deadlines | `work/driver-rt-1101` `03c9209f` | Conflicts with `bringup`. The SD polling baseline measured p50 973 µs, p99 8.88 ms and 1.55 MB/s; the interrupt-driven candidate is not built. | Merge `bringup`, build the interrupt candidate, and run the matched read-only SD comparison. |
| NPU eight-core concurrency qualification and runner support | `work/npu-wdt-1031` `7915da3a` | Conflicts with `bringup`. The CPU control and the combined run are approved but not run. Do not land commits `38841b70` or `4a06665a`. | Reconcile the runner support with `bringup`, then run the CPU control and the CPU-plus-NPU run on port 3-3. |
| NPU idle clock gating, application-level queue access, joined streams | `work/npu-fastpath-1101` `9b2c5dbd` | The resident queue is in `bringup`. Gating is conditionally approved and not run. Three separate work-in-progress commits hold application access, the idle benchmark and fixture support. | Finish the gating build and its three reviews, then run the always-on control before the gating image. |
| GPU submission fast path on a retained ring | in `bringup` | Median submission falls from 13.458 to 3.833 µs; round trips stay near 104 µs. The fast path is enabled only when the service sets `job_irq_ack`. | Correct the queue-add expectation format, and run the recognition image against the pinned baseline. |
| Mimus CPU recognition speed and its OS pins | in `bringup`; mimus `port/sunstoneos-cpu-perf` `8da767113` (pins application `647aeb636`, engine `bcaf39ff5`) | All 32 corpus recordings are exact at 0.027888× real time aggregate and 0.032916× worst. Entry `1793570213701678324-a3d8ee82` reproduces one recording from the default pins. | Run the native race image and write the performance report. |
| Camera ISP replay and pixel comparison | `work/camera-isp-1031` `b9e9ad89` | The control passes on port 3-3. Commit `cf5b3951` (vendor CAP, READY and feedback ordering) is work in progress. | Reproduce the `0x8201` replay failure on the host, review the ordering commit, then run IRQ replay and the pixel comparison. |
| Audio on the shared probe owner, DMA ordering margin | `work/audio-1031` `8c681d9d` | The ordering run passes: worst visibility delay 170.67 µs within a 16 ms period. The CMake integration is work in progress; an unfinished fixture is on `work/os-audio-check-1101`. | Finish the fixture, rebuild, and qualify the interrupt-driven period path with idle clock gating. |
| CPU-plus-DMA latency with DMA0 started from reset | `work/latency-1101` `91a69a28` | The tip is work in progress. A Secure EL3 draft is on `work/latency-cold-security-1101` and needs review. | Finish the cold-start admission wiring, build both images, and run the CPU control and then the combined run on 3-3. |
| Display modes from EDID, DisplayPort EDID transport, sensor controls | `work/peripheral-classes-1101` `599994ff` | Generic HDMI timing is committed. DSI dividers and DisplayPort follow-ups are work in progress. New display PLL frequencies need coordinator approval before any board run. | Finish DSI validation and DisplayPort AUX EDID, then run the full gate. |
| USB host 5 V port bring-up | in `bringup` | Control, one-second pulse and ten-second hold pass on port 3-1 with TYPEC24 empty. | Rebuild the input, serial, camera and camera-JPEG images, attach the powered hub and devices, and run each class image. |
| GPU shader speed for mimus | in `bringup`; mimus `port/sunstoneos-gpu-on-cpu-perf` `3bddc3c51` | Integer shaders run recognition in 327.476 ms against 135.474 ms CPU-only. The exact FP32 variants preserve all sums but are slower (501.761 ms). | Calibrate the memory-traffic counters, then measure a CPU and GPU overlap schedule. |

The coordinator's working notes, including resume commands for each worker, are in `~/.cache/sunstoneos/coordinator-1031/scratchpad/` (`pending-followups.md`, `wrapup-workers.sh`).

Build commands belong in [the image guide](os-image.md).
Use [the validation policy](validation.md) and [reference index](reference-index.md).
[The hardware worklog](hardware-worklog-20261031.md) preserves frozen-input and
fault records. Historical claims do not authorize a new launch.
