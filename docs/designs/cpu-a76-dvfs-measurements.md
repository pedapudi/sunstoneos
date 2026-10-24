# A76 recognition cores, CPU operating points and in-line SGIs: measurements against the design budgets

Status: **host-modeled and emulated**. Every figure comes from the built ELF
files, the CPU clock transition model, the OS image host models, the cached
eight-core Unicorn model or QEMU. No figure comes from the board, and no
clock or voltage changed on any board. The budgets are in
[cpu-a76-dvfs.md](cpu-a76-dvfs.md).

## Conditions

- Base tree: `15600aea`, the integration branch merged into the package
  branch. Measured commits: `a0780792` (placement and admission),
  `d3a44665` (clock option) and `4f5acef6` (in-line SGI and time slice),
  each with the package's `primary.patch` applied for the files outside the
  package (`apps/mimus/parallel_runtime.*`, `apps/mimus/recorded.cc` and the
  `vInterruptCore` hunk of `patches/freertos-a733-smp.patch`).
- Image comparisons: each tree was extracted with `git archive`, copied over
  one shared source path and built in one shared build directory with clang
  21 and the project toolchain file, so file-name strings and their addresses
  match. A build from the worktree path gave the same ELF hash. Functions are
  compared by symbol size (`nm -S`).
- Cached instruction model: `checks/a733-smp-cached.py --count` from the
  measured tree, which now counts `vInterruptCore`, `xTaskIncrementTick` and
  the SGI sends inside each counted call, run on the cached diagnostic of the
  base tree and of `4f5acef6`. The base diagnostic was built at `f29c157c`,
  whose kernel, port and diagnostic sources equal `15600aea`'s. Figures are
  medians per call over the scheduler run; every task in that image runs at
  EL0.
- QEMU: `checks/smp-port-cost.py` from the measured tree, once with `--root`
  set to the base tree and once with `--baseline` and
  `--round-trip-reduction 0`. QEMU 10.2.1, `-icount shift=0`. Cortex-A55 and
  Cortex-A76 models gave identical counts.
- The host was shared with other work at a load average of 20 to 33 on 24
  hardware threads during these runs.

## Budgets

| Metric | Base | Target | Measured | Result |
| --- | --- | --- | --- | --- |
| Default OS images, first commit | — | Change only in the default-affinity constant and the build identity | Supervised, resident, resident with microphone, resident with headset: `vTaskStartScheduler` +4 bytes and the affinity immediate (`#-1` to `#0x3f`) in task creation; `mimus_transcribe_recorded` +64 bytes for the `CLOCK` hook, which prints nothing without the OS record; identity | Met, with the hook named |
| Call sites that place work on cores 6 or 7 (A76 build) | Every task created without affinity | Three | Recognizer (`1 << 6`), inference worker (`1 << 7`), GPU worker in the GPU build; every other task takes 0x3F | Met |
| A76 admission | No path | 8 reads, no writes, once per boot; clock build adds 22 inspection reads and 2 PMIC snapshots | As targeted, counted from the code; the model's inspection case writes no register and no PMIC byte | Met |
| CPU-B raise with a voltage write (A2 on board `3-3`) | No path | 5 clock writes, 2 PMIC requests, 3 adapter `DSB SY` | 5 clock writes, 1 snapshot and 1 PMIC write, 3 `DSB SY` (code) | Met |
| Owner ticks while parked | No path | 0 with a lock of about 20 µs | 0; parked 25 µs; 15 owner ticks for the whole transition | Met |
| Same, lock of 300 µs | No path | Recorded | 1 parked tick; parked 1,206 µs | Recorded |
| Longest park | No path | About 90 ms; RETAINED stays parked | Bounded by the 10 ms update, 70 ms lock and 10 ms margin limits | By construction |
| Owner wake-ups added | — | 0 | 0: the thermal task's per-tick loop is unchanged | Met |
| Tick on core 0: instructions, `SVC`, `DSB`, SGI sends (median) | 1,667, 7, 32, 7 | At most 700, 0, at most 18, 0 | 1,237, 0, 20, 2 | `SVC` met; the rest not met |
| SGI sends from ticks over the run | 314 in 67 ticks | — | 181 in 64 ticks | Recorded |
| `vInterruptCore` from a handler: instructions, `SVC`, `DSB`, `ISB` | 161, 1, 2, 1 | At most 80, 0, 1, 0 | 39, 0, 1, 0 | Met |
| SGI handler calls; `vTaskSwitchContext` calls over the run | 407; 708 | — | 189; 484 | Recorded |
| EL1 cross-core round trip (QEMU) | 12,191 | At most 12,191 | 11,994 | Met |
| EL0 cross-core round trip (QEMU, new) | 14,127 | At most 24 more per send | 14,169 (+21 per send) | Met at the revised bound |
| Other QEMU port operations | `smp.md` table | None higher | Unchanged | Met |
| Recognition, bench recording | Not measured | Model at the board's highest point | Not measured; B1 and A2 measure it | Board |

