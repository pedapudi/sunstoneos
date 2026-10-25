# Rank 7: all-core first-task diagnostic

Status: **ready** after the coordinator reviews `root-review-draft.json`, including the earlier-checker point below.
Bundle: `~/.cache/sunstoneos/evidence/board-bundles-20261023/rank-7-first-task/`.

The diagnostic runs the frozen eight-core supervised ELF
`762237cd0f023a2c6895167b347352bb383e1012b5c39d1f8f0139278a68dba3` with a
returning overlay. Every core constructs its tasks, starts the scheduler and
restores its first FreeRTOS task context; an SVC at the first idle-task
instruction records the context. No task body, timer interrupt or SGI
workload runs. Every core then cleans the 145-page owned writable interval,
CPU0 restores its controls, the seven secondaries reach OFF and the monitor
returns to the ROM. [The supervised SMP record](../a733-smp-supervised.md#all-eight-cpus-reach-their-first-task-context-and-return-safely)
describes it. It passed once on port 3-1 on 2026-10-18
(`all-core-first-task-20261018/physical-010/`).

## What the current runner refused, and the refreeze

The current `tools/run-native-startup-probe.py` refuses the 2026-10-18
bundle before any device request. Its loaders pin source digests that have
changed since:

- Build manifest sources (`Identity source changed.`):
  `boot/a733/all-core-first-task-probe.S` and
  `tools/prepare-secondary-identity-probe.py` (the workload diagnostic was
  added to both), `boot/a733/native-el3-cache-probe.S`,
  `include/a733_cpu_start.h` (comment wording only) and
  `platform/a733-smp-cached/boot.h` (resident-image constants, compiled only
  with `SUNSTONE_A733_RESIDENT`).
- Host evidence (`First-task host evidence differs.`): eight of the eleven
  pinned host tools, including `tools/run-native-startup-probe.py`,
  `tools/check-a733-smp-supervised.py`, `tools/check-a733-smp.py` and
  `tools/usb-access.py`.
- Instruction evidence: the model `checks/a733-smp-cached.py`, which gained
  instruction counting and two architectural registers, and the success
  checker `checks/all-core-first-task-probe.py`. The primary-preparation
  snapshot that the instruction checks resume from is itself pinned to the
  old model.

The current builder, run on the same ELF and primary-publication bundle,
produces an overlay and payload segments byte-identical to the accepted
ones (`overlay-comparison.json`). The executed bytes are therefore
the bytes that passed on 2026-10-18. The refreeze regenerates only the
records:

- `build/`: rebuilt by the current `tools/prepare-secondary-identity-probe.py
  --first-task`; its manifest (`11ce5fe2…`) pins the current sources.
- A new primary-preparation snapshot from `checks/secondary-identity-probe.py`,
  whose own three cases pass (`cpu-refreeze/identity-check/`).
- The three instruction cases, rerun with the current model
  (`cpu-refreeze/first-task-check/`): the success case passes eight
  first-task entries, 9,280 cleaned lines per core and matched return, and
  its report is byte-identical to the 2026-10-18 report; the
  wrong-task-argument and failed-EL3-restoration cases retain the cores.
  `tools/all-core-first-task-host.py` pins the two failure cases to the
  earlier checker `113e6336…` (`PRIOR_CHECKER`), so they ran from a copy of
  the current tree with only that checker replaced
  (`cpu-refreeze/shadow-prior-checker/`). The multicore owner may instead
  update the pin; the reviewer must accept one of the two.
- The nine host cases of `checks/all-core-first-task-host.py`, with the
  current host tools (`host/software.json`, `75a1559a…`).

The current runner's own loaders (`validate_bundle`, the host profile's
`load` and `validate_software`, run from `bringup`) accept the bundle, and
`cpu-prepare.py` binds it to a saved port 3-3 survey offline. Every step is
in `common/cpu-refreeze.sh`, with its log in `common/cpu-refreeze.log`.

## Commands

