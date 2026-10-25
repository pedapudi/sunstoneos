# Rank 7 follow-up: all-eight-core pre-scheduler in a clean lifetime

Status: **offline preparation complete**; coordinator review and a fresh
port-3-1 lifetime are required before launch.
Package: `~/.cache/sunstoneos/evidence/cpu-diagnosis-20261024/`.
Bundle: `pre-scheduler/` beneath that package.
Port: **3-1**, after its power removal and a new cold DDR initialization.
The machine-readable proposal is `ready-proposal.json`; no board or queue was
accessed while preparing it.

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
was the first launch to request a secondary power-up.
[The diagnosis](rank-7-diagnosis.md#the-corrected-diagnostic-did-not-return-in-a-clean-lifetime)
gives the reasoning.

## Frozen inputs and reused evidence

The source worktree is fixed at
`19f74cc08a66ef95aa365f61249fcf8ee21b975b` in the package's `source/`
directory. The preparation helper and diagnostic wrapper use this tree.
The prior package remains preserved under
`board-bundles-20261023/rank-7-pre-scheduler/`.

The prior package's loader refused its software evidence against current
sources: `checks/a733-smp-cached.py` had changed since the evidence was made.
The new package executes the identity-prefix cases and pre-scheduler cases
against the frozen sources. All three pre-scheduler reports have the same
hashes as the accepted reports. The seven host cases are reused because their
source hashes and report inputs are unchanged. The refreshed software record
passes the existing validator.

| Package path | Content |
| --- | --- |
| `pre-scheduler/build/` | Existing overlay and payloads; byte-identical to the 2026-10-18 pre-scheduler build |
| `pre-scheduler/identity-check-current/` | Fresh primary-preparation snapshot and three passing identity cases |
| `pre-scheduler/instruction-current/` | Passing success, counter-corruption and failed-EL3-restoration cases |
| `pre-scheduler/host-current/software.json` | Reused host cases bound to the refreshed instruction evidence |
| `pre-scheduler/template/` | Historical prepared ELF and admission inputs; these do not authorize a current launch |
| `snapshot/manifest.json` | Existing evidence-snapshot inventory of 110 selected source, artifact and tool files |
| `verify-inputs.py`, `offline-validation.json` | Offline hash, loader and historical-byte comparison; no board access |
| `common/bundle-run.py`, `wrapper-retention.json` | Wrapper with child retention after observation expiry; passing modeled timeout case |
| `pre-scheduler/root-review-draft.json` | Items for the coordinator's review |

The native ELF SHA-256 is given above. Other executable identities are:

| Input | SHA-256 |
| --- | --- |
| Overlay | `fc9116647829f58327b3140a3ef2ef6ab186f067472c3f668d62145401661d93` |
| Build manifest | `fb66a00116ef28ee63f0de6ab87f1d7e44a4625db394aacd49f9746e6a457ce5` |
| Host evidence | `445fb4cf5dc8a89bccda0c52b41d7d04ebb5abae575e82439581dca78d5e2a40` |
| Instruction evidence | `9147ce45546d69257e0bd42126581b07d04beb9901d49a6aeb0957921d85b935` |
| Wrapper | `3b1d9ce65bac32799165594d734fa818b0360525e474c8794905d8749f1ea51e` |

The verifier checks the snapshot copies and their original paths, including
the external `usb_guard.py` and FEL executable. It also validates the
historical template, overlay patch locations and current software evidence.
A changed input is a preparation failure: resolve it offline before launch.
The coordinator must separately validate current identity, permissions and
continuity; the package has no live board binding.

The overlay has no pre-release admission: a refused check parks, as on
2026-10-18. The 32-bit state capture in step 1 covers the inherited words that
those checks read, except the secondaries' SGI and PPI enable, pending and
active words.

## Names used below

```sh
PORT=3-1
PY=~/.cache/sunstoneos/venv/bin/python
V=~/.cache/sunstoneos/evidence/board-bundles-20261023
F=~/.cache/sunstoneos/evidence/cpu-diagnosis-20261024
W=~/.cache/sunstoneos/evidence/board-session-20261023
S=$W/port-$PORT
FEL=~/.cache/sunstoneos/build/sunxi-a733/sunxi-fel
R=/home/sunil/git/sunstoneos-worktrees/bringup
SB=~/.cache/sunstoneos/evidence/bootstrap-retention-20261024/survey-build
SW=~/.cache/sunstoneos/evidence/launch-survey-20261017/software.json
export PYTHONDONTWRITEBYTECODE=1
```

First run the offline verifier:

```sh
$PY -B $F/verify-inputs.py
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
   $PY -B $F/common/bundle-run.py $PORT $S/ps-cache-off 120 \
     "Board $PORT: returning instruction-cache disable before the pre-scheduler diagnostic" \
     "The helper turns the instruction cache off and the 40 SRAM bytes at 0x73000 are restored." \
     -- $PY -B $V/common/cache-off.py $S/ps-cache-off
   $PY -B $V/common/bundle-run.py $PORT $S/ps-survey 240 \
     "Board $PORT: returning SRAM survey before the pre-scheduler diagnostic" \
     "Nonce-bound launcher controls, restored SRAM and a responsive same ROM." \
     --prerequisite launch-survey-software=$SW \
     --artifact $SB/manifest.json --artifact $SB/launch-survey.bin \
     -- $PY -B $R/tools/run-launch-survey.py run --fel $FEL --build $SB \
        --software $SW --database {database} --evidence $S/ps-survey \
        --claim {claim} --owner {owner}
   ```

3. Check each item of `$F/pre-scheduler/root-review-draft.json`, then
   write the review record:

   ```sh
   $PY -c 'import json,sys; r=json.load(open(sys.argv[1])); r["status"]="Coordinator reviewed "+sys.argv[3]; open(sys.argv[2],"x").write(json.dumps(r,indent=2)+"\n")' \
     $F/pre-scheduler/root-review-draft.json $S/ps-root-review.json "the rank 7 pre-scheduler bundle for port $PORT."
   ```

4. Bind the bundle to the current instance (offline):

   ```sh
   $PY -B $F/pre-scheduler/cpu-prepare.py --bundle $F/pre-scheduler \
     --port $PORT --survey $S/ps-survey/session.json --root-review $S/ps-root-review.json \
     --continuity $S/ps-ddr/session.json --continuity $S/ps-survey-after-ddr/session.json \
     --continuity $S/ps-state/session.json --continuity $S/ps-cache-off/session.json \
     --output $S/ps-prepare
   ```

5. Verify the frozen inputs again, then run the diagnostic once:

   ```sh
   $PY -B $F/verify-inputs.py
   $PY -B $F/common/exec-command.py $S/ps-prepare/command.json
   ```

## Observation and recovery

The runner bounds each FEL transaction to 15 seconds and waits up to 10 seconds
for a new ROM instance after launch. On unresolved failure it records the
failure, quarantines the instance and holds the port lock without further
protocol requests. The wrapper's 300-second budget is an observation limit;
it records expiry and continues waiting for the existing child.

The wrapper writes its PID and the runner PID to
`$S/ps-prepare/runtime-setup/runner.json` and streams output to the adjacent
`runner.log`. Keep the launch tool's session handle. Inspect those files and
`$S/ps-prepare/runtime/session.json` without communicating with the board.
Do not terminate either process to free ownership after observation expiry.
The coordinator closes the retained process and claim only after physical
recovery and review of the exact recorded process.

## Outcomes

- **Success.** The runner exits 0 and the wrapper reports `passed`.
  `runtime/session.json` has `complete`, `rom_usable` and `dram_restored`
  true. Its report establishes eight participants, 256 atomic increments,
  cleaned owned lines, restored controls and seven OFF secondaries.
  Review that evidence before proposing a first-task launch; this procedure
  stops after the pre-scheduler result.
- **No return.** The stop location remains unknown: entry checks, secondary
  startup, the barrier, cleanup and return have no host-visible checkpoint.
  Keep ownership, record the failure and request removal of every power
  source. Do not retry or run this diagnostic on port 3-3.
- **Other failure.** Inspect whether the runner returned and restored DRAM
  or retained the instance. Stop this procedure and retain the claim pending
  coordinator review. A nonzero exit alone does not establish safe recovery.
