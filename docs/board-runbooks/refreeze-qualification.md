# Qualifying the refrozen native image on port 3-3

Status: prepared offline; nothing here has run. The returning diagnostics
until now ran the native image frozen on 2026-10-18 (`762237cd…`). The
bundles below run the image built from `bringup` at `d502f8ea`
(`a8228eaa…`), whose EL1 entry sets VPIDR_EL2, VMPIDR_EL2 and
CNTHP_CTL_EL2, zeroes the three active-priority copies, sets the Secure
priority mask and SCTLR_EL1.nTWE and nTWI, and whose per-core controller
waits for GICD_CTLR.RWP
([design](../designs/native-refreeze-1025.md)).

One fresh lifetime runs five diagnostics in order of increasing scope. The
three earlier ones also prepare the state the next one starts from.
Prediction: every run returns normally, with controls and saved DRAM
restored, and every 91-word capture equals the capture taken after the
cold DDR start.

| Order | Diagnostic | Bundle (`$N/bundles-6cd9d008/…`) | What it adds |
| --- | --- | --- | --- |
| 1 | All-core pre-scheduler | `rank-7-pre-scheduler/` | Power-up of seven secondaries, the corrected entry on all eight cores, the production barrier and 256 atomic increments |
| 2 | All-core first task | `rank-7-corrected/` | The pre-release admission and first FreeRTOS task entry on all eight cores |
| 3 | All-core task switch | `cpu-next-task-switch/` | Native idle yields and worker selection; with the tick stopped, the idle loops yield in place of their WFI |
| 4–6 | Deadline workload, three times | `bundle-deadline/` | The full 32-round workload with ticks and SGIs; CPU0 returns with a record at any stop |

## Outcomes of one launch

- **Completed.** The runner validates the report, restores DRAM and closes
  the claim. Take the capture, then continue with the next row.
- **Refusal with a code** (first task only, before any secondary starts).
  The runner restores DRAM; the board stays usable. Stop the sequence: the
  refusal names the inherited word that differs.
- **Deadline return** (rows 4–6). The runner saves the record and the owned
  memory, restores nothing, quarantines the instance and keeps the claim.
  Decode the record as in [rank 8 bisection](rank-8-workload-bisect.md) and
  remove every power source; do not continue in this lifetime. For reason 1
  the runner's message says whether the deadline interrupt found CPU0 in
  `identity_park` (a completion barrier that timed out) or elsewhere, from
  ELR_EL3.
- **No return, or any capture that differs from the first.** Stop, remove
  every power source and do not retry.

## Names

Set the variables of [the README](README.md#names-used-in-every-runbook)
with `PORT=3-3`, then:

```sh
N=~/.cache/sunstoneos/evidence/refreeze-20261025
S=<the session directory of the new lifetime>
```

Every bundle holds `build/`, `host/software.json`, `template/` (the
admission records and prepared bundle of the refrozen image, see
`$N/template/README.txt`), `root-review-draft.json` and `cpu-prepare.py`.
The script loads the runner and host modules from `$N/source-6cd9d008/`,
the tree of `work/refreeze-1025` at `6cd9d008` that built and checked the
bundles; `$N/bundles-6cd9d008/index.json` names each bundle's profile and
the bundle it replaces. `$N/bundles/` holds the superseded first set,
kept unchanged. Write session records only under `$S`.

## Commands

1. After the power removal: cold DDR, the 91-word capture, the cache-off
   helper and a survey, as for rank 7 in this port's runbooks
   (`$S/ddr`, `$S/state-0`, `$S/r1-cache-off`, `$S/r1-survey`). The capture's
   secondary-power words must equal the clean capture.
2. For run *k* of the table (`r1` to `r6`), with `X` its bundle directory:
   1. write `$S/rk-root-review.json` from `$N/bundles-6cd9d008/X/root-review-draft.json`;
   2. bind it, listing every record of this lifetime from the DDR start in
      order (for `r1`: `$S/ddr`, `$S/state-0`, `$S/r1-cache-off`,
      `$S/r1-survey`):

      ```sh
      $PY -B $N/bundles-6cd9d008/X/cpu-prepare.py --bundle $N/bundles-6cd9d008/X --port 3-3 \
        --survey $S/rk-survey/session.json --root-review $S/rk-root-review.json \
        --continuity $S/ddr/session.json --continuity $S/state-0/session.json \
        ... --continuity $S/rk-survey/session.json --output $S/rk-prepare
      ```

   3. run the printed `bundle-run.py` command once;
   4. take the 91-word capture (`$S/rk-state-after`) and compare it with
      `$S/state-0`;
   5. unless this was the last run, run the cache-off helper and a survey
      (`$S/r(k+1)-cache-off`, `$S/r(k+1)-survey`) and append run *k*'s
      records, `rk-prepare/runtime`, `rk-state-after` and the new pair, to
      the continuity list.

The bundles were bound offline against the first survey of the
`board-session-20261025-5` port 3-3 lifetime
(`$N/offline-binding-6cd9d008/`, never deployed); a binding for the new
lifetime replaces those records.

The other bundles in `$N/bundles-6cd9d008/` refreeze the remaining frozen profiles
(rank 8, the first-task and task-workload profiles of 2026-10-24, the
quiet deadline profile and the three CPU0 profiles). They bind the same
way and are not part of this sequence.
