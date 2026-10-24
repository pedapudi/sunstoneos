# Rank 8: full returning eight-core workload

Status: **ready** after the coordinator reviews `root-review-draft.json`; runs only after ranks 6 and 7 pass on the same board.
Bundle: `~/.cache/sunstoneos/evidence/board-bundles-20261023/rank-8-workload/`.

The workload runs the same frozen ELF
`762237cd0f023a2c6895167b347352bb383e1012b5c39d1f8f0139278a68dba3` with
the workload overlay. Every core runs its production worker for 32
floating-point rounds, its background task, the production tick and SGI
handlers and FreeRTOS task switching; CPU0 services the timer. After all
eight workers finish, each core stops timer generation and interrupt
delivery, cleans the 145-page owned writable interval, and the monitor
returns to the ROM after the seven secondaries reach OFF. The USB observer
task is suppressed. [The supervised SMP record](../a733-smp-supervised.md#the-complete-returning-workload-passes-instruction-checks)
describes it. It has never run on a board.

## What the current runner refused, and the refreeze

The current `tools/run-native-startup-probe.py` refuses the frozen bundle
of `all-core-task-workload-20261019/` before any device request:

- Build manifest sources (`Identity source changed.`):
  `include/a733_cpu_start.h` (comment wording only) and
  `platform/a733-smp-cached/boot.h` (resident-image constants, compiled only
  with `SUNSTONE_A733_RESIDENT`).
- Host evidence (`Workload host evidence differs.`):
  `tools/check-a733-smp-supervised.py`, `tools/check-a733-smp.py` and
  `tools/usb-access.py`.
- Instruction evidence: the model `checks/a733-smp-cached.py`, and through
  it the primary-preparation snapshot that the instruction checks resume
  from.

The current builder, run on the same ELF and primary-publication bundle,
produces an overlay and payload segments byte-identical to the accepted
ones (`overlay-comparison.json`). The executed bytes are therefore
the bytes that the 2026-10-19 instruction checks ran. The refreeze
regenerates only the records:

- `build/`: rebuilt by the current `tools/prepare-secondary-identity-probe.py
  --task-workload`; its manifest (`73a422d7…`) pins the current sources.
- A new primary-preparation snapshot from `checks/secondary-identity-probe.py`
  (`cpu-refreeze/identity-check/`).
- The two instruction cases, rerun with the current model
  (`cpu-refreeze/workload-check/`): the success case completes 32 rounds,
  background progress and SGIs on every core and CPU0 ticks, cleans and
  returns, and its report is byte-identical to the 2026-10-19 report; the
  damaged-round-count case resumes from the new completed-worker checkpoint
  and retains all cores, also with a byte-identical report.
- The 13 host cases of `checks/all-core-first-task-host.py --task-workload`,
  with the current host tools (`host/software.json`, `84fd5210…`).

The current runner's own loaders (`validate_bundle`, the host profile's
`load` and `validate_software`, run from `bringup`) accept the bundle, and
`cpu-prepare.py` binds it to a saved port 3-3 survey offline. Every step is
in `common/cpu-refreeze.sh`, with its log in `common/cpu-refreeze.log`.

## Commands

Set the variables in [the README](README.md#names-used-in-every-runbook).
The board is on the instance that rank 7 returned. Rank 8 starts only
when rank 7 passed on this board and the rank 6 classifier
(`thermal-result.py`) reported every reading below 90 °C.

1. Turn the instruction cache off and survey:

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/r8-cache-off 120 \
     "Board $PORT: returning instruction-cache disable before the eight-core workload" \
     "The helper turns the instruction cache off and the 40 SRAM bytes at 0x73000 are restored." \
     -- $PY -B $V/common/cache-off.py $S/r8-cache-off
   $PY -B $V/common/survey.py $PORT $S/r8-survey "Board $PORT before the eight-core workload"
   ```

2. Check each item of `$V/rank-8-workload/root-review-draft.json`, then write the
   review record that the admission cites. The preparation script refuses a
   record whose `status` does not begin with `Coordinator reviewed`:

   ```sh
   $PY -c 'import json,sys; r=json.load(open(sys.argv[1])); r["status"]="Coordinator reviewed "+sys.argv[3]; open(sys.argv[2],"x").write(json.dumps(r,indent=2)+"\n")' \
     $V/rank-8-workload/root-review-draft.json $S/r8-root-review.json "the rank 8 bundle for port $PORT."
   ```

3. Bind the bundle to the current instance (offline):

   ```sh
   $PY -B $V/rank-8-workload/cpu-prepare.py --bundle $V/rank-8-workload \
     --port $PORT --survey $S/r8-survey/session.json --root-review $S/r8-root-review.json \
     --continuity $S/ddr/session.json --continuity $S/kernel/session.json \
     --continuity $S/cache-off-1/session.json --continuity $SURVEY_WARM/session.json \
     --continuity $S/timeout/session.json --continuity $S/r6-cache-off/session.json \
     --continuity $S/r6-survey/session.json --continuity $S/r6-thermal/session.json \
     --continuity $S/r7-cache-off/session.json --continuity $S/r7-survey/session.json \
     --continuity $S/r7-prepare/runtime/session.json --continuity $S/r8-cache-off/session.json \
     --output $S/r8-prepare
   ```

   `SURVEY_WARM` is `$S/survey-warm-1` on port 3-1 and `$S/survey-warm-2` on
   port 3-3. The script checks the build, host evidence and frozen image with
   the current runner's own loaders, refuses unless the survey's launcher
   gates all pass, and writes `rationale.json`, `recovery.json`, `ddr.json`,
   `platform.json`, `admission.json`, `prepared/` (with a fresh reservation,
   epoch, generation and lease) and `command.json`.

4. Run the diagnostic once:

   ```sh
   $PY -B $V/common/exec-command.py $S/r8-prepare/command.json
   ```

   `command.json` runs `common/bundle-run.py` with a 300 s queue timeout,
   the prerequisites `supervised-ram-recovery` (`r8-prepare/recovery.json`)
   and `native-startup-software` (`host/software.json`), artifacts covering
   the build manifest, software evidence, survey and `prepared.json`, and the
   runner `tools/run-native-startup-probe.py run --build ... --bundle
   $S/r8-prepare/prepared --software ... --survey $S/r8-survey/session.json
   --database ... --claim ... --owner coordinator --evidence
   $S/r8-prepare/runtime`. The runner checks its own claim.

   Success: the runner exits 0 and `bundle-run.py` prints
   `"result": "passed"`. `$S/r8-prepare/runtime/session.json` then has
   `complete`, `rom_usable` and `dram_restored` true, and a `result` in
   which every core reports 32 rounds, background progress, received SGIs and
   no failure, CPU0 reports at least 32 ticks, and seven secondaries are OFF.

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
9 on this board.
