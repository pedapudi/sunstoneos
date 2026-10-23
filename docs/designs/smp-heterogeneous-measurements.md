# SMP port and cached base: measurements against the design budgets

Status: **emulated**. Every figure below comes from QEMU instruction counts,
the Unicorn model of the cached eight-core image, or the built ELF files. No
figure comes from the board. The budgets are in
[smp-heterogeneous.md](smp-heterogeneous.md).

## Conditions

- Base tree: the merge base with the integration branch,
  `f4a272f6cfaa15597f2987f807a6efc1da6fdb32`. Measured tree: commit
  `df27d08`. Both were extracted with `git archive` into separate directories
  and built in separate directories with clang 21 and the project toolchain
  file. The measured tree needs the storage-maintenance hash update that the
  package delivers as `primary.patch`; it was applied to the extracted tree
  before configuring.
- QEMU figures: `checks/smp-port-cost.py` from the measured tree, run once
  with `--root` set to the base tree and once on the measured tree with
  `--baseline`. QEMU 10.2.1, two cores, `-icount shift=0`, where
  `INST_RETIRED` is exact. Each operation is the least of 16 samples minus an
  empty region; the round trip is the mean of 256. Cortex-A55 and Cortex-A76
  models gave identical counts in both trees.
- Unicorn figures: `checks/a733-smp-cached.py --count` from the measured tree,
  run on the cached diagnostic ELF of each tree. Each figure is the median per
  call over the scheduler run. Every task in that image runs at EL0, so the
  critical sections take the supervisor-call path for the mask.
- Consolidation comparison: the cached diagnostic, observe and supervised
  images were built from the commit before the consolidation (`137a827`) and
  from the consolidation commit (`b55159e`), stripped and compared byte by
  byte and section by section.
- Validation on the measured tree: three trials of `tools/check-smp.py --case
  soak8` passed with `--cpu cortex-a55` and three with `--cpu cortex-a76`. The
  cached model passed its eight-core scheduler run and its missing-secondary
  refusal on both trees.

## Results

| Metric | Base | Measured | Budget | Within budget |
| --- | --- | --- | --- | --- |
| Critical-section pair, EL1 task: instructions (QEMU) | 487 | 296 | ≤ 100 | No |
| Critical-section pair, EL1 task: `SVC` (disassembly of the EL1 path) | 4 | 0 | 0 | Yes |
| Critical-section pair, EL0 task: instructions (QEMU) | 487 | 368 | ≤ 180 | No |
| `vTaskExitCritical`, EL0 task: `DSB` / `DMB` / `ISB` (Unicorn) | 19 / 4 / 5 | 18 / 4 / 4 | 0 / 0 / 1 after the second change | Second change not made |
| Current-task read: instructions (QEMU); `SVC`, `DSB`, `ISB` once the task field is set | 170; 3, 2–4, 2–4 | 11; 0, 0, 0 | 0, 0, 0 | Yes |
| `ucPortGetCoreID`: instructions, `SVC`, `DSB`, `ISB` (Unicorn) | 121, 1, 1, 1 | 5, 0, 0, 0 | — | — |
| Notification given to the caller and taken, EL1 task: instructions (QEMU) | 1,644 | 656 | `SVC` 0 | Yes |
| Same-core switch after a notification, FPU state included: instructions (QEMU) | 2,779 | 1,320 | ≤ 800 | No |
| `vTaskSwitchContext`: instructions, `SVC`, `DSB`, `ISB`, lock calls (Unicorn) | 1,747, 10, 47, 24, 4 | 305, 0, 9, 0, 2 | 0 `DSB` after the second change | Second change not made |
| SGI0 handler: instructions, `SVC`, `DSB`, `ISB`, lock calls (Unicorn) | 479, 2, 24, 12, 2 | 15, 0, 0, 0, 0 | `SVC`, `DSB`, `ISB`, locks all 0 | Yes |
| Tick on core 0: instructions, `SVC`, `DSB`, `ISB` (Unicorn) | 2,071, 12, 36, 23 | 1,667, 7, 32, 15 | `SVC` 0, `DSB` 18 | No |
| `vTaskEnterCriticalFromISR`: instructions, `DSB`, `ISB` (Unicorn) | 227, 6, 1 | 89, 7, 3 | — | — |
| `vTaskExitCriticalFromISR`: instructions, `DSB`, `ISB` (Unicorn) | 208, 12, 5 | 90, 11, 4 | — | — |
| Cross-core notification round trip, instructions on both cores (QEMU) | 29,236 | 12,191 (−58%) | ≥ 30% below base | Yes |
| Port cache lines written by several cores (ELF symbols) | FPU flags, ownership bits, lock words and recursion counts packed in 152 bytes | `xPortCoreStates` 8 × 64 bytes and `xPortLocks` 2 × 64 bytes, both 64-byte aligned; nesting and yield arrays unchanged | One line per lock and per core | Yes |
| Patched kernel copies for the cached, observe and supervised images | 3 | 1 | 1 | Yes |
| Lines in the three CMake files | 325 | 200 | ≤ 200 | Yes |
| Stripped ELF bytes changed by the consolidation, outside `a733_smp_build_id` | — | cached 0; observe 433; supervised 1,018 | 0 | No; see below |
| Port and kernel function bytes in the cached diagnostic | 34,164 | 34,636 (+472) | — | — |
| `.text` / `.bss` of each of the three images | — | +512 / +496 bytes | — | — |
| `DSB`, `DMB`, `ISB` and `SVC` instructions in the cached diagnostic | 217 | 216 | — | — |

