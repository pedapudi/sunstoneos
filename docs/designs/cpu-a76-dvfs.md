# Design: Cortex-A76 recognition cores, CPU operating points and in-line SGIs in the integrated OS image

Status: **implemented and measured in host models and emulation**; no board
has run it. [The measurements](cpu-a76-dvfs-measurements.md) compare each
budget with the base tree. It
follows [performance-architecture.md](../performance-architecture.md), whose
section numbers it cites, implements the placement constants of
[smp-heterogeneous.md](smp-heterogeneous.md) and uses the CPU clock driver of
[cpu-clock.md](../cpu-clock.md) with one change, a settle delay around each
mux switch. Three changes:

1. **A76 admission.** The CMake option `A733_A76_ADMITTED`, which is also the
   compile definition, runs the recognizer on core 6 and its inference worker
   on core 7, and keeps every other task on the A55 cluster (cores 0–5).
2. **Operating points.** The option `SUNSTONE_A733_OS_CPU_CLOCK` links the
   driver into the supervised OS image. CMake refuses it with the resident
   lifetime or without `A733_A76_ADMITTED`. A sealed session may then grant
   changes of CPU-L (the A55 cluster) or CPU-B (the A76 cluster) to points of
   the pinned table. Both options are off by default. Without the second
   option no image writes a CPU clock register or a CPU rail voltage.
3. **Fewer SGIs, sent in line.** The tick asks a core to give up its time
   slice only when a ready task can take that core, and `vInterruptCore`
   writes `ICC_SGI1R_EL1` directly at EL1 instead of taking `SVC 117`.

The implementation differs from this design in these places:

- The native register adapter lives in `apps/os/cpu.c` beside the owner,
  as `apps/os/thermal.c` keeps its own; no `drivers/clock` file is added.
- The adapter issues `DSB SY` after every mux write: the park, the P write
  and the select, three per frequency change.
- The clock build's read-only inspection re-reads the clock report for each
  cluster: 11 reads per cluster, 22 in all, and two PMIC snapshots.
- `mimus_parallel_init` takes the owner and worker cores as default
  arguments of 0 and 1, so `apps/mimus/main.cc` and `checks/mimus-linear.cc`
  need no change.
- The EL0 budget is the direct cost of the send path, 5 instructions per
  EL0 send by disassembly of the QEMU port. The QEMU round trip, which
  lock-wait interleaving amplifies, is recorded with a regression guard of
  24 instructions per send.
- The settle waits for its first time reading plus 3 µs, because the time
  source counts whole microseconds; a 2 µs wait on a quantized reading can
  end about 1 µs after the write.
- The burst bounds its polls, as the settle bounds its time reads, so a
  stopped time source ends in RETAINED.
- The engine's inference-progress hook is defined once in `apps/os/cpu.c`.
- A grant names one cluster: the OS admission refuses a record that names
  both, as preparation does.

## A76 admission alone does not give the GPU design's figure

The GPU recognition design (`docs/designs/gpu-recognition.md` of the GPU
recognition package) models recognition of the 8.87 s bench recording at
0.818 s on the A76 pair at 2,002 MHz, against the goal's 0.4435 s (0.05 times
the audio duration). That figure needs:

- **A speed bin that admits 2,002 MHz.** Board `3-3` returned `0x00000496`
  at fuse word `0x0300624c` (`thermal-sid-shadow-20261018/runtime/`,
  [thermal-application.md](../thermal-application.md)): code `0x04`, column
  vf0400, CPU-B up to 2,002 MHz at 1,050 mV. Board `3-1` has not been read.
  Other bins stop at 1,794 MHz (vf0100) or 1,612 MHz (vf0300, vf0600), and
  code 0 at 1,508 MHz under the driver's envelope.
- **A granted raise.** The CPU-B rate that boot0 leaves is not recorded.
- **Both A76 cores.** One A76 at 2,002 MHz models at about 1.30 s
  (0.739 s linear, 0.565 s other stages), so the inference worker must run on
  core 7.
- **One lifetime for both.** The GPU linear build requires the resident
  lifetime. A grant needs the supervised lifetime: every change needs the
  user's approval before each run, and the stage-2 loader stamps a fresh epoch
  on every boot, so a prepared grant cannot name a resident boot.

