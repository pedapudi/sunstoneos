# Board session for the multicore performance changes

Status: **planned; nothing in it is built or has run.** The lock bookkeeping,
IRQ exit, wake placement and late-tick changes of
[the multicore performance design](../designs/multicore-performance-1025.md)
merge only after this session passes. It also measures the counter skew that
the lock-free counter clock of `work/mc-perf-a-1025` assumes.

The session reuses the returning eight-core workload with a CPU0 return
deadline, profile `all-core-task-workload-deadline-v1`, and its runbook
[rank-8-workload-bisect.md](rank-8-workload-bisect.md). With the status-masked
GICR_CTLR comparison, that profile completed ten times in one port 3-3
lifetime on 2026-10-25 (`board-session-20261025-3/port-3-3`, bundles
`workload-bisect-20261025/rwp3`). The session runs the same profile on two new
native images, a control and a changed one, that differ only in the four
board-gated changes. Both carry the same measurement additions.

## What the session measures

Every run measures all four items below; the control and the changed image
differ only in the expected values.

| Measurement | Where | Expected with the control | Expected with the changes |
| --- | --- | --- | --- |
| SGI, tick and floating-point progress on all eight cores | The workload's existing per-core results | 32 rounds per core, background progress, SGIs on every secondary, ticks on CPU0, no failure | The same |
| PMU cycles per critical-section pair and per SGI round trip to the sending core, per core | New per-core words, least of 16 samples | The base cycle counts | Lower on every core; the difference divided by the removed barriers estimates one `DSB SY` on each core type |
| `CNTPCT_EL0` skew between core 0 and each other core | New per-core words, most negative difference over 1,024 exchanges in each direction | Not compared | The same as the control; at most a few ticks |
| A forced late tick on core 0 | New CPU0 words: tick IRQs and kernel ticks across a 5.5-period masked interval | One tick IRQ per due period | At most three due periods per tick IRQ: two IRQs for five or six periods; the kernel tick count advances by the same amount |
| Where unpinned tasks run | New per-core histograms of 64 one-tick sleeps for two tasks | A55-mask task: mostly core 5; all-core task: core 7 | A55-mask task: core 0, the tick's core; all-core task: core 7 |

A skew larger than 2,400 ticks (100 microseconds) would make the lock-free
counter clock fault on that board. Report the largest value per core pair
whatever it is.

## What has to be built and frozen

Nothing below exists yet. Each step needs its own review before any board
run.

1. **Measurement additions.** One commit, under a new compile definition
   `SUNSTONE_A733_PERF_SESSION`, in `platform/a733-smp-cached/diagnostic.c` and
   `platform/a733-smp-cached/platform.c`. They write only the reserved words of
   each core's 128-byte record in `a733_smp_core_results`, which the runner
   already reads back after a completed run:
   - In `a733_smp_pre_scheduler`, after the 256-increment probe, each core
     enables the cycle counter and EL0 access to it (`PMCR_EL0.E`,
     `PMCNTENSET_EL0` bit 31, `PMUSERENR_EL0.EN` and `.CR`). Core 0 then
     exchanges 1,024 counter samples with each other core in turn: one side
     publishes `ISB; MRS CNTPCT_EL0` with a release store, the other side
     loads it with acquire and reads its own counter. Each core records the
     most negative difference in each direction, in signed ticks.
   - After its 32 rounds, each worker records the least of 16 samples of the
     `PMCCNTR_EL0` cycles of 64 critical-section pairs, and of 16 SGIs to its
     own core (`vInterruptCore(0, core)`), each until its next instruction.
   - The tick branch of `vApplicationIRQHandler` counts tick IRQs in CPU0's
     record. CPU0's worker then records that count and `xTaskGetTickCount()`,
     enters a critical section, waits until the counter has advanced 132,000
     ticks (5.5 periods), leaves it, sleeps one tick and records both deltas.
   - After CPU0 has validated every core's rounds and background progress, it
     suspends the background tasks. It then runs two unpinned tasks at worker
     priority, one with affinity `A733_SMP_A55_MASK` and one with all cores.
     Each sleeps one tick 64 times and counts its wakes per core in the
     cores' records. CPU0 then completes the workload as before.
