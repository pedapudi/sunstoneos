# Board session for the multicore performance changes

Status: **built and frozen; not run.** The lock bookkeeping, IRQ exit, wake
placement and late-tick changes of
[the multicore performance design](../designs/multicore-performance-1025.md)
merge only after this session passes. It also measures the counter skew that
the lock-free counter clock assumes.

The session reuses the returning eight-core workload with a CPU0 return
deadline, profile `all-core-task-workload-deadline-v1`, as
[rank-8-workload-bisect.md](rank-8-workload-bisect.md) runs it, and the
returning-profile machinery of bringup `c4eadf9f`, which pins it to the
second refrozen native image `6fd7af86` with the EL3 security writes
(`beff729b`; bundles in `~/.cache/sunstoneos/evidence/refreeze-el3-20261025/`),
qualified on port 3-3. Two native images differ
only in the four board-gated changes, and both carry the measurement phase of
`SUNSTONE_A733_PERF_SESSION` (`platform/a733-smp-cached/perf_session.c`). It
runs while the unchanged workers wait: the workers' code and the workload's
data addresses are the same as without it.

The background tasks run throughout the measurements, so no core idles, as
in the profile's qualified board runs. The wake-placement change acts only
between idle cores, so this session does not measure it; the last section
proposes the change to the measurement phase that would.

## Frozen sets

`M=~/.cache/sunstoneos/evidence/multicore-performance-session-c4eadf9f`

| Set | Source | Native image `a733-smp-supervised` | Bundle |
| --- | --- | --- | --- |
| Control | `work/mc-perf-control-1025`: bringup `c4eadf9f` and the measurement commits | `9d1ed4117d877ef27502da1663cec294386f75140d8cbfd608b14a05f4fca503` | `$M/control/bundle-deadline` |
| Changed | `work/mc-perf-session-1025`: bringup `c4eadf9f`, the four changes, the design and the measurement commits | `5d240e3e2ddba716bf6f29582d13390ba4df292d67cebda2f93ef75972927fab` | `$M/changed/bundle-deadline` |

`$M/freeze-set.sh` builds and freezes one set from its branch and worktree.

Each set directory holds:

- `image/`: the native ELF, its map and build identity, and the cached
  diagnostic built with the same option;
- `source/`: the tree that built it; `binding-source/`: the same tree with
  the four constants that the refreeze pins to its own image (image hash, worker
  address, `.bss` end, EL1 vector address) set to this image by
  `$M/pin-binding.py`, and nothing else changed;
- `template/`: the admission template, derived from refreeze's by
  `$M/derive-template.py`, and `template/prepared/`, made by
  `tools/check-a733-smp-supervised.py prepare`;
- `primary-publication/prepared/`: the primary publication overlay;
- `offline/`: the identity, workload and return-deadline profiles, their
  instruction models and the host checks with modeled FEL, run by
  `$M/run-offline.sh`;
- `bundle-deadline/`: `build/`, `host/`, `template/` and
  `root-review-draft.json`, laid out as the rwp3 bundles are, made by
  `$M/assemble-bundle.py`;
- `prepare-session.py`: refreeze's `cpu-prepare.py` bound to this set's
  `binding-source/`; `binding-dry-run/`: its output against the saved port
  3-3 survey and records of 2026-10-25 (`board-session-20261025-5`), which
  shows that the bundle, admission, host evidence and launcher identity
  validate. Its command file is renamed `command.dry-run-not-for-launch.json`;
  it admits nothing.

Offline results, on both sets: the deadline profile, workload and identity
builds; the deadline success, stalled-core, stalled-no-tick and six GIC
restoration cases; the workload success and wrong-rounds cases; the identity
deadline and task-workload host checks. The task-workload host check accepts
between a third of CPU0's ticks and all of them as tick IRQs, because the
late-tick change accounts up to three ticks per IRQ.

## Predictions

The model column is the deadline profile's own instruction model on the
frozen bundle (`offline/deadline-check/success.bin`, decoded by
`$M/decode-results.py`).