The tick misses its instruction, barrier and send targets for two reasons.
The ready-list scan costs about 345 instructions: ticks without a send take
574 instructions at the median against 229 in the base. Ticks that wake
tasks still send SGIs; the remaining sends are 20 of 64 ticks with seven
sends and 9 with two. The targets assumed neither. The barrier count, 20,
is the base tick's 18 non-send barriers plus the `DSB ISHST` of each of the
two median sends.

The EL0 send path is 5 instructions longer (disassembly). QEMU's lock-wait
interleaving amplifies a longer send: a control build with five no-ops in the
base send path gave an EL0 round trip of 14,147 and an EL1 round trip of
12,240, that is 10 and 24 instructions per send. The design's bound of 16 per
send became 24, the larger control figure.

## Recognition model

From `placement.py --summary` of the GPU package at `ad0420c` for the linear
stage with the GPU table, plus 4.582 s on one A55 at 494 MHz scaled to the
A76 pair; perfect scaling assumed. A2 runs the supervised build without the
GPU, so its target is CPU-only recognition on the pair: 14.894 s × 494 ÷
(2 × 2 × 2,002) + 0.283 s, about 1.20 s at 2,002 MHz.

| CPU-B point | With the GPU table | CPU-only on the pair |
| --- | --- | --- |
| 2,002 MHz (board `3-3`, vf0400) | 0.818 s | 1.20 s |
| 1,794 MHz | 0.893 s | 1.34 s |
| 1,612 MHz | 0.978 s | 1.49 s |
| 1,508 MHz | 1.012 s | 1.60 s |
| 1,014 MHz | 1.313 s | 2.37 s |

## Checks

All passed unless stated otherwise.

- `checks/cpu-clock-check.py` with the extended model: settle, inspection,
  burst and A2 cases. Altered drivers failed the model: no settle after the
  park, a settle of 0 µs, a settle of 1 µs, and a burst that polls once.
- `checks/os-image-model.c` for the clock build: 36 refusals supervised and
  57 with the microphone, including each grant-record rule; a 2,002 MHz
  CPU-B grant for the session is admitted.
- `checks/os-image-launch.py`: default supervised (21 cases), supervised
  with microphone (27), clock build (31) and clock build with microphone
  (37). The clock builds add the A2 preparation and nine refusals.
- `checks/os-image-audio.py` on the supervised microphone build and the
  clock build with microphone; `checks/os-image-headset.py` on the resident
  A76 headset build; `checks/reset-latency.py`.
- Builds and their link checks: the four default OS images, the A76 build
  supervised, resident and resident with headset, and the clock build
  supervised and with microphone. CMake refuses the clock option with the
  resident lifetime and without `A733_A76_ADMITTED`.
- `checks/smp-sgi-send.py` on the measured cached diagnostic: four cases. On
  the base diagnostic it fails at the handler case, which takes `SVC 117`.
- `checks/a733-smp-cached.py --count`: the eight-core scheduler run and the
  missing-secondary refusal pass on both diagnostics.
- `tools/check-smp.py`: `kernel` and `context`, ten trials each on
  `cortex-a55` and `cortex-a76`.
- `soak8`: under the host load above, the real-time QEMU run failed in its
  first trial at the soak's one-second migration acknowledgement for both
  trees. Twenty alternating single trials passed 10 times for the base and
  11 times for the measured tree, so the failures are not attributable to
  the change. Under `-icount`, which ties virtual time to instructions and
  makes each run deterministic, the measured soak passed with shifts 0 to 4
  on both CPU models, ten distinct schedules. With shifts 5 to 7 each
  instruction takes 32 to 128 ns of virtual time, and both trees fail the
  same one-second wait; the base passes at shifts 0 and 4 and fails at 5
  and 7.
- `tools/check-mimus-smp.py` on `smp-mimus-model`: features, log
  probabilities, frame count, words, punctuation, capitals and formatted
  text match the host reference, with the worker's cores at their defaults.

Evidence: `~/.cache/sunstoneos/build/cpu-a76-dvfs/evidence/` and the build
directories beside it.
