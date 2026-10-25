# Rank 7: corrected all-core first-task diagnostic

Status: **ready** once the branch that adds the pre-release admission is merged
into `bringup` and the coordinator has reviewed `root-review-draft.json`.
Bundle: `~/.cache/sunstoneos/evidence/board-bundles-20261023/rank-7-corrected/`.
It replaces `rank-7-first-task/`, which is not run again.

The diagnostic runs the frozen eight-core supervised ELF
`762237cd0f023a2c6895167b347352bb383e1012b5c39d1f8f0139278a68dba3` with a
returning overlay. Every core constructs its tasks, starts the scheduler and
restores its first FreeRTOS task context; an SVC at the first idle-task
instruction records the context. No task body, timer interrupt or SGI
workload runs. Every core then cleans the 145-page owned writable interval,
CPU0 restores its controls, the seven secondaries reach OFF and CPU0 returns
to the ROM.

Before it releases any secondary, the overlay reads the inherited state. It
reads the startup contract and the zeroed report with 64-bit loads and
`CNTFRQ_EL0` as a system register. It reads the GIC-600 distributor, all
eight redistributor frames and the seven secondaries' power words with 32-bit
loads, except `GICR_TYPER`, which it reads with 64-bit loads. On the first word that a later check would refuse,
it writes a failure code, the word's address and the value read into the
report, and returns to the ROM. [The diagnosis](rank-7-diagnosis.md#the-corrected-diagnostic-checks-the-inherited-state-before-releasing-any-secondary)
lists the codes and the checks that still end without a return.

## Bundle contents

| Path | Content |
| --- | --- |
| `build/` | Built by `tools/prepare-secondary-identity-probe.py --first-task`; manifest SHA-256 `110f5ecf…` |
| `build-identity/`, `identity-check/` | Identity-profile build and the primary-preparation snapshot that the instruction checks resume from; its own three cases pass |
| `first-task-check/` | Five instruction cases, all passing: success (eight first-task entries, 9,280 cleaned lines per core, matched return, report byte-identical to 2026-10-18), wrong task argument and failed EL3 restoration (cores retained), every GIC word 0 (returns with code `0x10`) and CPU0 PPI 29 pending (returns with code `0x34`) |
| `host/` | Ten host cases of `checks/all-core-first-task-host.py`, all passing; `host/software.json` SHA-256 `24d5a961…` |
| `template/` | Unchanged admission records and prepared bundle of the 2026-10-18 pass |
| `overlay-comparison.json` | Native payload segments identical to the 2026-10-18 build; the overlay differs only in its identity extension |
| `cpu-prepare.py` | Binds the bundle to the current USB instance (offline) |
| `root-review-draft.json` | Items for the coordinator's review |

An offline bind against the port 3-3 clean-lifetime survey, with this
branch's loaders, validated the bundle, the admission and the software
evidence.

## Names used below

```sh
PORT=3-1
PY=~/.cache/sunstoneos/venv/bin/python
V=~/.cache/sunstoneos/evidence/board-bundles-20261023
W=~/.cache/sunstoneos/evidence/board-session-20261023
S=$W/port-$PORT
FEL=~/.cache/sunstoneos/build/sunxi-a733/sunxi-fel
SR=/home/sunil/git/sunstoneos-worktrees/bringup
SB=~/.cache/sunstoneos/evidence/bootstrap-retention-20261024/survey-build
SW=~/.cache/sunstoneos/evidence/launch-survey-20261017/software.json
export PYTHONDONTWRITEBYTECODE=1
```

`$S` already holds the records of the board's current clean lifetime: the
reconnect survey, `ddr-2/` and `survey-after-ddr-2/` (and on port 3-3 also
`state-capture-clean/` and `gic-clock-capture/`). Each runner refuses an
existing evidence directory.

## Commands

Run on port 3-1 first. Run on port 3-3 only after port 3-1 returned with
success.

1. Capture the inherited state read-only, with 32-bit loads:

   ```sh
   $PY -B $W/queued-run.py $PORT $S/r7c-state 300 \
     "Board $PORT: read-only CPU power, CPU configuration and GIC-600 state with 32-bit loads" \
     "Every listed word reads back on the current ROM instance and the FEL scratch bytes are restored; no device register is written." \
     -- $PY -B $W/state-capture-readl.py $S/r7c-state
   ```

   Expected: `complete` and
   `gic_read` true, `scratch_restored` true, and every word equal to the port
   3-3 clean values in the diagnosis: `GICD_CTLR` `0x37`, `GICD_TYPER`
   `0x007b0408`, CPU0 redistributor words 0, secondary frame n `GICR_TYPER`
   low word `0x21 | (n << 8)` (`0x31 | (7 << 8)` on frame 7), `GICR_PWRR`
   `n << 8`, `GICR_CTLR` 0, `GICR_WAKER` 6, power status `0x100`, control,
   mode and entry words 0. A different word: stop and report it; do not
   launch.

2. Turn the instruction cache off and survey:

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/r7c-cache-off 120 \
     "Board $PORT: returning instruction-cache disable before the corrected first-task diagnostic" \
     "The helper turns the instruction cache off and the 40 SRAM bytes at 0x73000 are restored." \
     -- $PY -B $V/common/cache-off.py $S/r7c-cache-off
   $PY -B $V/common/bundle-run.py $PORT $S/r7c-survey 240 \
     "Board $PORT: returning SRAM survey before the corrected first-task diagnostic" \
     "Nonce-bound launcher controls, restored SRAM and a responsive same ROM." \
     --prerequisite launch-survey-software=$SW \
     --artifact $SB/manifest.json --artifact $SB/launch-survey.bin \
     -- $PY -B $SR/tools/run-launch-survey.py run --fel $FEL --build $SB \
        --software $SW --database {database} --evidence $S/r7c-survey \
        --claim {claim} --owner {owner}
   ```

3. Check each item of `$V/rank-7-corrected/root-review-draft.json`, then write
   the review record. The preparation script refuses a record whose `status`
   does not begin with `Coordinator reviewed`:

   ```sh
   $PY -c 'import json,sys; r=json.load(open(sys.argv[1])); r["status"]="Coordinator reviewed "+sys.argv[3]; open(sys.argv[2],"x").write(json.dumps(r,indent=2)+"\n")' \
     $V/rank-7-corrected/root-review-draft.json $S/r7c-root-review.json "the corrected rank 7 bundle for port $PORT."
   ```

4. Bind the bundle to the current instance (offline). List every record of
   this powered lifetime from the cold DDR initialization onwards, in order:

   ```sh
   $PY -B $V/rank-7-corrected/cpu-prepare.py --bundle $V/rank-7-corrected \
     --port $PORT --survey $S/r7c-survey/session.json --root-review $S/r7c-root-review.json \
     --continuity $S/ddr-2/session.json --continuity $S/survey-after-ddr-2/session.json \
     --continuity $S/r7c-state/session.json --continuity $S/r7c-cache-off/session.json \
     --output $S/r7c-prepare
   ```

   On port 3-3, add `state-capture-clean` and `gic-clock-capture` after
   `survey-after-ddr-2`. The script checks the build, the host evidence and
   the frozen image with the runner's own loaders, refuses unless the
   survey's launcher gates all pass, and writes the admission records,
   `prepared/` (with a fresh reservation, epoch, generation and lease) and
   `command.json`.

5. Run the diagnostic once:

   ```sh
   $PY -B $V/common/exec-command.py $S/r7c-prepare/command.json
   ```

## Outcomes

- **Success.** The runner exits 0 and `bundle-run.py` prints
  `"result": "passed"`. `$S/r7c-prepare/runtime/session.json` has `complete`,
  `rom_usable` and `dram_restored` true and a `result` with eight first-task
  entries, 9,280 cleaned lines per core and seven OFF secondaries. On port
  3-1, continue with the history test in the diagnosis: kernel check,
  forced-stall case, thermal application, steps 1 to 5 again.
- **Refusal.** The runner exits nonzero with `failure` beginning
  `Admission refused before any secondary was released: code …`, and
  `rom_usable` and `dram_restored` true. `runtime/report.bin` holds the code
  at offset `0x160`, the address at `0x168` and the value at `0x170`. The ROM
  answers on a new instance and needs no power removal. Do not retry; report
  the code. For codes `0x01` to `0x03` (zero epoch or lease, expired
  deadline, nonzero report) the host decoder can instead report `Admission
  refusal report is inconsistent.`, because its decoder requires a nonzero
  epoch and deadline and an otherwise zero report, which these refusals need
  not have. The code is then read from `runtime/report.bin` at offset
  `0x160`.
- **No return within the runner's 10 s window.** CPU0 stopped in one of the
  parking paths the diagnosis lists: in the native startup or the base
  overlay before the admission, in a stalled admission read, in
  `identity_primary_run` or `secondary_distributor_begin` after the
  admission but before release, or after release. The observation alone does
  not tell these apart. Keep the runner's lock, record the failure and
  request a power removal for this port. Do not retry, and do not run rank 8
  or port 3-3.

## Stop conditions

Any nonzero exit other than an admission refusal: stop this board and keep
the claim.