The table below models the A76 pair with the GPU placement table. The linear
stage comes from `placement.py --summary` of the GPU package at `ad0420c`.
The other stages scale 4.582 s on one A55 at 494 MHz, assuming perfect
scaling over both cores and twice an A55's work per clock on an A76. Each row
assumes a placement table generated for that rate; an image that runs below
its table's rate, after THROTTLE or on a lower bin, does worse.

| CPU-B point | Linear stage | Other stages | Recognition | Times audio |
| --- | --- | --- | --- | --- |
| 2,002 MHz | 0.535 s | 0.283 s | 0.818 s | 0.092 |
| 1,794 MHz | 0.578 s | 0.315 s | 0.893 s | 0.101 |
| 1,612 MHz | 0.627 s | 0.351 s | 0.978 s | 0.110 |
| 1,508 MHz | 0.637 s | 0.375 s | 1.012 s | 0.114 |
| 1,014 MHz | 0.755 s | 0.558 s | 1.313 s | 0.148 |

The A55 pair at 494 MHz models at 3.438 s. This package does not reach the
goal; it supplies the core placement and the clock that the other levers
assume. The model ignores the DSU (L3) clock, which the driver never changes.

## Data flow and ownership

- **Placement.** `include/a733_smp_placement.h` (new) holds preprocessor
  constants only: `A733_SMP_A55_MASK` 0x3F and the recognition core, 6 with
  admission and 2 without, and the inference core 7, which exists only with
  admission. A preprocessor check keeps each inside the scheduled cores.
- **Default affinity.** Under `SUNSTONE_A733_OS`,
  `platform/a733-smp-cached/FreeRTOSConfig.h` defines
  `configTASK_DEFAULT_CORE_AFFINITY` as `A733_SMP_A55_MASK`; the pinned
  `FreeRTOS.h` uses `tskNO_AFFINITY` only when it is undefined. Today every
  board and OS task created without affinity, the recognizer included, may
  run on cores 6 and 7; the PMIC owner alone is pinned to core 0. With the
  constant, only the three call sites below place work on cores 6 and 7.
  In the GPU build, `apps/os/start.c` asserts that `OS_RECOGNITION_CORE`
  equals `A733_SMP_RECOGNITION_CORE`.
- **Recognizer and worker.** Under admission and without the GPU option,
  `apps/os/recognition-placement.patch` (new) creates the recognizer with
  `1 << 6`, calls `mimus_parallel_init(6, 7)` before the scheduler and
  `mimus_parallel_bind()` from the recognizer, and `cmake/a733-os.cmake` adds
  `apps/mimus/parallel_runtime.cc`. The worker's owner and worker cores, now
  fixed at 0 and 1, become those two parameters. In the GPU build,
  `recognition-dispatch.patch` places the recognizer itself and makes the
  same two calls under `A733_A76_ADMITTED`, which this package added after
  the GPU package merged.
- **Admission record.** Before the scheduler, `os_thermal_start` calls
  `os_cpu_admit` (new `apps/os/cpu.c`), which makes the eight reads of
  `a733_cpu_clock_report`: the oscillator at `0x07090160`, the fuse word at
  `0x0300624c`, and the PLL and mux words of CPU-L, CPU-B and the DSU in the
  pages at `0x08871000`, `0x08872000` and `0x08873000`. `mappings()` in
  `thermal.c` already checks the first two pages and gains the other three.
  When CPU-B is not on its locked PLL with a decodable rate, the start is
  refused, and the board stops with panic 11 before any task runs. The CPU
  clock build also reads the six pattern and LFM words of CPU-L and CPU-B and
  takes one read-only PMIC snapshot of DCDC3 and of DCDC5, under the grant
  owner's exclusive rail lease, for B1's record.
- **Grant.** With `SUNSTONE_A733_OS_CPU_CLOCK`, the driver's 96-byte grant
  record, which has its own magic, version and size, follows the thermal
  facts and any microphone facts. The session is then 904 or 1,088 bytes.
  `apps/os/admission.c` calls `a733_cpu_clock_grant_load` with the board
  session's epoch and generation, and refuses an owner equal to any other
  session owner.
