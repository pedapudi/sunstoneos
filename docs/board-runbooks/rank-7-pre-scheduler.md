# Rank 7 follow-up: all-eight-core pre-scheduler in a clean lifetime

Status: **ready** after the coordinator reviews `root-review-draft.json`.
Bundle: `~/.cache/sunstoneos/evidence/board-bundles-20261023/rank-7-pre-scheduler/`.
Port: **3-1**, after its power removal and a new cold DDR initialization.

The diagnostic runs the frozen eight-core supervised ELF
`762237cd0f023a2c6895167b347352bb383e1012b5c39d1f8f0139278a68dba3` with the
pre-scheduler overlay. CPU0 completes the native startup and publication
checks, then releases the seven secondaries one after another. Every core
runs the production preparation, enters cached EL1, and takes part in the
production eight-core barrier and 256 atomic increments under the production
lock. No task is created and the scheduler does not start. Each core then
cleans its 192 owned cache lines, CPU0 restores its controls, the seven
secondaries reach OFF and CPU0 returns to the ROM.

This is the diagnostic that ran immediately before the only passing
first-task run, in the same powered lifetime on 2026-10-18 (queue row 163,
`all-core-pre-scheduler-20261018/entry-corrected/physical-008/`). In both
lifetimes in which the first-task diagnostic failed, the first-task launch
was the first launch to power up a secondary.
[The diagnosis](rank-7-diagnosis.md#the-corrected-diagnostic-did-not-return-in-a-clean-lifetime)
gives the reasoning.

## Bundle contents

| Path | Content |
| --- | --- |
| `build/` | Built by `tools/prepare-secondary-identity-probe.py --pre-scheduler`; overlay and payload segments byte-identical to the 2026-10-18 build; manifest SHA-256 `fb66a001…` |
| `build-identity/`, `identity-check/` | Identity-profile build and the primary-preparation snapshot that the instruction checks resume from; its own three cases pass |
| `instruction/` | Three instruction cases, all passing: success (report byte-identical to 2026-10-18), counter corruption and failed EL3 restoration (cores retained) |
| `host/` | Seven host cases of `checks/all-core-pre-scheduler-host.py`, all passing; `host/software.json` SHA-256 `333a041a…` |
| `template/` | Admission records and prepared bundle of the 2026-10-18 pre-scheduler pass |
| `overlay-comparison.json` | Byte comparison with the 2026-10-18 build |
| `cpu-prepare.py` | Binds the bundle to the current USB instance (offline) |
| `root-review-draft.json` | Items for the coordinator's review |

`cpu-prepare.py` bound the bundle offline, with the loaders in `bringup`, to
the port 3-3 clean-lifetime survey.

The overlay has no pre-release admission: a refused check parks, as on
2026-10-18. The 32-bit state capture in step 1 covers the inherited words that
those checks read, except the secondaries' SGI and PPI enable, pending and
active words.

## Names used below

```sh
PORT=3-1
PY=~/.cache/sunstoneos/venv/bin/python
V=~/.cache/sunstoneos/evidence/board-bundles-20261023
W=~/.cache/sunstoneos/evidence/board-session-20261023
S=$W/port-$PORT
export PYTHONDONTWRITEBYTECODE=1
```

Before step 1: remove every power source from port 3-1, reconnect, run a
survey and a cold DDR initialization, and survey again. Name those record
directories `ps-survey-after-reconnect`, `ps-ddr` and `ps-survey-after-ddr`.
Each runner refuses an existing evidence directory.

## Commands

1. Capture the inherited state read-only. `readl` runs a 12-word stub in the
   FEL scratch area; the script saves and restores those bytes.

   ```sh
   $PY -B $W/queued-run.py $PORT $S/ps-state 300 \
     "Board $PORT: read-only CPU power, CPU configuration and GIC-600 state with 32-bit loads" \
     "Every listed word reads back on the current ROM instance and the FEL scratch bytes are restored; no device register is written." \
     -- $PY -B $W/state-capture-readl.py $S/ps-state
   ```

   Every word must match the clean values in
   [rank-7-corrected.md](rank-7-corrected.md#commands), step 1. A different
   word: stop and report it; do not launch.

2. Turn the instruction cache off and survey:

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/ps-cache-off 120 \
     "Board $PORT: returning instruction-cache disable before the pre-scheduler diagnostic" \
     "The helper turns the instruction cache off and the 40 SRAM bytes at 0x73000 are restored." \
     -- $PY -B $V/common/cache-off.py $S/ps-cache-off
   $PY -B $V/common/survey.py $PORT $S/ps-survey "Board $PORT before the pre-scheduler diagnostic"
   ```

3. Check each item of `$V/rank-7-pre-scheduler/root-review-draft.json`, then
   write the review record:

   ```sh
   $PY -c 'import json,sys; r=json.load(open(sys.argv[1])); r["status"]="Coordinator reviewed "+sys.argv[3]; open(sys.argv[2],"x").write(json.dumps(r,indent=2)+"\n")' \
     $V/rank-7-pre-scheduler/root-review-draft.json $S/ps-root-review.json "the rank 7 pre-scheduler bundle for port $PORT."
   ```

4. Bind the bundle to the current instance (offline):

   ```sh
   $PY -B $V/rank-7-pre-scheduler/cpu-prepare.py --bundle $V/rank-7-pre-scheduler \
     --port $PORT --survey $S/ps-survey/session.json --root-review $S/ps-root-review.json \
     --continuity $S/ps-ddr/session.json --continuity $S/ps-survey-after-ddr/session.json \
     --continuity $S/ps-state/session.json --continuity $S/ps-cache-off/session.json \
     --output $S/ps-prepare
   ```

5. Run the diagnostic once:

   ```sh
   $PY -B $V/common/exec-command.py $S/ps-prepare/command.json
   ```

## Outcomes

- **Success.** The runner exits 0 and `bundle-run.py` prints
  `"result": "passed"`. `$S/ps-prepare/runtime/session.json` has `complete`,
  `rom_usable` and `dram_restored` true and a `result` with eight
  participants, 256 atomic increments and seven OFF secondaries. Then, in the
  same lifetime, run the corrected first-task diagnostic once, with
  [rank-7-corrected.md](rank-7-corrected.md) steps 2 to 5. Pass
  `$S/ps-prepare/runtime/session.json` and the new cache-off record as
  continuity records. This reproduces the order of the 2026-10-18 pass:
  pre-scheduler, cache-off, survey, first-task.
- **No return within the runner's 10 s window.** The eight-core startup
  through the production barrier fails in a lifetime without the CPU0
  diagnostics that preceded it on 2026-10-18. Keep the runner's lock, record
  the failure, request a power removal, and do not run the first-task
  diagnostic or port 3-3.

Any other nonzero exit: stop this board and keep the claim.