2. **Model and host support.** `checks/workload-bisect-probe.py` and
   `checks/all-core-task-workload-probe.py` run their success case with the
   definition; `tools/workload-bisect-host.py` and
   `tools/all-core-task-workload-host.py` decode and report the new words
   without pass thresholds. The native pins move to the new images:
   `NATIVE` in `tools/prepare-primary-publication-probe.py`, the
   `a733_smp_core_results` address `0x41201080` and the completion patch
   address `0x410012ec` in `tools/all-core-task-workload-host.py`, the result
   offset `0x4280` in both host modules and the snapshot offsets in
   [the rank 8 bisection runbook](rank-8-workload-bisect.md#reading-a-deadline-return).
   The overlay builder checks every displaced word at its symbol-relative
   patch points, so those need no change if the checks pass.
3. **Two source trees.** The control is `work/mc-perf-a-1025` with steps 1
   and 2 applied; the changed tree is `work/mc-perf-1025` with the same
   commits. They differ only in the four board-gated changes.
4. **Two native images.** In each tree, configure with
   `~/.cache/sunstoneos/bin/cfg.sh SRC BUILD -DSUNSTONE_A733_PERF_SESSION=ON`
   and build the target `a733-smp-supervised`, the target of the frozen
   native image `762237cd…`. Record each ELF's SHA-256 and build identity.
5. **Bundles per image**, as `workload-bisect-20261025/rwp3` was built from
   `762237cd…`:
   - the primary-prefix checkpoint (as
     `board-bundles-20261023/cpu-refreeze/identity-check/primary-prefix.pickle`);
   - `tools/prepare-secondary-identity-probe.py --task-workload
     --return-deadline` (profile 1 only; no `--quiet-background`), giving
     `build-deadline/` and `bundle-deadline/` with `host/software.json`,
     `template/` and `root-review-draft.json`;
   - the model runs of `rwp3/run-models.sh` for the deadline profile and the
     workload: `success`, `stalled-core`, `stalled-no-tick`, the four
     `restore-rwp` cases and `wrong-rounds`;
   - the host validation that writes `host-deadline/software.json`.
6. **Freeze** each set under
   `~/.cache/sunstoneos/evidence/multicore-performance-session/control/` and
   `.../changed/`, each with its `source/` tree, its native ELF, its
   `prepare-workload-bisect.py` (the rwp3 script bound to that source tree)
   and the model logs.

## Board sequence

One fresh port 3-3 lifetime, where profile 1 is qualified. Set the variables
of [the README](README.md#names-used-in-every-runbook) with `PORT=3-3`, and
`B` to the frozen control or changed set.

1. Cold DDR, the cache-off helper, a survey and the 91-word capture, as in
   steps 1 to 4 of [the rank 8 bisection runbook](rank-8-workload-bisect.md#commands),
   with `$B/bundle-deadline`.
2. Run the control first, then the changed image, then repeat the pair twice:
   six runs, each after the cache-off helper and a survey, each with a
   91-word capture after it and a new output directory.
3. Stop at the first deadline return or non-return; follow the outcomes in
   [the rank 8 bisection runbook](rank-8-workload-bisect.md#outcomes-of-one-launch).
   A control failure stops the session before any changed run.

Port 3-1 repeats the sequence only after port 3-3 passes it.

## Pass criteria

- Every run completes with controls and DRAM restored and each capture equal
  to the clean one.
- The changed image's cycle counts are lower than the control's on every
  core, in each of the three pairs of runs.
- The skew stays far below 2,400 ticks on every core pair.
- The late tick and the placement histograms match the table above.

A failed criterion keeps the four changes off `bringup`; record it in the
design and the handoff.