- **Approval record.** `apps/os/prepare.py` writes a grant only from a
  reviewed record of schema `a733-os-cpu-clock-approval-v1`, as
  `validate_admission` does for the audio record. The record names the board
  identity and port, the image SHA-256, the session epoch, generation and
  lease, the step (A1, A2 or A3), the cluster, the highest point, the change
  count, the user's approval text and date, and an evidence reference to that
  board's B1 record. The B1 record must show the bin code, the DCDC3 and
  DCDC5 readings and clear pattern and LFM words. Preparation refuses unless
  every grant field equals the record's, the highest point is a row of that
  bin's column, and the grant's approval hash is the record's SHA-256. A
  record that grants nothing needs no approval record.
- **Owner.** The thermal task, which already runs every tick on an A55 core,
  is the driver's only caller, so no task or supervised service is added.
  After the policy's first reading it calls `a733_cpu_clock_init`. Once the
  cluster's own sensor has a reading, it requests each granted cluster's
  maximum once. The deadline is the request time plus 120 ms: the 90 ms that
  the driver requires before touching the PLL, two PMIC requests and the
  ramp. Each pass runs the thermal step first and then the clock step. The
  driver lowers a raised cluster to 1,014 MHz at THROTTLE and restores it at
  NORMAL while changes remain.
- **Record.** One 64-bit word holds the applied CPU-L and CPU-B points in
  MHz and their voltages in mV (11 bits each) and a 20-bit
  completed-transition count. One word, written with one store-release and
  read with one load-acquire, is single-copy atomic on AArch64 and needs no
  sequence protocol. The recognizer prints it in a `CLOCK` line before and
  after each recognition, through a weak hook that `apps/mimus/recorded.cc`
  calls; a count that differs shows a transition during the timed run.

## Interrupts and polling

No interrupt is added or rerouted. INTID 71 stays on core 0, and only the GPU
build routes INTID 95 to core 6. The owner adds no wake-up: the thermal task
already runs every tick and reads the sensors every 25 ms.

A frequency change no longer waits a tick while the cluster is parked. In one
pass, the owner repeats driver steps while they make progress, for at most
200 µs, as the USB0 service loop does
([usb0_service.h](../../apps/board/usb0_service.h)), and sleeps a tick only
after that budget:

1. Park the mux on the oscillator and read it back.
2. Wait 2 µs on raw counter ticks, twice the fixed delay that the mainline
   sunxi-ng CPU mux notifier makes after each switch (`ccu_mux.c`
   `ccu_mux_notifier_cb`, `delay_us = 1` in `ccu-sun50i-a64.c` and
   `ccu-sun50i-h6.c`, Linux `e5f0a698`).
3. Write N, then N with the update bit; poll update and lock.
4. Write P, select the PLL, read back, and wait 2 µs again.

With a lock of about 20 µs the cluster stays parked for tens of
microseconds and the owner sleeps no tick while parked. The worst case is
about 90 ms: the 10 ms update bound, the 70 ms lock bound and the 10 ms
margin. A RETAINED result leaves the cluster on the oscillator until the
session halts. Voltage ramps happen outside the parked window and take
`|target − present| ÷ 250 µV/µs`; on board `3-3` the A2 ramp is
`(1,050 mV − B1's DCDC3 reading) ÷ 250 µV/µs`, at most 1 ms from 800 mV. A
longer wait only delays the frequency change, so ramp waits sleep whole
ticks.

A CRITICAL reading is delayed by at most one clock step, 200 µs, because the
thermal step runs first in each pass. After CRITICAL the owner stops calling
the driver and the session halts; a cluster caught mid-transition stays at
most parked on the oscillator.

## Memory, cache and barriers

Driver state and the record word are static in `apps/os/cpu.c`; only the
thermal task writes them. No DMA buffer, device memory or cache maintenance
is added. The register adapter in `apps/os/cpu.c` uses plain 32-bit
accessors on the Device-nGnRnE pages, which the peripheral observes in
program order. It issues one `DSB SY` after each mux write, before the 2 µs
wait that assumes the write arrived (section 1.2): three per frequency
change.

