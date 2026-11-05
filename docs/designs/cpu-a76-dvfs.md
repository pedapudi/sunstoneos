# Design: Cortex-A76 recognition cores, CPU operating points and in-line SGIs in the integrated OS image

Status: **implemented; host-modeled and emulated**. No board has run the integrated OS image with this design. The remaining board steps are open work. It follows [performance-architecture.md](../performance-architecture.md), implements the placement constants of [smp-heterogeneous.md](smp-heterogeneous.md) and uses the CPU clock driver of [cpu-clock.md](../cpu-clock.md). The vendor top points and the board sequence that reaches them are in [cpu-top-opp.md](cpu-top-opp.md). Measurements and predictions are in [the evidence records](../evidence/cpu-a76-dvfs.jsonl).

Three parts:

1. **A76 admission.** Every OS image places application work on cores 6 and 7 (`include/a733_smp_placement.h`) and keeps every other task on the A55 cluster (cores 0 to 5). A manifest that names the A55 cluster (`cpu.cluster` a55) places its application on A55 cores instead.
2. **Operating points.** Every OS image links the CPU clock driver. A sealed session grant, built with `tools/session.py build ... --clock-grant` and requested by the manifest's `cpu.clock_grants`, may change CPU-L (the A55 cluster) or CPU-B (the A76 cluster) to points of the pinned table. A session without the grant reads nothing of the clock tree and writes no CPU clock register or CPU rail voltage.
3. **Fewer SGIs, sent in line.** The tick asks a core to give up its time slice only when a ready task can take that core, and `vInterruptCore` writes `ICC_SGI1R_EL1` directly at EL1 instead of taking `SVC 117`.

## Placement and admission

- **Placement constants.** `include/a733_smp_placement.h` holds preprocessor constants: `A733_SMP_A55_MASK` (0x3F), the application core (6 with admission, 2 without) and the worker core (7 with admission, 3 without). A preprocessor check keeps each inside the scheduled cores. `platform/a733-smp-cached/FreeRTOSConfig.h` defines `configTASK_DEFAULT_CORE_AFFINITY` as `A733_SMP_A55_MASK`, so a task created without affinity runs only on cores 0 to 5. Application tasks take their core from the session manifest's cluster: core 6 for an A76 manifest, core 2 otherwise (`apps/os/application.c`). In the GPU build `apps/os/start.c` asserts that `OS_RECOGNITION_CORE` equals `A733_SMP_APPLICATION_CORE`.
- **Admission record.** Before the scheduler, `os_thermal_start` calls `os_cpu_admit` (`apps/os/cpu.c`), which reads the CPU clock report: the oscillator, the speed-bin fuse word (`0x0300624c`), and the PLL and mux words of CPU-L, CPU-B and the DSU in the pages at `0x08871000`, `0x08872000` and `0x08873000`. The start is refused, and the board stops, when CPU-B is not on its locked PLL at boot0's 1014 MHz. The ROM return resets CPU-L and the DSU but not CPU-B, so this check keeps the baseline at boot0's point ([cpu-top-opp.md](cpu-top-opp.md)). The clock build also reads the pattern and LFM words of CPU-L and CPU-B and takes one read-only PMIC snapshot each of DCDC3 and DCDC5 under the grant owner's exclusive rail lease.
- **Grant.** The driver's 96-byte grant record (own magic, version and size) is part of the session (`struct os_session.cpu_clock`). `apps/os/admission.c` loads it with `a733_cpu_clock_grant_load` against the board session's epoch and generation and refuses an owner equal to any other session owner. A grant names one cluster, or both clusters for the top-point path.
- **Owner.** The thermal task, which runs every tick on an A55 core, is the driver's only caller (`os_cpu_clock_step`). After the policy's first reading it initializes the driver, inspects CPU-L and CPU-B read-only, then requests each granted cluster's maximum once, in the order CPU-B, CPU-L, then the DSU when both clusters are granted. Each request waits for the cluster's own sensor and has a deadline of the request time plus 120 ms (the 90 ms the driver requires before touching the PLL, two PMIC requests and the ramp). Each pass runs the thermal step first and then one clock burst. The driver lowers a raised cluster to 1,014 MHz at THROTTLE and restores it at NORMAL while changes remain. When the session grants the performance-mode service (`cpu.perf` in the manifest, `--perf high|low` in the session) it replaces the one-request-per-cluster start ([performance-modes.md](performance-modes.md)).
- **Record.** One 64-bit word holds the applied CPU-L and CPU-B points in MHz and their voltages in mV (11 bits each) and a 20-bit completed-transition count. The owner writes it with one store-release and readers load it with one load-acquire, which is single-copy atomic on AArch64. The `CLOCK` line, produced by `os_cpu_clock_line`, prints it with the DSU rate, speed-bin code, pattern bits and the longest parked interval; a transition count that differs before and after a recognition shows a transition during the timed run.

