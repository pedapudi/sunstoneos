# Design: Cortex-A76 recognition cores, CPU operating points and in-line SGIs in the integrated OS image

Status: **design**; no code exists yet. It follows
[performance-architecture.md](../performance-architecture.md), whose section
numbers it cites, implements the placement constants of
[smp-heterogeneous.md](smp-heterogeneous.md) and calls the CPU clock driver of
[cpu-clock.md](../cpu-clock.md) without changing it. Three changes:

1. **A76 admission.** The build option `SUNSTONE_A733_OS_A76` defines
   `A733_A76_ADMITTED`, runs the recognizer on core 6 and keeps every other
   task on the A55 cluster (cores 0–5).
2. **Operating points.** The build option `SUNSTONE_A733_OS_CPU_CLOCK` links
   the driver into the supervised OS image; CMake refuses it with the
   resident lifetime. A sealed session may then grant changes of CPU-L (the
   A55 cluster) or CPU-B (the A76 cluster) to points of the pinned table.
   Both options are off by default. Without the second option no image
   writes a CPU clock register or a CPU rail voltage.
3. **In-line SGI.** `vInterruptCore` writes `ICC_SGI1R_EL1` directly when it
   runs at EL1. This removes the seven `SVC 117` calls left in each tick.

## A76 admission alone does not give the GPU design's figure

The [GPU recognition design](gpu-recognition.md) models recognition of the
8.87 s bench recording at 0.818 s on the A76 pair at 2,002 MHz, against the
goal's 0.4435 s (0.05 times the audio duration). That figure needs three
conditions:

- **A speed bin that admits 2,002 MHz.** Only the vf0200, vf0400, vf0500,
  vf0401 and vf0501 columns of the pinned table do. vf0000 stops at 1,976 MHz,
  vf0100 at 1,794, vf0300 and vf0600 at 1,612, and fuse code 0 (an
  unprogrammed fuse reads the same) at 1,508 MHz under the driver's envelope.
  The board's fuse word has not been read.
- **A granted raise.** The CPU-B rate that boot0 leaves is not recorded.
- **One lifetime for both.** The GPU linear build requires the resident
  lifetime. A grant needs the supervised lifetime: every change needs the
  user's approval before each run, and the stage-2 loader stamps a fresh epoch
  on every boot, so a prepared grant cannot name a resident boot.

The table below models the A76 pair with the GPU placement table. The linear
stage comes from `placement.py --summary` of the GPU package at `ad0420c`.
The other stages scale 4.582 s on one A55 at 494 MHz, assuming perfect
scaling and twice an A55's work per clock on an A76.

| CPU-B point | Linear stage | Other stages | Recognition | Times audio |
| --- | --- | --- | --- | --- |
| 2,002 MHz | 0.535 s | 0.283 s | 0.818 s | 0.092 |
| 1,976 MHz | 0.536 s | 0.286 s | 0.823 s | 0.093 |
| 1,794 MHz | 0.578 s | 0.315 s | 0.893 s | 0.101 |
| 1,612 MHz | 0.627 s | 0.351 s | 0.978 s | 0.110 |
| 1,508 MHz | 0.637 s | 0.375 s | 1.012 s | 0.114 |
| 1,014 MHz | 0.755 s | 0.558 s | 1.313 s | 0.148 |

The A55 pair at 494 MHz models at 3.438 s. This package does not reach the
goal; it supplies the core placement and the clock that the other levers
assume. The model ignores the DSU (L3) clock, which the driver never changes.

## Data flow and ownership

- **Placement.** `include/a733_smp_placement.h` (new) holds preprocessor
  constants only: `A733_SMP_A55_MASK` 0x3F, the service core 0, and the
  recognition core 6 with admission or 2 without. `A733_SMP_A76_MASK` (0xC0)
  exists only under `A733_A76_ADMITTED`, so a use without admission fails to
  compile. A `_Static_assert` keeps each mask inside the scheduled cores.
- **Default affinity.** Every OS build passes
  `configTASK_DEFAULT_CORE_AFFINITY=0x3F`, which the pinned `FreeRTOS.h` reads
  before its `tskNO_AFFINITY` default. Today every board and OS task created
  without affinity, the recognizer included, may run on cores 6 and 7; the PMIC
  owner alone is pinned to core 0. With the constant, only tasks created with
  an explicit A76 mask reach cores 6 and 7. `apps/os/start.c` asserts that the
  constant equals `A733_SMP_A55_MASK` and, in the GPU build, that
  `OS_RECOGNITION_CORE` equals `A733_SMP_RECOGNITION_CORE`. These assertions
  are the header's first consumer.