## Synchronization across cores

- One task uses the driver, the policy level and the clock leases. The clock
  registry, rail ledger and PMIC provider keep their manager locks for these
  state changes (section 1.7, heavier form).
- The record word: one store-release by the owner, one load-acquire by the
  recognizer.
- The worker's job and completion queues are unchanged; only their cores
  move.
- In-line SGI: `vInterruptCore` reads `TPIDRRO_EL0` once. When the TCB field
  is nonzero and either bit 56 is set (an EL1 task) or the
  `ullPortInterruptNestings` entry of the core in bits [55:48] is nonzero (an
  interrupt handler), it writes the value from `portSGI1R_VALUE` after
  `DSB ISHST`; otherwise it keeps `SVC 117`. Two preconditions make the test
  correct. From task context the kernel calls `vInterruptCore` inside its
  critical section, so the task cannot migrate between the register read and
  the nesting load, and an EL0 task reads zero because no handler is active
  on its core. A switch at IRQ exit sets bit 56 for an EL1 task through
  `portSET_CURRENT_TASK`. When the nesting count is nonzero the send omits
  `ISB`, because the handler's exception return synchronizes the context;
  otherwise it ends with `ISB`, as `gic_ipi_send_mask` does. The path is in `patches/freertos-smp-c.patch`,
  which the QEMU SMP images also apply; `portSGI1R_VALUE` defaults to the
  upstream formula and the A733 configurations define it in line as
  `(core << 16) | 1` after asserting SGI 0, as `a733_cpu_sgi1r` computes it.
- Time slice: the seven sends per tick in the cached instruction model come
  from tasks of one priority pinned one per core. In the OS image the
  recognizer and the PMIC owner share priority 3, so core 6 would be asked
  to yield at every tick for a task that cannot run there. The tick test in
  `patches/freertos-smp-tasks.patch` sets a core's yield only when a ready,
  not-running task of that core's priority admits that core. It computes the
  union of those tasks' affinity masks once per distinct running priority per
  tick.

## Reused and removed