| Measurement | Control | Changed | Model (control; changed) |
| --- | --- | --- | --- |
| Rounds, background progress, SGIs, failure | 32, nonzero, nonzero, 0 on every core | The same | Met by both |
| Cycles of 64 critical-section pairs, per core | Base value | Lower on every core: 22 `DSB SY` fewer per pair | 34,945–35,806 instructions; 33,537–34,315 |
| Cycles of an SGI to the own core, per core | Base value | About the cost of 3 barriers lower; reported only | 934–1,026; 907–1,019 |
| Counter skew, each direction | At least −2 ticks | The same | 0 or one model quantum (250) |
| Forced late tick: tick IRQs; kernel ticks | Equal, 5 or 6 | 2; 5 or 6 | 5, 5; 2, 6 |

On the board the cycle counter counts core cycles. The model counts one per
instruction and its tick period is shorter than one sample, so its core 0
figures include a tick. A skew below −2,400 ticks (100 microseconds) on any
core pair would make the lock-free counter clock fault on that board.

## Commands

Set the variables of [the README](README.md#names-used-in-every-runbook) with
`PORT=3-3`, then:

```sh
M=~/.cache/sunstoneos/evidence/multicore-performance-session-c4eadf9f
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
   even `n`: turn the instruction cache off and survey, bind, run and
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
   $PY -B $M/decode-results.py $S/mp$n-$k-prepare/runtime/report.bin $k > $S/mp$n-$k-results.json
   ```

   Then take the 91-word capture. The first run's continuity list starts with
   the DDR record of step 1; each later run appends the previous run's
   records.
4. Stop at the first deadline return, non-return or failed capture, and
   follow the outcomes in
   [the rank 8 bisection runbook](rank-8-workload-bisect.md#outcomes-of-one-launch).
   A control failure stops the session before any changed run.

Port 3-1 repeats the sequence only after port 3-3 passes it.

## Pass criteria

- Every run completes with controls and DRAM restored, `complete` true in its
  results, and each capture equal to the clean one.
- In each of the three pairs of runs, the changed image's cycle counts are
  lower than the control's on every core. The difference per pair, divided by
  22, estimates one `DSB SY` on each core type.
- No skew value is below −2,400 ticks.
- The late tick matches the predictions.

A failed criterion keeps the four changes off `bringup`; record it in the
design and the handoff.

## How the sets were built

```sh
$M/freeze-set.sh control work/mc-perf-control-1025 ~/git/sunstoneos-worktrees/mc-perf-control-1025
$M/freeze-set.sh changed work/mc-perf-session-1025 ~/git/sunstoneos-worktrees/mc-perf-session-1025
```

The script configures `~/.cache/sunstoneos/build/mc-perf-session-c4-<k>` with
`-DSUNSTONE_A733_PERF_SESSION=ON` and builds the native image, then runs
`$M/pin-binding.py`, `$M/derive-template.py`,
`tools/check-a733-smp-supervised.py prepare`,
`tools/prepare-primary-publication-probe.py`, the cached-diagnostic model
`checks/a733-smp-cached.py --perf-session` (into `$M/<k>/model/`),
`$M/run-offline.sh` and `$M/assemble-bundle.py`. `$M/patch-points.py` reads
the overlay's patch-point words and the pinned layout of an image.

## Proposal: measure wake placement between idle cores

The deadline profile keeps the native idle loop (DSB, then WFI at EL0), so
idle cores need no overlay change. The smallest change is in the measurement
phase alone: hold the background tasks as well as the workers, then run two
unpinned tasks, one with `A733_SMP_A55_MASK` and one with all cores, that each
sleep one tick 64 times and count their wakes per core in `reserved[7]` and
`reserved[8]` (`$M/idle-placement-proposal/idle-placement.patch`, about 130
lines, and 128 ticks of run time). Built on the changed tree, that variant
passes every offline step above, and its deadline and workload models record
all 64 wakes of the A55-mask task on core 0 and all 64 of the all-core task
on core 7 (`$M/idle-placement-proposal/set/`). The control would place the
A55-mask task on core 5. No OS image is needed. It adds idle WFI on every core
to a profile whose ten qualified runs never idled, so it belongs in a second
session after this one passes.
