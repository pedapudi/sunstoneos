# Board session for the multicore performance changes

Status: **second session built and frozen; not run.** The lock bookkeeping,
IRQ exit, wake placement and late-tick changes of
[the multicore performance design](../designs/multicore-performance-1025.md)
merge only after a session passes. The first session ran on port 3-3 on
2026-10-25: all six runs completed and every capture equalled the clean one,
but its cycle measurement timed lock contention between the eight cores and
failed its criterion (the design records the figures). This runbook is for
the second session, whose measurement times one core at a time.

The session reuses the returning eight-core workload with a CPU0 return
deadline, profile `all-core-task-workload-deadline-v1`, as
[rank-8-workload-bisect.md](rank-8-workload-bisect.md) runs it, and the
returning-profile machinery of bringup `c4eadf9f`, which pins it to the
second refrozen native image `6fd7af86` with the EL3 security writes
(`beff729b`; bundles in `~/.cache/sunstoneos/evidence/refreeze-el3-20261025/`),
qualified on port 3-3. Two native images differ only in the four
board-gated changes, and both carry the measurement phase of
`SUNSTONE_A733_PERF_SESSION` (`platform/a733-smp-cached/perf_session.c`,
record version `PERF2`). It runs while the unchanged workers wait: the
workers' code and the workload's data addresses are the same as without it.

## The measurement

- **Critical-section pairs, one core at a time.** The measurement tasks take
  turns in core order. While one core times, the other measurement tasks
  wait in WFE, so only core 0's tick takes the kernel locks meanwhile. Each
  core times 32 samples of 64 `taskENTER_CRITICAL`/`taskEXIT_CRITICAL` pairs
  with `PMCCNTR_EL0` and records the median and the 11th and 22nd smallest
  samples. For 32 samples that interval contains the true median with
  probability 95.0%.
- **SGI to the own core**, 32 samples, median: the timing ends when the
  core's SGI handler has counted the SGI.
- **Counter skew** between core 0 and every other core, 1,024 exchanges in
  each direction, before the scheduler starts.
- **A forced late tick** on core 0: 5.5 periods behind the interrupt mask.

The background tasks run throughout, so no core idles, as in the profile's
qualified runs. The wake-placement change acts only between idle cores, so
this session does not measure it; the last section proposes how a later
session could.

## Predictions

Each pair at EL0 makes two supervisor calls and four lock calls; the changed
image executes 22 fewer `DSB SY` per pair. A `DSB SY` with no outstanding
memory access costs on the order of the store-buffer drain, taken here as 10
to 40 cycles; the Cortex-A55 and Cortex-A76 manuals give no figure. The
predicted saving is therefore 220 to 880 cycles per pair, or 14,000 to 56,000
per 64-pair sample, on every core.

The model column is the deadline profile's own instruction model on the
frozen bundles (`offline/deadline-check/success.bin`, decoded by
`$M/decode-results.py`); its cycle counter advances one per instruction.
`$M/analyse-session.py` applied to those two results passes and reports 22
instructions saved per pair on both core types, one per removed `DSB SY`.

| Measurement | Control | Changed | Model (control; changed) |
| --- | --- | --- | --- |
| Rounds, background progress, SGIs, failure | 32, nonzero, nonzero, 0 on every core | The same | Met by both |
| Median cycles of 64 pairs, per core | Base value | 14,000 to 56,000 lower on every core | 34,965–35,684 instructions; 33,559–34,267 |
| Confidence interval half-width of that median | At most 5% of it | The same | At most 1.1% of the median on both |
| Median cycles of an SGI to the own core until counted | Base value | About the cost of 3 barriers lower; reported only | 939–1,009; 912–982 |
| Counter skew, each direction | 3 to 8 ticks, as in the first session | The same | 0 or one model quantum (250) |
| Forced late tick: tick IRQs; kernel ticks | Equal, 5 or 6 | 2; 5 or 6 | 5, 5; 2, 5 |