- **Recognizer.** In builds without the GPU option, the new
  `apps/os/recognition-placement.patch` creates the recognizer with
  `1 << A733_SMP_RECOGNITION_CORE`. The GPU build's own recognizer patch
  already does this. Core 7 runs only its idle task until the engine's second
  inference worker lands with the GPU engine hook.
- **Admission record.** Before the scheduler, `os_thermal_start` calls
  `os_cpu_admit` (new `apps/os/cpu.c`). It checks the Device mapping of the
  clock words once and makes 14 reads: the oscillator, the fuse word, the PLL
  and mux words of CPU-L, CPU-B and the DSU (`a733_cpu_clock_report`), and the
  pattern and LFM words of CPU-L and CPU-B. When CPU-B is not on its locked
  PLL with a decodable rate, it refuses the start, and the board stops with
  panic 11 before any task runs. In the CPU clock build the thermal task also
  takes one PMIC snapshot each of DCDC3 and DCDC5, under the exclusive rail
  lease of the grant's owner that the driver uses, and records each rail's
  boot floor with `a733_rail_floor`.
- **Grant.** With `SUNSTONE_A733_OS_CPU_CLOCK`, the session gains a 128-byte
  extension after the thermal facts (and after the microphone facts when
  present): magic, version and size, the driver's 96-byte grant record and
  8 zero bytes. The session is then 936 or 1,120 bytes. `apps/os/admission.c`
  calls `a733_cpu_clock_grant_load` with the board session's epoch and
  generation, and refuses an owner equal to any other session owner.
  `apps/os/prepare.py` writes the record only from a reviewed approval record,
  whose SHA-256 it stores. A record that grants nothing is valid, and the
  driver then refuses every request.
- **Owner.** The thermal task, on an A55 core, is the driver's only caller, so
  no task or supervised service is added. After the policy's first reading it
  calls `a733_cpu_clock_init` with the policy that it already updates, the
  board's clock registry, rail ledger and PMIC provider, and the native
  adapter `drivers/clock/a733_cpu_clock_native.c` (new). Once the cluster's
  own sensor has a reading, it requests each granted cluster's maximum once.
  It then calls `a733_cpu_clock_poll` after every thermal step. The driver
  lowers a raised cluster to 1,014 MHz at THROTTLE and restores it at NORMAL
  while changes remain.
- **Record.** After each completed transition the owner publishes the applied
  point and voltage of each cluster with a completed-transition count. A
  `CLOCK` line in the recognizer's transcript reports them, from a weak hook
  that `apps/mimus/recorded.cc` calls (see "Changes outside this package").

## Interrupts and polling

No interrupt is added or rerouted. The tick and SGI0 stay as they are,
INTID 71 stays on core 0, and only the GPU build routes INTID 95 to core 6.
While no transition is active, the thermal task keeps its 25 ms period and
the clock work adds no wake-up. While a transition is active it sleeps one
tick between driver steps. Section 1.3 permits a tick wait for PLL lock and
rail ramps. A ramp needs `|ΔV| ÷ 250 µV/µs`, at most 1 ms for the 250 mV from
800 to 1,050 mV. A longer wait only delays the frequency change, so the
settle wait sleeps whole ticks and never spins. PMIC results come through the
power service, which submitters poll once per tick.

## Memory, cache and barriers

Driver state and the record are static in `apps/os/cpu.c`; only the thermal
task writes them. No DMA buffer, device memory or cache maintenance is added.
The adapter uses plain 32-bit accessors on the Device-nGnRnE window, which
the peripheral observes in program order, and one `DSB SY` after each step
that writes a clock register, before the step's update or lock timer starts
(section 1.2). That is two per frequency change.

## Synchronization across cores

- One task uses the driver, the policy level and the clock leases. The
  clock registry, rail ledger and PMIC provider keep their manager locks for
  these state changes (section 1.7, heavier form).
- The record is a 64-byte line with a sequence count. The writer makes the
  count odd, writes the fields and stores an even count with release. The
  recognizer reads the count with acquire before and after the fields and
  marks the line `changed` when they differ.
- The owner runs on CPU-L. Changing CPU-B parks cores 6 and 7 on the 26 MHz
  oscillator for at most 80 ms while their PLL relocks; changing CPU-L also
  parks the owner. A transition during recognition therefore shows in the
  record's count.
