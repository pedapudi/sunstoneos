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
| Owner ticks while parked | No path | 0 with a lock of about 20 µs | 0; parked 23 µs of model time; 15 owner ticks for the whole transition | Met |
| Same, lock of 300 µs | No path | Recorded | 1 parked tick; parked 1,203 µs of model time | Recorded |
| Longest park | No path | About 90 ms; RETAINED stays parked | Bounded by the 10 ms update, 70 ms lock and 10 ms margin limits | By construction |
| Owner wake-ups added | — | 0 | 0: the thermal task's per-tick loop is unchanged | Met |
| Tick on core 0 that wakes no task, scheduler running: instructions (median, largest), `SVC`, `DSB`, SGI sends | 1,345, 1,667, 5, 28, 5 (16 ticks) | At most 700, 0, at most 18, 0 | 554, 590, 0, 18, 0 (9 ticks) | Met |
| Ticks that wake tasks: ticks, SGI sends over the run (required preemptions) | 36, 246 | Recorded | 47, 177 | Recorded |
| Ticks taken while the scheduler is suspended: ticks, median instructions | 15, 229 | Recorded | 8, 231 | Recorded |
| `vInterruptCore` from a handler: instructions, `SVC`, `DSB`, `ISB` | 161, 1, 2, 1 | At most 80, 0, 1, 0 | 18, 0, 1, 0 | Met |
| SGI handler calls; `vTaskSwitchContext` calls over the run | 407; 708 | — | 185; 480 | Recorded |
| EL1 cross-core round trip (QEMU) | 12,191 | At most 12,191 | 11,994 | Met |
| EL0 send path (QEMU port) | Upstream `vInterruptCore` | At most 5 instructions more per EL0 send | 5 more (disassembly) | Met |
| EL0 cross-core round trip (QEMU, new) | 14,127 | Recorded; regression guard 24 per send | 14,169 (+21 per send) | Recorded |
| Other QEMU port operations | `smp.md` table | None higher | Unchanged | Met |
| Recognition, bench recording | Not measured | Model at the board's highest point | Not measured; B1 and A2 measure it | Board |

The tick rows come from `checks/a733-smp-cached.py --count`, whose
`tick_by_wakes` result sorts each tick by its `prvYieldForTask` entries and
by whether the scheduler was suspended. The budget covers ticks that wake no
task; in the base those ticks still sent five time-slice SGIs to cores whose
same-priority peers are pinned elsewhere. An earlier version of this record
compared the median of all ticks, which mixes pended and waking ticks, and
reported the budget as missed. Skipping the ready-list scan when a list is no
longer than the cores running its priority was measured and left out: the
zero-wake median rose to 618 instructions in that run.

The EL0 send path of the QEMU port is 5 instructions longer than upstream's
(disassembly). QEMU's lock-wait interleaving amplifies a longer send: a
control build with five no-ops in the base send path gave an EL0 round trip
of 14,147 and an EL1 round trip of 12,240, that is 10 and 24 instructions per
send. The check keeps 24 per send as a regression guard on the round trip.

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

- `checks/cpu-clock-check.py` with the extended model, whose clock runs in
  nanoseconds and returns whole microseconds: settle, inspection, burst, A2
  and stopped-time-source cases. Altered drivers failed the model: no settle
  after the park, a settle that trusts its first quantized reading, a burst
  that polls once, and a burst without a poll bound, which never returns.
- `checks/os-image-model.c` for the clock build: 37 refusals supervised and
  58 with the microphone, including each grant-record rule and a grant of
  both clusters; a 2,002 MHz CPU-B grant for the session is admitted.
- `checks/os-image-launch.py`: default supervised (22 cases) and clock build
  (34). The clock build adds the A2 preparation and eleven refusals,
  including a B1 record that did not pass or carried a grant, and every
  build refuses a link check whose `cpu_clock` record differs.
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
- `soak8` deviates from its gate, ten passing real-time trials per CPU
  model: under the host load above, the real-time QEMU run failed in its
  first trial at the soak's one-second migration acknowledgement for both
  trees. Twenty alternating single trials passed 10 times for the base and
  11 times for the measured tree, so the failures are not attributable to
  the change. Under `-icount`, which ties virtual time to instructions and
  makes each run deterministic, the measured soak passed with shifts 0 to 4
  on both CPU models, ten distinct schedules, before and after the review
  changes. These runs and the base comparison stand in for the real-time
  gate until a run on a lightly loaded host. With shifts 5 to 7 each
  instruction takes 32 to 128 ns of virtual time, and both trees fail the
  same one-second wait; the base passes at shifts 0 and 4 and fails at 5
  and 7.
- `tools/check-mimus-smp.py` on `smp-mimus-model`: features, log
  probabilities, frame count, words, punctuation, capitals and formatted
  text match the host reference, with the worker's cores at their defaults.

## After the GPU recognition package merged

The integration branch at `db7b77ce` adds the GPU recognition package. On
the merged tree, with the A76 worker calls added to the GPU recognizer patch
and `primary.patch` applied, the four default OS images were compared with
the same images built from `db7b77ce` at one shared path. The same four
functions differ, and only they: `mimus_transcribe_recorded` (1,868 to 1,932
bytes, the `CLOCK` hook), `vTaskStartScheduler` (564 to 568, the
default-affinity constant), `vInterruptCore` (28 to 100, the in-line send)
and `xTaskIncrementTick` (1,060 to 856, the time-slice rule). The default,
A76 (supervised and resident), clock and GPU-linear builds, the last with
and without `A733_A76_ADMITTED`, link and pass their link checks; with
admission the GPU interrupt routes to core 6 and the image links the
inference worker. `checks/os-image-dispatch.py --configure-from` on the GPU
build, `checks/os-image-launch.py` on the default (21 cases) and clock (31)
builds, `checks/smp-sgi-send.py` on the rebuilt cached diagnostic and
`checks/check-index.py --verify` pass.

## After the code review

The review changes alter the default images once more: the A733 SGI value
is computed in line, so `vInterruptCore` is 120 bytes and the out-of-line
`a733_smp_sgi_value` and `a733_cpu_sgi1r` (104 bytes) leave the images. The
four default images, compared with `db7b77ce` at one shared path, now differ
in `mimus_transcribe_recorded` (+64 bytes), `vTaskStartScheduler` (+4),
`vInterruptCore` (28 to 120), `xTaskIncrementTick` (1,060 to 856) and those
two removed functions. The default, A76, clock and GPU-linear builds, with
and without admission, link and pass their link checks;
`checks/os-image-dispatch.py --configure-from`, `checks/smp-sgi-send.py`,
`checks/smp-port-cost.py`, `tools/check-smp.py` `kernel` and `context` (ten
trials on each CPU model), the soak under `-icount` and
`tools/check-mimus-smp.py` pass again.

Evidence: `~/.cache/sunstoneos/build/cpu-a76-dvfs/evidence/`,
`evidence-m/`, `evidence-r/` and the build directories beside them.