## Interrupts and polling

No interrupt is added or rerouted. INTID 71 stays on core 0, and only the GPU build routes INTID 95 to core 6. The owner adds no wake-up: the thermal task already runs every tick and reads the sensors every 25 ms.

A frequency change does not wait a tick while the cluster is parked. In one pass, the owner repeats driver steps while they make progress, for at most 200 µs (`BURST_US`), as the USB0 service loop does ([usb0_service.h](../../apps/board/usb0_service.h)), and sleeps a tick only after that budget:

1. Park the mux on the oscillator and read it back.
2. Wait 20 µs on raw counter ticks, as boot0 waits after each CPU mux switch ([cpu-top-opp.md](cpu-top-opp.md)). The settle waits for its first time reading plus 21 µs, because the time source counts whole microseconds and a 20 µs wait on a quantized reading can end about 19 µs after the write.
3. Write N, then N with the update bit; poll update, then lock until three reads at least 3 µs apart show it, and wait 20 µs, as the vendor CCU helper does.
4. Write P, select the PLL, read back, and wait 20 µs again.

With a lock of about 20 µs the cluster stays parked for tens of microseconds and the owner sleeps no tick while parked. The worst case is about 90 ms: the 10 ms update bound, the 70 ms lock bound and the 10 ms margin. The burst bounds its polls and the settle bounds its time reads, so a stopped time source ends in RETAINED. A RETAINED result leaves the cluster on the oscillator until the session halts. Voltage ramps happen outside the parked window and take `|target - present| / 250 µV/µs` rounded up to whole milliseconds, as the vendor regulator driver waits: the A2 ramp from 800 mV to 1,050 mV waits 1 ms. Ramp waits sleep whole ticks.

A CRITICAL reading is delayed by at most one clock step (200 µs), because the thermal step runs first in each pass. After CRITICAL the owner stops calling the driver and the session halts; a cluster caught mid-transition stays at most parked on the oscillator.

## Memory, cache and barriers

Driver state and the record word are static in `apps/os/cpu.c`; only the thermal task writes them. No DMA buffer, device memory or cache maintenance is involved. The register adapter uses plain 32-bit accessors on the Device-nGnRnE pages and issues one `DSB SY` after each mux write, before the 20 µs wait that assumes the write arrived (section 1.2 of the performance architecture). That makes three per frequency change: the park, the P write and the select.

## Synchronization across cores

- One task uses the driver, the policy level and the clock leases. The clock registry, rail ledger and PMIC provider keep their manager locks for these state changes (section 1.7, heavier form).
- The worker's job and completion queues are unchanged; only their cores move.
- **In-line SGI.** `vInterruptCore` (`patches/freertos-smp-c.patch`) reads `TPIDRRO_EL0` once. When the TCB field is nonzero and either bit 56 is set (an EL1 task) or the `ullPortInterruptNestings` entry of the core in bits [55:48] is nonzero (an interrupt handler), it writes the value from `portSGI1R_VALUE` after `DSB ISHST`. Otherwise it keeps `SVC 117`, because `ICC_SGI1R_EL1` is UNDEFINED at EL0 (IHI 0069G section 12.2.21). From task context the kernel calls `vInterruptCore` inside its critical section, so the task cannot migrate between the register read and the nesting load, and an EL0 task reads zero because no handler is active on its core. A switch at IRQ exit sets bit 56 for an EL1 task through `portSET_CURRENT_TASK`. When the nesting count is nonzero the send omits `ISB`, because the handler's exception return synchronizes the context; otherwise it ends with `ISB`, as `gic_ipi_send_mask` in the pinned Linux `irq-gic-v3.c` does. `portSGI1R_VALUE` defaults to the upstream formula; the A733 configurations define it in line as `(core << 16) | 1` after asserting SGI 0.
- **Time slice.** The tick test in `patches/freertos-smp-tasks.patch` sets a core's yield only when a ready, not-running task of that core's priority admits that core. It computes the union of those tasks' affinity masks once per distinct running priority per tick. This stops time-slice SGIs to cores that no waiting task admits, for example core 6 when the recognizer and the PMIC owner share a priority.

## Qualified figures

All rows are host-model or emulation results; no board has run the image.

| Metric | Base | Measured | Record |
| --- | --- | --- | --- |
| Tick on core 0 that wakes no task: median instructions, SVC, DSB, SGI sends | 1,345, 5, 28, 5 | 554, 0, 18, 0 | `cpu-a76-dvfs.jsonl`, `cpu-a76-dvfs-host-budgets` |
| `vInterruptCore` from a handler: instructions, SVC, DSB, ISB | 161, 1, 2, 1 | 18, 0, 1, 0 | same |
| EL1 cross-core round trip (QEMU), instructions | 12,191 | 11,994 | same |
| EL0 send path | upstream | 5 instructions more | same |
| Owner ticks while parked, lock of about 20 µs | no path | 0 (parked 23 µs of model time) | same |
| CPU-B raise with a voltage write | no path | 5 clock writes, 1 PMIC write, 3 `DSB SY` | same |
| Admission | no path | 8 reads, no writes; the clock build adds 22 inspection reads and 2 PMIC snapshots | same |
| Recognition, bench recording | not measured | modeled 1.20 s CPU-only at 2,002 MHz | `cpu-a76-dvfs.jsonl`, `cpu-a76-dvfs-recognition-model` |