- In-line SGI: `vInterruptCore` reads `TPIDRRO_EL0` once. When the TCB field
  is nonzero and either bit 56 is set (an EL1 task) or the
  `ullPortInterruptNestings` entry of the core in bits [55:48] is nonzero (an
  interrupt handler), it
  issues `DSB ISHST`, `MSR ICC_SGI1R_EL1` and `ISB`. Otherwise, for an EL0
  task or before the core has resumed a task, it keeps `SVC 117`. An EL0 task
  reads a nesting count of zero, because no handler is active on a core that
  runs a task. The path lives in `patches/freertos-smp-c.patch`, which the
  QEMU SMP images also apply. The target value comes from a port macro,
  `portSGI1R_VALUE`, whose default is the upstream formula and which
  `platform/a733-smp-cached/FreeRTOSConfig.h` maps to `a733_smp_sgi_value`.

## Reused and removed

Reused: the CPU clock driver and grant loader, the thermal policy and task,
the rail ledger floor, the PMIC write path, session admission and
preparation, and the existing clock, SMP and OS-image models. Removed:
`SVC 117` for EL1 callers, the `vInterruptCore` hunk of
`patches/freertos-a733-smp.patch`, and `A733_A76_ADMITTED` as a user-set
CMake variable; the option sets it.

## Budgets and measurement

