# Board session for the multicore performance changes

Status: **built and frozen; not run.** The lock bookkeeping, IRQ exit, wake
placement and late-tick changes of
[the multicore performance design](../designs/multicore-performance-1025.md)
merge only after this session passes. It also measures the counter skew that
the lock-free counter clock assumes.

The session reuses the returning eight-core workload with a CPU0 return
deadline, profile `all-core-task-workload-deadline-v1`, as
[rank-8-workload-bisect.md](rank-8-workload-bisect.md) runs it, and the
returning-profile machinery of `work/refreeze-1025` (`89dc4d4c`), which pins
it to native images built from bringup `d502f8ea`. Two native images differ
only in the four board-gated changes, and both carry the measurement phase of
`SUNSTONE_A733_PERF_SESSION` (`platform/a733-smp-cached/perf_session.c`). It
runs while the unchanged workers wait: the workers' code and the workload's
data addresses are the same as without it.

The background tasks run throughout the measurements, because the returning
profile ends a core's run when that core's idle task first starts. No core is
ever idle, so this session cannot measure the wake-placement change, which
acts only between idle cores. Its evidence stays the QEMU and model figures
of the design.

## Frozen sets

`M=~/.cache/sunstoneos/evidence/multicore-performance-session`

| Set | Source | Native image `a733-smp-supervised` | Bundle |
| --- | --- | --- | --- |
| Control | `work/mc-perf-control-1025`: refreeze `89dc4d4c` and the measurement commit | `f9badd16992f15e2b342a57d0fd5378045df44c370eed3386f9bc05a4cb02ee8` | `$M/control/bundle-deadline` |
| Changed | `work/mc-perf-session-1025`: refreeze `89dc4d4c`, the four changes, the design and the measurement commit | `d645d10568aaeb8cc76f754fa97091d4380c79087c25e992a70932531c33e221` | `$M/changed/bundle-deadline` |

Each set directory holds:

- `image/`: the native ELF, its map and build identity, and the cached
  diagnostic built with the same option;
- `source/`: the tree that built it; `binding-source/`: the same tree with
  the four constants that refreeze pins to its own image (image hash, worker
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
  3-3 survey and records of 2026-10-25 (`board-session-20261025-3`), which
  shows that the bundle, admission, host evidence and launcher identity
  validate. Its command file is renamed `command.dry-run-not-for-launch.json`;
  it admits nothing.

Offline results, on both sets: the deadline profile, workload and identity
builds; the deadline success, stalled-core, stalled-no-tick and six GIC
restoration cases; the workload success and wrong-rounds cases; the identity
and deadline host checks. The task-workload host check
(`tools/all-core-task-workload-host.py`) passes on the control and fails on
the changed set, because it requires one kernel tick per tick IRQ and the
late-tick change accounts up to three; that profile is not part of this
session, and the check's owner must relax it before that profile runs with
the change.

## Predictions

The model column is the deadline profile's own instruction model on the
frozen bundle (`offline/deadline-check/success.bin`, decoded by
`$M/decode-results.py`).

| Measurement | Control | Changed | Model (control; changed) |
| --- | --- | --- | --- |
| Rounds, background progress, SGIs, failure | 32, nonzero, nonzero, 0 on every core | The same | Met by both |
| Cycles of 64 critical-section pairs, per core | Base value | Lower on every core: 22 `DSB SY` fewer per pair | 34,945–35,806 instructions; 33,537–34,345 |
| Cycles of an SGI to the own core, per core | Base value | About the cost of 3 barriers lower; reported only | 934–1,005; 907–1,019 |
| Counter skew, each direction | At least −2 ticks | The same | 0 or one model quantum (250) |
| Forced late tick: tick IRQs; kernel ticks | Equal, 5 or 6 | 2; 5 or 6 | 6, 6; 2, 6 |

On the board the cycle counter counts core cycles. The model counts one per
instruction and its tick period is shorter than one sample, so its core 0
figures include a tick. A skew below −2,400 ticks (100 microseconds) on any
core pair would make the lock-free counter clock fault on that board.

## Commands

Set the variables of [the README](README.md#names-used-in-every-runbook) with
`PORT=3-3`, then:

```sh
M=~/.cache/sunstoneos/evidence/multicore-performance-session
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

From each source tree:

```sh
~/.cache/sunstoneos/bin/cfg.sh <source> ~/.cache/sunstoneos/build/mc-perf-session-<k> -DSUNSTONE_A733_PERF_SESSION=ON
cmake --build ~/.cache/sunstoneos/build/mc-perf-session-<k> --target a733-smp-supervised a733-smp-cached-diagnostic -j 12
$PY $M/pin-binding.py $M/<k>/binding-source $M/<k>/image/a733-smp-supervised.elf
$PY $M/derive-template.py $M/<k>/template $M/<k>/image/a733-smp-supervised.elf <build identity>
(cd $M/<k>/binding-source && $PY tools/check-a733-smp-supervised.py prepare --image $M/<k>/image/a733-smp-supervised.elf \
   --image-manifest $M/<k>/template/image-input-manifest.json --admission $M/<k>/template/admission.json \
   --output $M/<k>/template/prepared)
(cd $M/<k>/binding-source && $PY tools/prepare-primary-publication-probe.py --image $M/<k>/image/a733-smp-supervised.elf \
   --output $M/<k>/primary-publication/prepared --clang /usr/lib/llvm-21/bin/clang \
   --lld ~/.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld --objcopy /usr/lib/llvm-21/bin/llvm-objcopy)
$M/run-offline.sh $M/<k>
```

`checks/a733-smp-cached.py --perf-session` runs the measurement phase of each
tree's cached diagnostic in the eight-core instruction model; `$M/<k>/model/`
holds the results. `$M/patch-points.py` reads the overlay's patch-point words
and the pinned layout of an image.