## Pass criteria

`$M/analyse-session.py` applies them to the six decoded results:

```sh
$PY $M/analyse-session.py $S/mp1-control-results.json $S/mp3-control-results.json $S/mp5-control-results.json \
  --changed $S/mp2-changed-results.json $S/mp4-changed-results.json $S/mp6-changed-results.json
```

- Every run completes with controls and DRAM restored, `version_ok` true in
  its results, and each capture equal to the clean one.
- **Valid measurement:** in every run, every core's confidence interval
  half-width is at most 5% of its median. A run that fails this is not
  evidence either way; record it and stop.
- **Lower on every core:** for every core, every changed run's median is below
  every control run's median. With three runs of each image, an exact
  one-sided rank test of no difference gives that ordering with probability
  1/20 per core.
- No skew value is below −2,400 ticks, and the late tick matches the
  prediction.

The script also reports, per core type, the saving per pair (the difference
of the medians of the run medians, divided by 64) and per removed `DSB SY`
(divided by 22). A failed criterion keeps the four changes off `bringup`;
record it in the design and the handoff.

## Frozen sets

`M=~/.cache/sunstoneos/evidence/multicore-performance-session-v2`

| Set | Source | Native image `a733-smp-supervised` | Bundle |
| --- | --- | --- | --- |
| Control | `work/mc-perf-control-1025`: bringup `c4eadf9f` and the measurement commits | `70216b5aa7511faa4f530bbd4325ef4503f06249651ae62309a12d2c2c7db457` | `$M/control/bundle-deadline` |
| Changed | `work/mc-perf-session-1025`: bringup `c4eadf9f`, the four changes, the design and the measurement commits | `3c156207802d8f433e05ee747f3e989b6e6e64d08901621eb398fbf7965208c2` | `$M/changed/bundle-deadline` |