The checks are `checks/cpu-clock-check.py` (transition model), `checks/os-cpu-clock-native.py` (register adapter and owner request order), `checks/smp-port-cost.py`, `checks/a733-smp-cached.py --count`, `tools/check-smp.py` and `tools/check-mimus-smp.py`. Their results are in `cpu-a76-dvfs-host-checks`.

## Failure classes

| Failure | Class (section 1.8) | Result | Retained |
| --- | --- | --- | --- |
| Admission record refused | 1 | Start refused, board stops | Nothing |
| Grant record refused | 1 | Session refused at admission | Nothing |
| Transition REFUSED, or EXPIRED before the PLL is touched | 1 | Cluster stays at its point, which its voltage covers; recorded | Nothing |
| Transition RETAINED: lock or update timeout, readback mismatch, rejected PMIC write | 3 | `board_fail`; the session halts | CPU clock and rail leases, the PMIC request; the cluster stays parked |
| THROTTLE lowering refused | 1 | Not retried in that episode; CRITICAL still ends the session | Nothing |

## Limitations

- No load-based governor (no consumer; power cost unmeasured), no A76 power-down when idle, no cluster migration, and no measured clock on the recognition core (the recognizer runs at EL0 without PMU access; the record gives configured points).
- The driver refuses LFM, so it does not change N in place as the vendor driver does.
- The DSU changes only with a grant of both clusters, in the order of [cpu-top-opp.md](cpu-top-opp.md).
- Operating-point changes in the resident lifetime need a standing user approval of a qualified point and a grant that names the slot generation in place of the epoch. The GPU build runs only in the resident lifetime, so its placement table is regenerated for the point that a resident-lifetime grant qualifies.
- No source proves that parking a running cluster's mux on the oscillator works. The pinned A733 driver changes N in place with LFM and never reparents; boot0 parks each CPU mux while it programs the PLLs from CPU0.
- No source proves that lock reports the final rate with the pattern and LFM enables clear, that DCDC3 and DCDC5 settle within the vendor's 1 ms wait, or that application SGIs are delivered on this board.
- The host-side approval record for a grant (schema `a733-os-cpu-clock-approval-v1`) and the B1 record (schema `a733-os-cpu-clock-b1-v1`) are not implemented in `apps/os/prepare.py`; no tool in the tree produces them.

## Open work

1. **B1**, per board: the supervised A76 build with a grant of no cluster, recognizer on core 6 and worker on core 7, text and CRCs equal to the host reference, recognition time at boot0's CPU-B rate (about 2.10 s predicted). It needs ranks 1 to 5 and 9, ranks 7 and 8 and a reviewed physical entry record in one power cycle. Port 3-3 has not run rank 5 in its lifetime; on port 3-1 rank 7 returned and rank 8 passed ([rank-7-diagnosis.md](../board-runbooks/rank-7-diagnosis.md)). Read-only readings of port 3-3 (speed-bin code 0x04, CPU-B at 1,014 MHz, DCDC3 and DCDC5 at 800 mV, pattern and LFM clear) are in `cpu-a76-dvfs-b1-readings-3-3`; port 3-1 reads word `0x495`, also code 4 ([performance-modes.md](performance-modes.md)).
2. **A1, A2, A3**: CPU-B to 1,196 MHz, CPU-B to the bin's top point (2,002 MHz at 1,050 mV on port 3-3; about 1.20 s CPU-only recognition predicted), and optionally CPU-L to 1,196 MHz. Each needs the user's approval and a passed B1 on the same board. [cpu-top-opp.md](cpu-top-opp.md) plans the top-point run.
3. Implement the host preparation of the approval and B1 records.
4. Run the `soak8` real-time gate on a lightly loaded host.

## Sources

The SGI send follows `gic_ipi_send_mask` in the pinned Linux `irq-gic-v3.c` (`e5f0a698`). The clock and voltage sequences are the driver's, with the sources that [cpu-clock.md](../cpu-clock.md) cites (linux-orangepi `2ac08e8c` `ccu-sun60iw2-cpupll.c`, `ccu_nkmp.c`, `sun60iw2p1-cpu-vf.dtsi` V0.9, the SID node, the board device tree; u-boot `b791be84` `pmu_axp8191.c`). The 20 µs mux settle and the lock confirmation follow boot0 and the vendor CCU helper (`ccu_common.c` lines 47 to 64).