Reused: the CPU clock driver and grant loader, the thermal policy and task,
the rail ledger floor (set by the driver's first snapshot), the PMIC write
path, session admission and preparation, the mimus parallel worker, and the
existing clock, SMP and OS-image models. Removed: `SVC 117` for EL1 callers,
time-slice SGIs to cores that no waiting task admits, and the
`vInterruptCore` hunk of `patches/freertos-a733-smp.patch`.

## Budgets and measurement

The three changes land as separate commits so each comparison isolates one.
The tick and SGI baselines were counted on 2026-10-24 with the cached
eight-core instruction model (`checks/a733-smp-cached.py --count`, with
`vInterruptCore` and `xTaskIncrementTick` added to its counted functions) on
the diagnostic built from this branch after the bringup merge.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Default OS images, first commit | — | Loadable bytes differ only in the default-affinity constant in `tasks.c` and the build identity | `llvm-objcopy -O binary`, disassembly |
| Call sites that place work on cores 6 or 7 (A76 build) | Every task without affinity | Three: recognizer, inference worker, GPU worker | Compile-time default; the call sites named above |
| A76 admission | No path | 8 reads, no writes, once per boot; the clock build adds 22 inspection reads and 2 read-only PMIC snapshots | `cpu-clock-model.c` with the OS owner |
| CPU-B raise with a voltage write | No path | 5 clock register writes, 2 PMIC requests, 3 adapter `DSB SY` | Same |
| Owner ticks while parked | No path | 0 when the PLL locks in about 20 µs; the model rejects a PLL write without the 2 µs settle | Same |
| Longest park | No path | About 90 ms; RETAINED stays parked until the halt | Same |
| Owner wake-ups added | — | 0 | Same |
| Transitions per boot | — | At most twice the grant's change count: each THROTTLE lowering follows a raise | Model |
| Tick on core 0 that wakes no task (scheduler running): instructions, `SVC`, `DSB`, SGIs sent | 1,345, 5, 28, 5 (median) | At most 700, 0, at most 18, 0 | Instruction model, ticks with no `prvYieldForTask` entry |
| Receive work per tick on other cores | 7 × (SGI handler 15 + `vTaskSwitchContext` 305 instructions) | 0 | Same |
| `vInterruptCore` from a handler: instructions, `SVC`, `DSB`, `ISB` | 161, 1, 2, 1 | At most 80, 0, 1, 0 | Same |
| EL0 send path (QEMU port) | Upstream `vInterruptCore` | At most 5 instructions more per EL0 send | Disassembly; the QEMU EL0 round trip is recorded |
| Other QEMU port operations | Table in [smp.md](../smp.md#measured-cost) | None higher; EL1 round trip at most 12,191 instructions | Same |
| Recognition, bench recording | B1 at boot0's CPU-B rate | The table above at the board's highest point (A2) | Model; board |

Gates: `tools/check-smp.py` cases `kernel`, `context` and `soak8` (ten trials)
on `cortex-a55` and `cortex-a76`, `checks/cpu-clock-check.py`, the
`checks/os-image-*` checks, and the link check of every OS build. The model
gains a case in which another core's nesting count is nonzero while an EL0
task sends; that send must still take `SVC 117`.

## Failure classes

| Failure | Class (section 1.8) | Result | Retained |
| --- | --- | --- | --- |
| Admission record refused | 1 | Start refused, panic 11 | Nothing |
| Grant record refused | 1 | Session refused at admission | Nothing |
| Transition REFUSED, or EXPIRED before the PLL is touched | 1 | Cluster stays at its point, which its voltage covers; recorded | Nothing |
| Transition RETAINED: lock or update timeout, readback mismatch, rejected PMIC write | 3 | `board_fail`; the supervised session halts | CPU clock and rail leases, the PMIC request; the cluster stays parked |
| THROTTLE lowering refused | 1 | Not retried in that episode; CRITICAL still ends the session | Nothing |

## Left out

- **A load-based governor**: no consumer, and the power cost is unmeasured.
- **DSU clock changes**: they return if board timing shows the A76 pair
  limited by the L3.
- **Operating-point changes in the resident lifetime, and so in the GPU
  build**: they need a standing user approval of a qualified point and a
  grant that names the slot generation in place of the epoch.
- **A measured clock on the recognition core**: the recognizer runs at EL0
  without PMU access; the record gives configured points.
- **Changing N in place with LFM**, as the vendor driver does: the driver
  refuses LFM.
- **Cluster migration and A76 power-down when idle**: no consumer.

## Hardware sequences and their sources

The SGI send follows `gic_ipi_send_mask` in the pinned Linux `irq-gic-v3.c`
(`e5f0a698`, `cpu-audit-drivers-irqchip-irq-gic-v3-c`). IHI 0069G §12.2.21
(pinned) makes `ICC_SGI1R_EL1` UNDEFINED at EL0, which is why EL0 tasks keep
the supervisor call. The clock and voltage sequences are the driver's, with
the sources that [cpu-clock.md](../cpu-clock.md) cites (linux-orangepi
`2ac08e8c` `ccu-sun60iw2-cpupll.c`, `ccu_nkmp.c`, `sun60iw2p1-cpu-vf.dtsi`
V0.9, the SID node, the board device tree; u-boot `b791be84`
`pmu_axp8191.c`). The 2 µs mux settle cites the mainline notifier above; the
implementation pins `ccu_mux.c`, `ccu-sun50i-a64.c` and `ccu-sun50i-h6.c`. No
source proves:

- parking a cluster's mux on the oscillator while that cluster executes. The
  pinned A733 driver changes N in place with LFM and never reparents at run
  time; the mainline precedent is for other Allwinner SoCs;
- that lock reports the final rate with the pattern and LFM enables clear;
- DCDC3 and DCDC5 settling at the device tree's 250 µV/µs;
- the CPU-B rate and the pattern and LFM words that boot0 leaves, and board
  `3-1`'s fuse code;
- application SGI delivery on this board. The supervisor-call path writes
  the same register at the same exception level and is also unproven.

## Board steps

Each step schedules sustained work on cores 6 and 7, which no physical run
has shown. Every step therefore requires, on the same board, that rank 7 (the
all-core first-task diagnostic) has returned and that rank 8 (task bodies,
floating point, SGIs and the timer on all cores) has then passed. On port 3-1, in one lifetime on 2026-10-24, the corrected rank 7
diagnostic returned and the rank 8 workload passed, after the CPU0-only
preparation and the all-eight-core pre-scheduler; a later image of that
lifetime also delivered one timer interrupt to each secondary
([rank-7-diagnosis.md](../board-runbooks/rank-7-diagnosis.md)). Port 3-3
has run neither. The OS image is a
Terminal test, so each step also needs ranks 1 to 5 and rank 9 earlier in the
same power cycle ([board test plan](../audit/board-test-plan.md)). Every
supervised session ends in whole-board power removal, so no raised point
survives into a later image.

Without a clock or voltage change:

- **B1**, per board. The supervised A76 build with the CPU clock option and
  a record that grants nothing. It shows the recognizer on core 6 and the
  worker on core 7 with text and CRCs equal to the host reference, and every
  service reporting while the tick and SGIs use the new paths. It records
  the recognition time from counter ticks at boot0's CPU-B rate, the `CLOCK`
  lines before and after it, the fuse code, the CPU-B PLL, mux, pattern and
  LFM words, and the DCDC3 and DCDC5 readings. This time is A2's baseline.

B1 has a read-only part that needs no core 6 or 7: the fuse (bin) code, the
CPU-L, CPU-B and DSU PLL and mux/P words, the CPU-L and CPU-B pattern and LFM
words, and the DCDC3 and DCDC5 readings. Board bundles 14 and 15 of
[board-bundles-next.md](board-bundles-next.md) read them: bundle 14 through
FEL `readl` and bundle 15 through the qualified PMIC provider in a returning
CPU0 image. They can run before ranks 7 and 8, and their records supply the
bin column of the approval record and the DCDC3 selector that a selector write
must find. They are not B1: B1 is the supervised A76 run with the recognizer
on core 6 and the worker on core 7, which still needs ranks 7 and 8 on the
same board.

On port 3-3 on 2026-10-25, bundles 14 and 15 recorded:

| Item | Word or reading | Decoded |
| --- | --- | --- |
| Speed bin | SID `0x0300624c` = `0x496` | Code `0x04`, column 4 |
| CPU-B | PLL `0xf8802700`, mux/P `0x03000000` | Source PLL, N = 39, P = 0: 1,014 MHz from the 26 MHz oscillator |
| DSU | PLL `0xf8802600`, mux/P `0x03010105` | N = 38, P = 1: 494 MHz |
| CPU-L | PLL `0xf8802600`, mux/P `0x03010000` | 494 MHz |
| Pattern and LFM | CPU-L and CPU-B: `+0x4` = 0, `+0x8` = `0x30000000`, `+0x18` = `0x40000001` | Pattern type, pattern LFM enable and LFM ramp enable all clear |
| DCDC3, DCDC5 | Enable `0xff`, selector `0x9e` each | Enabled, 800 mV each |

B1 itself on port 3-3 still needs, in one power cycle:

- rank 5, the forced-stall return, which this lifetime has not run; ranks 1
  to 4 and rank 9 have run in it (cold DDR `s7-ddr`, the base kernel check
  `s7-bb2-run`, native USB `s13-r9-usb`);
- ranks 7 and 8, which set `board-bundles-20261025-2` runs;
- the supervised integrated OS image built with `A733_A76_ADMITTED` and the
  CPU clock option, with a grant record that names no cluster, prepared by
  `apps/os/prepare.py` from a reviewed entry record (the thermal owner's
  facts from a fresh reading and the DDR reservation of the recognition
  inputs). No integrated OS image has run on a board, and this one takes
  over USB0 and ends the session, so it runs last and the board then needs
  a power removal;
- the B1 record of schema `a733-os-cpu-clock-b1-v1` that `apps/os/prepare.py`
  checks before any A step: B1 passed with a grant of no cluster on that
  board and port, with clear pattern and LFM words.

Each of the following needs the user's explicit approval before the run,
runs last in a connection window after B1 has passed on that board, and is
specific to that board through its approval record:

- **A1.** CPU-B to 1,196 MHz, which needs 800 mV in every bin; the driver
  writes a voltage only if DCDC3 is below that. This is the first park and
  relock of a running cluster.
- **A2.** CPU-B to the highest point of that board's bin (2,002 MHz at
  1,050 mV on board `3-3`), with the DCDC3 write and ramp, and recognition
  timed at that point. The recognizer starts the timed run only after the
  clock owner has finished its requests, and the `CLOCK` lines before and
  after the run show the point and the largest parked interval. A2 runs the
  supervised build without the GPU, so its target is CPU-only recognition
  on the A76 pair: 14.894 s × 494 ÷ (2 × 2 × 2,002) for the linear stage
  plus 0.283 s for the other stages, about 1.20 s at 2,002 MHz. When
  recognition is CPU-bound, B1's time divided by A2's equals 2,002 MHz
  divided by B1's CPU-B rate; a smaller ratio shows a limit outside the
  cores, such as the DSU clock.
- **A3**, optional. CPU-L to 1,196 MHz, the first trial that
  [cpu-clock.md](../cpu-clock.md) names, for the service cores.

## Files each package changes

| File | This package | GPU recognition package |
| --- | --- | --- |
| `cmake/a733-os.cmake` | Option block after the mSBC check, away from the option lines that the GPU package edits; the worker source and definitions after the headset definitions; one identity append after the input-hash loop | Its option checks, sources, definitions and identity string |
| `docs/os-image.md` | A new section before "Build and checks" | Its existing sections |
| `apps/os/start.c`, `thermal.c`, `admission.c`, `os.h`, `prepare.py`; new `cpu.c`, `recognition-placement.patch` | Yes | No |
| `apps/os/dispatch.c`, `dispatch.h`, `identity.c`, `services.c`, `placement.py`, `application.patch`, `recognition-dispatch.patch`; new `gpu.c`, `gpu_hook.cc` | No, except the A76 calls below | Yes |
| `apps/os/recognition-dispatch.patch`: `mimus_parallel_init` on the recognition and inference cores, `mimus_parallel_bind()` and the inference progress hook, under `A733_A76_ADMITTED` | Yes, after the merge | Its other hunks |
| `include/a733_smp_placement.h` (new) | Yes | No; `start.c` checks the GPU build's `OS_RECOGNITION_CORE` against it |

This package also changes `drivers/clock/a733_cpu_clock.c` and its header
(the 2 µs settles, the parked time, the read-only inspection and the burst),
`platform/a733-smp-cached/FreeRTOSConfig.h`,
`platform/a733-smp/FreeRTOSConfig.h`, `patches/freertos-smp-c.patch`,
`patches/freertos-smp-tasks.patch`, `checks/cpu-clock-model.c`,
`checks/a733-smp-cached.py`, `checks/smp-port-cost.c` and `.py`,
`checks/smp-sgi-send.py` (new), `checks/os-image-model.c`,
`checks/os-image-launch.py`, `docs/cpu-clock.md`, `docs/smp.md` and
`docs/os-image.md`.

## Changes outside this package

These land through `primary.patch`:

- `patches/freertos-a733-smp.patch`: delete the `vInterruptCore` hunk.
- `apps/mimus/parallel_runtime.cc` and `parallel_runtime.h`: the owner core
  and worker core become parameters of `mimus_parallel_init`, with defaults
  0 and 1, and the `WORKERS` line's core mask follows them.
- `apps/mimus/recorded.cc`: in native builds, `report_clock` prints the line
  from a weak `os_cpu_clock_line` hook, before and after recognition.
- `checks/index.json`: regenerated for `checks/smp-sgi-send.py`.
- `cmake/a733-storage-maintenance.cmake`: the pinned SHA-256 of
  `platform/a733-smp-cached/platform.c`, which no longer defines
  `a733_smp_sgi_value`; the storage platform patch still applies with
  `--fuzz=0`.

Other owners: the GPU package regenerates its placement table with
`--cpu-mhz` for the point that A2 qualifies once a resident-lifetime grant
exists, because the GPU build runs only in the resident lifetime.