Each set directory holds `image/` (the native ELF, map and build identity),
`source/` and `binding-source/` (the same tree with the four constants that
the refreeze pins to its own image set to this image by `$M/pin-binding.py`),
`template/` and `template/prepared/`, `primary-publication/prepared/`,
`offline/` (the identity, workload and return-deadline profiles, their
instruction models and host checks with modeled FEL, by `$M/run-offline.sh`),
`model/` (the cached diagnostic's model with the option), `bundle-deadline/`
(by `$M/assemble-bundle.py`), `prepare-session.py` (the second refreeze's
`cpu-prepare-22501205.py` bound to `binding-source/`) and `binding-dry-run/`.

Offline results, on both sets: the deadline success, stalled-core,
stalled-no-tick and six GIC restoration cases; the workload success and
wrong-rounds cases; the identity, deadline and task-workload host checks; and
a binding dry run against the saved port 3-3 survey of
`board-session-20261025-5`, whose command file is renamed
`command.dry-run-not-for-launch.json` and admits nothing.

## Commands

Set the variables of [the README](README.md#names-used-in-every-runbook) with
`PORT=3-3`, then:

```sh
M=~/.cache/sunstoneos/evidence/multicore-performance-session-v2
S=<the session directory of a fresh port 3-3 lifetime>
```

1. After a power removal: cold DDR, then the 91-word capture, as in step 1 of
   [the rank 8 bisection runbook](rank-8-workload-bisect.md#commands). Its
   secondary-power words must equal the clean capture.
2. Review each bundle's draft and write its review record:

   ```sh
   for k in control changed; do
     $PY -c 'import json,sys; r=json.load(open(sys.argv[1])); r["status"]="Coordinator reviewed "+sys.argv[3]; open(sys.argv[2],"x").write(json.dumps(r,indent=2)+"\n")' \
       $M/$k/bundle-deadline/root-review-draft.json $S/mp-root-review-$k.json "the $k multicore performance bundle for port 3-3."
   done
   ```

3. For run `n` from 1 to 6, with `k` = `control` for odd `n` and `changed` for
   even `n`: turn the instruction cache off and survey, bind, run, decode and
   capture.

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/mp$n-cache-off 120 \
     "Board $PORT: returning instruction-cache disable before multicore performance run $n" \
     "The helper turns the instruction cache off and the 40 SRAM bytes at 0x73000 are restored." \
     -- $PY -B $V/common/cache-off.py $S/mp$n-cache-off
   $PY -B $V/common/bundle-run.py $PORT $S/mp$n-survey 240 \
     "Board $PORT: returning SRAM survey before multicore performance run $n" \
     "Nonce-bound launcher controls, restored SRAM and a responsive same ROM." \
     --prerequisite launch-survey-software=$SW \
     --artifact $SB/manifest.json --artifact $SB/launch-survey.bin \
     -- $PY -B $SR/tools/run-launch-survey.py run --fel $FEL --build $SB \
        --software $SW --database {database} --evidence $S/mp$n-survey \
        --claim {claim} --owner {owner}
   $PY -B $M/$k/prepare-session.py --bundle $M/$k/bundle-deadline --port 3-3 \
     --survey $S/mp$n-survey/session.json --root-review $S/mp-root-review-$k.json \
     --continuity <every record of this lifetime from the DDR start, in order> \
     --output $S/mp$n-$k-prepare
   $PY -B $V/common/exec-command.py $S/mp$n-$k-prepare/command.json
   $PY -B $M/decode-results.py $S/mp$n-$k-prepare/runtime/report.bin > $S/mp$n-$k-results.json
   ```

   Then take the 91-word capture. The first run's continuity list starts with
   the DDR record of step 1; each later run appends the previous run's
   records.
4. Stop at the first deadline return, non-return or failed capture, and
   follow the outcomes in
   [the rank 8 bisection runbook](rank-8-workload-bisect.md#outcomes-of-one-launch).
   A control failure stops the session before any changed run.
5. Run `$M/analyse-session.py` as above.

Port 3-1 repeats the sequence only after port 3-3 passes it.

## How the sets were built

```sh
$M/freeze-set.sh control work/mc-perf-control-1025 ~/git/sunstoneos-worktrees/mc-perf-control-1025
$M/freeze-set.sh changed work/mc-perf-session-1025 ~/git/sunstoneos-worktrees/mc-perf-session-1025
```

The script configures `~/.cache/sunstoneos/build/mc-perf-session-v2-<k>` with
`-DSUNSTONE_A733_PERF_SESSION=ON` and builds the native image, then runs
`$M/pin-binding.py`, `$M/derive-template.py`,
`tools/check-a733-smp-supervised.py prepare`,
`tools/prepare-primary-publication-probe.py`, the cached-diagnostic model
`checks/a733-smp-cached.py --perf-session`, `$M/run-offline.sh` and
`$M/assemble-bundle.py`. `$M/patch-points.py` reads the overlay's patch-point
words and the pinned layout of an image.

## Proposal: measure wake placement between idle cores

The deadline profile keeps the native idle loop (DSB, then WFI at EL0), so
idle cores need no overlay change. The smallest change is in the measurement
phase alone: hold the background tasks as well as the workers, then run two
unpinned tasks, one with `A733_SMP_A55_MASK` and one with all cores, that each
sleep one tick 64 times and count their wakes per core
(`~/.cache/sunstoneos/evidence/multicore-performance-session-c4eadf9f/idle-placement-proposal/`,
a patch of about 130 lines against the first record version, and 128 ticks
of run time). Built on the changed tree, that variant passes every offline
step above, and its deadline and workload models record all 64 wakes of the
A55-mask task on core 0 and all 64 of the all-core task on core 7. The
control would place the A55-mask task on core 5. No OS image is needed. The
`PERF2` record uses all eleven free words, so the variant must give up one
measurement, such as the SGI timing, for its two counts. It adds idle WFI on
every core to a profile whose qualified runs never idled, so it belongs in a
session after this one passes.