## Findings

- **Budgets not met with a reason in the design.** The design attributes the
  296-instruction EL1 pair and the 1,320-instruction switch to the four
  `vPortRecursiveLock` calls, which keep their assertions and barriers until
  the second change.
- **Budgets not met without a reason in the design.** The EL0 pair retires
  368 instructions against a target of 180. The tick keeps 7 `SVC` and 32
  `DSB` against targets of 0 and 18. [smp.md](../smp.md) explains both: the
  EL0 pair keeps two supervisor calls and the four lock calls, and the seven
  tick calls are `SVC 117`, which `vInterruptCore` takes because the tick
  interrupts EL0 tasks whose bit 56 is clear. The design document does not
  carry either explanation. The design's tick baseline of 2 `SVC` and 21 `DSB`
  was counted by reading the source; the measured base is 12 and 36, and the
  measured tree is below the measured base on both.
- **Barriers move from the tick into the ISR critical section.** Removing the
  tick handler's outer mask (design item 4) means the inner
  `uxPortSetInterruptMaskFromISR` no longer finds the mask already set, so it
  runs its `DAIF` bracket and PMR write. `vTaskEnterCriticalFromISR` rises
  from 6 to 7 `DSB` and from 1 to 3 `ISB` per call, while the tick as a whole
  falls by 4 `DSB` and 8 `ISB`.
- **Consolidation bytes.** In the observe and supervised images, every
  differing byte outside the build identity lies in the `configASSERT` file
  names of the kernel copy (`build/freertos-a733-smp-observe/port.c` becomes
  `build/freertos-a733-smp-cached/port.c`) and in the address immediates that
  point at those strings. After masking immediates, the disassembly of both
  images is identical to the previous build. Image sizes are unchanged, and
  the supervised `.rodata` is 8 bytes smaller. The commit message and
  [smp.md](../smp.md) state that only the 32 build-identity bytes differ; that
  holds for the cached diagnostic only.
- **Code and data size.** Port and kernel code grows by 472 bytes, from the
  inline EL1 mask sequences and the `TPIDRRO_EL0` fast paths that design items
  1 and 2 add; `prvAddCurrentTaskToDelayedList` accounts for 204 bytes.
  `.bss` grows by 496 bytes because each core's FPU flag and ownership bits,
  and each lock, get their own 64-byte line (design item 5).

## Commands

```sh
m="$HOME/.cache/sunstoneos/build/smp-heterogeneous-measure"
python checks/smp-port-cost.py --output "$m/cost-base" --root "$m/base-src"
python checks/smp-port-cost.py --output "$m/cost-head" --root "$m/head-src" \
  --baseline "$m/cost-base/result.json"
python checks/a733-smp-cached.py --evidence "$m/count-base" \
  --image "$m/build-base/a733-smp-cached-diagnostic.elf" --count
python checks/a733-smp-cached.py --evidence "$m/count-head" \
  --image "$m/build-head/a733-smp-cached-diagnostic.elf" --count
python tools/check-smp.py "$m/build-head/smp8-soak-model.elf" --case soak8 \
  --cpu cortex-a76 --trials 3 --evidence "$m/soak8-head-cortex-a76.json"
```