The three changes land as separate commits so each comparison isolates one.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Default OS images, first commit | — | Loadable bytes differ only in the default-affinity constant in `tasks.c` and the 32-byte build identity | `llvm-objcopy -O binary` and disassembly at both commits |
| Tasks that may run on cores 6 or 7, apart from their idle tasks, the recognizer and the GPU worker (A76 build) | Every task created without affinity (counted) | 0 | `checks/os-image-cpu.py` (new): the compiled default affinity and every explicit mask in the image's sources |
| A76 admission | No path | 14 reads and no writes before the scheduler, once per boot; in the CPU clock build, 2 PMIC snapshots afterwards | `cpu-clock-model.c` with the OS owner |
| CPU-B raise with a voltage write | No path | 5 clock register writes, 2 PMIC requests, 2 adapter `DSB SY`; at most 16 owner steps when the PLL locks within a tick | Same |
| Owner wake-ups added while no transition is active | — | 0 | Same |
| CPU-B on the oscillator per transition | No path | At most 80 ms (update and lock bounds); 20 µs modeled lock | Model; board |
| Transitions per boot | — | At most the grant's change count, 1 to 64 | Model |
| Tick on core 0, nothing woken, EL0 tasks: `SVC`, `DSB SY`, instructions | 7, 32, 1,667 | 0, at most 32, fewer than 1,667 | `checks/a733-smp-cached.py --count` |
| `vInterruptCore` from a handler or EL1 task: `SVC` | 1 | 0 | Same, with `vInterruptCore` counted |
| QEMU port operations | Table in [smp.md](../smp.md#measured-cost) | None higher; cross-core round trip at most 12,191 instructions | `checks/smp-port-cost.py` |
| Recognition, bench recording | 0.818 s modeled at 2,002 MHz | The table above at the recorded bin's highest point | Model; board |

Gates: `tools/check-smp.py` cases `kernel`, `context` and `soak8` (ten trials)
on `cortex-a55` and `cortex-a76`, `checks/cpu-clock-check.py`, the
`checks/os-image-*` checks, and the link check of every OS build. The CPU
clock build links in the supervised lifetime only.

## Failure classes

| Failure | Class (section 1.8) | Result | Retained |
| --- | --- | --- | --- |
| Admission record refused | 1 | Start refused, panic 11 | Nothing |
| Grant record refused | 1 | Session refused at admission | Nothing |
| Transition REFUSED, or EXPIRED before the PLL is touched | 1 | Cluster stays at its point, which its voltage covers; recorded | Nothing |
| Transition RETAINED: lock or update timeout, readback mismatch, rejected PMIC write | 3 | `board_fail`; the supervised session halts | CPU clock and rail leases, the PMIC request |
| THROTTLE lowering refused | 1 | Not retried in that episode; CRITICAL still ends the session | Nothing |

## Left out

- **A load-based governor** (lower when idle, per-utterance changes): no
  consumer, and the power cost is unmeasured.
- **DSU clock changes**: the driver never writes the DSU. They return if
  board timing shows the A76 pair limited by the L3.
- **Operating-point changes in the resident lifetime, and so in the GPU
  build**: they need a standing user approval of a qualified point and a
  grant that names the slot generation in place of the epoch.
- **A measured clock on the recognition core**: the recognizer runs at EL0
  without PMU access; the record gives configured points.
- **Changing N in place with LFM**, as the vendor driver does: the driver
  refuses LFM.
- **Cluster migration and A76 power-down when idle**: no consumer; idle
  cores wait in `WFI`.

## Hardware sequences and their sources

This package adds one register sequence, the SGI send. It follows
`gic_ipi_send_mask` in the pinned Linux `irq-gic-v3.c` (`e5f0a698`,
`cpu-audit-drivers-irqchip-irq-gic-v3-c`): `dsb(ishst)`, the register write,
then `isb()`. IHI 0069G §12.2.21 (pinned) makes the register UNDEFINED at
EL0, which is why EL0 tasks keep the supervisor call. The clock and voltage
sequences are the driver's, with the sources that
[cpu-clock.md](../cpu-clock.md) cites (linux-orangepi `2ac08e8c`
`ccu-sun60iw2-cpupll.c`, `ccu_nkmp.c`, `sun60iw2p1-cpu-vf.dtsi` V0.9, the SID
node, the board device tree; u-boot `b791be84` `pmu_axp8191.c`). No source
proves:

- parking a cluster's mux on the oscillator while that cluster executes. The
  pinned driver changes N in place with LFM and never reparents at run time;
- that lock reports the final rate with the pattern and LFM enables clear;
- DCDC3 and DCDC5 settling at the device tree's 250 µV/µs;
- the CPU-B rate, the pattern and LFM words, and the fuse code that boot0
  leaves;
- application SGI delivery on this board. The supervisor-call path writes
  the same register at the same exception level and is also unproven.

## Board steps

Each step schedules sustained work on cores 6 and 7, which no physical run
has shown. Every step therefore requires, on the same board, that rank 7 (the
all-core first-task diagnostic) has returned and that rank 8 (task bodies,
floating point, SGIs and the timer on all cores) has then passed. Rank 7 did
not return on 2026-10-23 or 2026-10-24, so every step below waits for an
offline diagnosis, a returning rank 7 and a passing rank 8. The OS image is a
Terminal test, so each step also needs ranks 1 to 5 and rank 9 earlier in the
same power cycle ([board test plan](../audit/board-test-plan.md)).

Without a clock or voltage change:

- **B1.** The supervised A76 build with the CPU clock option and a record
  that grants nothing. It shows the recognizer on core 6 with text and CRCs
  equal to the host reference, and every service reporting while the tick
  and SGIs use the in-line send. It records the `CLOCK` line, the fuse code,
  the CPU-B PLL, mux, pattern and LFM words, and the DCDC3 and DCDC5
  voltages.

Each of the following needs the user's explicit approval before the run. Each
runs last in a connection window after B1 has passed on that board, and its
grant names that approval record:

- **A1.** CPU-B to 1,196 MHz, which needs 800 mV in every bin, so B1's DCDC3
  reading decides whether any voltage write occurs. This is the first park
  and relock of a running cluster.
- **A2.** CPU-B to the highest point of B1's bin, with the DCDC3 write, its
  ramp, and recognition timed at that point.
- **A3**, optional. CPU-L to 1,196 MHz, the first trial that
  [cpu-clock.md](../cpu-clock.md) names, for the service cores.

## Files each package changes

| File | This package | GPU recognition package |
| --- | --- | --- |
| `cmake/a733-os.cmake` | Option block after the mSBC check; definitions after the headset definitions; one identity append after the input-hash loop | Its option checks, sources, definitions and identity string |
| `docs/os-image.md` | A new section before "Build and checks" | Its existing sections |
| `apps/os/start.c`, `thermal.c`, `admission.c`, `os.h`, `prepare.py`; new `cpu.c`, `recognition-placement.patch` | Yes | No |
| `apps/os/dispatch.c`, `dispatch.h`, `identity.c`, `services.c`, `placement.py`, `application.patch`, `recognition-dispatch.patch`; new `gpu.c`, `gpu_hook.cc` | No | Yes |
| `include/a733_smp_placement.h` (new) | Yes | No; `start.c` checks the GPU build's `OS_RECOGNITION_CORE` against it |

This package also owns `drivers/clock/a733_cpu_clock_native.c` (new),
`platform/a733-smp-cached/FreeRTOSConfig.h`, `patches/freertos-smp-c.patch`,
`checks/cpu-clock-model.c`, `checks/a733-smp-cached.py`,
`checks/os-image-cpu.py` (new), `docs/cpu-clock.md` and `docs/smp.md`.

## Changes outside this package

These land through `primary.patch`:

- `patches/freertos-a733-smp.patch`: delete the `vInterruptCore` hunk.
- `apps/mimus/recorded.cc`: in native builds, `report_clock` prints the line
  from a weak `os_cpu_clock_line` hook in place of nothing.
- `checks/index.json`: regenerate it for the new check.
- `tools/check-a733-os-supervised.py`, only if it checks the session size;
  the implementation confirms whether it does.

Other owners: the GPU package regenerates its table with `--cpu-mhz` for the
point that A2 qualifies.