Set the variables in [the README](README.md#names-used-in-every-runbook).
The board is on the instance that rank 6 returned.

1. Turn the instruction cache off and survey:

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/r7-cache-off 120 \
     "Board $PORT: returning instruction-cache disable before the first-task diagnostic" \
     "The helper turns the instruction cache off and the 40 SRAM bytes at 0x73000 are restored." \
     -- $PY -B $V/common/cache-off.py $S/r7-cache-off
   $PY -B $V/common/bundle-run.py $PORT $S/r7-survey 240 \
     "Board $PORT: returning SRAM survey before the first-task diagnostic" \
     "Nonce-bound launcher controls, restored SRAM and a responsive same ROM." \
     --prerequisite launch-survey-software=$SW \
     --artifact $SB/manifest.json --artifact $SB/launch-survey.bin \
     -- $PY -B $SR/tools/run-launch-survey.py run --fel $FEL --build $SB \
        --software $SW --database {database} --evidence $S/r7-survey \
        --claim {claim} --owner {owner}
   ```

2. Check each item of `$V/rank-7-first-task/root-review-draft.json`, then write the
   review record that the admission cites. The preparation script refuses a
   record whose `status` does not begin with `Coordinator reviewed`:

   ```sh
   $PY -c 'import json,sys; r=json.load(open(sys.argv[1])); r["status"]="Coordinator reviewed "+sys.argv[3]; open(sys.argv[2],"x").write(json.dumps(r,indent=2)+"\n")' \
     $V/rank-7-first-task/root-review-draft.json $S/r7-root-review.json "the rank 7 bundle for port $PORT."
   ```

3. Bind the bundle to the current instance (offline):

   ```sh
   $PY -B $V/rank-7-first-task/cpu-prepare.py --bundle $V/rank-7-first-task \
     --port $PORT --survey $S/r7-survey/session.json --root-review $S/r7-root-review.json \
     --continuity $S/ddr/session.json --continuity $S/kernel/session.json \
     --continuity $S/cache-off-1/session.json --continuity $SURVEY_WARM/session.json \
     --continuity $S/timeout/session.json --continuity $S/r6-cache-off/session.json \
     --continuity $S/r6-survey/session.json --continuity $S/r6-thermal/session.json \
     --continuity $S/r7-cache-off/session.json \
     --output $S/r7-prepare
   ```

   `SURVEY_WARM` is `$S/survey-warm-1` on port 3-1 and `$S/survey-warm-2` on
   port 3-3. The script checks the build, host evidence and frozen image with
   the current runner's own loaders, refuses unless the survey's launcher
   gates all pass, and writes `rationale.json`, `recovery.json`, `ddr.json`,
   `platform.json`, `admission.json`, `prepared/` (with a fresh reservation,
   epoch, generation and lease) and `command.json`.

4. Run the diagnostic once:

   ```sh
   $PY -B $V/common/exec-command.py $S/r7-prepare/command.json
   ```

   `command.json` runs `common/bundle-run.py` with a 300 s queue timeout,
   the prerequisites `supervised-ram-recovery` (`r7-prepare/recovery.json`)
   and `native-startup-software` (`host/software.json`), artifacts covering
   the build manifest, software evidence, survey and `prepared.json`, and the
   runner `tools/run-native-startup-probe.py run --build ... --bundle
   $S/r7-prepare/prepared --software ... --survey $S/r7-survey/session.json
   --database ... --claim ... --owner coordinator --evidence
   $S/r7-prepare/runtime`. The runner checks its own claim.

   Success: the runner exits 0 and `bundle-run.py` prints
   `"result": "passed"`. `$S/r7-prepare/runtime/session.json` then has
   `complete`, `rom_usable` and `dram_restored` true and a `result` with
   eight first-task entries, 9,280 cleaned lines per core and seven OFF
   secondaries.

## Admission

No warm-return chain is involved: the native runner requires a survey and
recovery record of the live instance, the prepared admission bound to it,
and the claim. The forced-stall return needs no extra record. The DDR and
platform records cite the continuity list of step 3 and the coordinator's
review.

## Stop conditions

Any nonzero exit: stop this board and keep the claim. A launch that does not
return within the runner's 10 s window means a core may be hung outside the
boot core's monitor return; keep the runner's lock, record the failure and
request a power removal for this port. Do not retry, and do not start rank
8 on this board.
