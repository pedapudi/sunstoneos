# Rank 8 bisection: the returning workload with a CPU0 return deadline

Status: profile 1 completed normally once on port 3-3, in a fresh lifetime
on 2026-10-25 (`board-session-20261025-1/port-3-3/tw-deadline-prepare`);
repeats in that lifetime follow. Its deadline return path has not run on a
board. The design,
the reference comparison and the candidate causes are in
[docs/designs/workload-bisect.md](../designs/workload-bisect.md).

The full returning workload did not return on port 3-3 on 2026-10-25, while
the same bytes returned on port 3-1 the day before. Its failure paths park
CPU0, so that run recorded nothing. The two profiles here run the same
workload and return CPU0 to the ROM with a record at the startup deadline
plus 0.5 s, or at any earlier CPU0 stop:

| Order | Profile | Bundle | Purpose |
| --- | --- | --- | --- |
| 1 | `all-core-task-workload-deadline-v1` | `$B/bundle-deadline/` | The unchanged workload, observable when it fails |
| 2 | `all-core-task-workload-quiet-deadline-v1` | `$B/bundle-quiet/` | The same with background tasks waiting in `WFE` between increments: much less sustained load |

Run profile 1 first. The failing run came after about twelve returning
images in one lifetime, so repeat profile 1 in the same lifetime, with a
91-word capture after each run: a failure that appears only as the lifetime
grows points at state left by earlier images, and an isolated failure at a
race. Both boards take power and data from one USB-C cable, so the supply
is not a difference between them.

The deadline return path, a CPU0 warm reset through RVBAR and RMR while
seven coherent secondaries keep running, has never run on a board. After
CPU0 leaves, the secondaries take no deadline exit of their own, and in
profile 1 their background loops keep running at full load. A run without
a return is therefore inconclusive: CPU0 could not take the deadline
interrupt, or the reset and return themselves hung. The follow-up for that
outcome is a forced-stall proof variant (profile 1 with one secondary
stopped before its completion record, expected to return with reason 2),
described in the design.

Profile 2 lowers the sustained load but does not isolate it: removing the
background loop's `SVC #0x6d` also changes interrupt timing and lock
traffic on all eight cores, and drops the check that a background task
stayed on its core. A completed profile 2 run is consistent with the tick
and WFE candidates as well as with the load.

## Outcomes of one launch

- **Completed.** The runner validates the workload as before, restores DRAM
  and closes the claim. The board stays usable.
- **Deadline return.** The runner prints `CPU0 returned through the deadline
  unit after …`, saves `report.bin` and `owned-writable-after.bin`, writes
  nothing to the board, keeps the claim and quarantines the instance.
  `session.json` holds the decoded record under `deadline_return`. The
  secondaries were not stopped, and the return leaves EL3 translation
  controls, VBAR_EL3 and CPU0's PPI 29 configuration changed (listed in the
  design), so remove every power source from port 3-3 before its next run. Reason 2 with `x0` 3 is the IRQ handler's deadline
  exit and `x1` its interrupt (30 for the tick, 0 for an SGI); reason 1 means
  CPU0 took no interrupt after the startup deadline, and `elr_el3`,
  `elr_el1` and `spsr_el3` locate it.
- **No return within the runner's 10 s.** Inconclusive: CPU0 could not take
  an EL3 interrupt (a hard stop, a stalled access or an EL3 park outside the
  unit), or the unproven warm reset and return hung. Remove power and do
  not retry.

## Names

Set the variables of [the README](README.md#names-used-in-every-runbook)
with `PORT=3-3`, then:

```sh
B=~/.cache/sunstoneos/evidence/workload-bisect-20261025
S=<the session directory of the new lifetime>
```

`$B/bundle-*/` holds `build/` (overlay and manifest), `host/software.json`
and `template/` (the admission records and prepared bundle of the passing
first-task attempt). `$B/source/` is the tree of the commit that built them;
its runner and host modules validate the bundle. `$B/prepare-workload-bisect.py`
is the coordinator's `cpu-prepare.py` with that source tree, the bisection
host module, ports 3-1 and 3-3, and the deadline-return outcome in its
admission text.

## Commands

1. After the power removal: cold DDR, the cache-off helper and a survey, as
   for rank 7 in this port's runbooks, then the 91-word capture. Its
   secondary-power words must equal the clean capture.
2. Write the root review record from `$B/bundle-deadline/root-review-draft.json`.
3. Bind the bundle to the live instance (offline):

   ```sh
   $PY -B $B/prepare-workload-bisect.py --bundle $B/bundle-deadline --port 3-3 \
     --survey $S/tw-survey/session.json --root-review $S/tw-root-review.json \
     --continuity <every record of this lifetime from the DDR start, in order> \
     --output $S/tw-deadline-prepare
   ```

4. Run once:

   ```sh
   $PY -B $V/common/exec-command.py $S/tw-deadline-prepare/command.json
   ```

5. For profile 2, repeat steps 1 to 4 in the next lifetime with
   `$B/bundle-quiet` and the output `$S/tw-quiet-prepare`.

## Reading a deadline return

The owned writable interval starts at `0x41200000` in
`owned-writable-after.bin`. Offsets into that file:

| Offset | Content |
| --- | --- |
| `0x1080 + 128·n` | Core n's results: rounds, background progress, SGIs, failure, ticks (CPU0 only) |
| `0x43000 + 64·n` | Core n's completion record (first word the epoch once published) |
| `0x43400 + 8·n` | Core n's arrival word (the epoch once arrived) |
| `0x883c8 + 8·n` | `pxCurrentTCBs[n]` |

The CPU0 tick timer state, every core's pending and active SGI/PPI words and
the secondaries' power status are in the record itself.
