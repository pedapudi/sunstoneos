# Rank 8 bisection: the returning workload with a CPU0 return deadline

Status: in one port 3-3 lifetime on 2026-10-25
(`board-session-20261025-1/port-3-3/`), profile 1 completed four times and
then returned through the deadline unit with reason 3. The record locates
the fault in CPU0's GIC restoration, which compared GICR_CTLR while its
read-only write-pending bit was still set. A profile that waited for that
bit (`$B/../rwp-fix/`) failed the same way on its first run in the next
lifetime (`board-session-20261025-2/port-3-3/tw1-prepare`): the bit read
set again after the wait, for a reason not yet explained. The corrected profiles in `$B` ignore GICR_CTLR's
read-only status bits in every comparison and keep the waits; they have not
run on a board. The
design, the reference comparison, the decoded record and the candidate
causes are in [docs/designs/workload-bisect.md](../designs/workload-bisect.md).

The full returning workload did not return on port 3-3 on 2026-10-25, while
the same bytes returned on port 3-1 the day before. Its failure paths park
CPU0, so that run recorded nothing. The two profiles here run the same
workload and return CPU0 to the ROM with a record at the startup deadline
plus 0.5 s, or at any earlier CPU0 stop:

| Order | Profile | Bundle | Purpose |
| --- | --- | --- | --- |
| 1 | `all-core-task-workload-deadline-v1` | `$B/bundle-deadline/` (corrected); `$B/../bundle-deadline/` and `$B/../rwp-fix/bundle-deadline/` ran on 2026-10-25 | The unchanged workload, observable when it fails |
| 2 | `all-core-task-workload-quiet-deadline-v1` | `$B/bundle-quiet/` | The same with background tasks waiting in `WFE` between increments: much less sustained load |

Next, run the corrected profile 1 repeatedly in one fresh lifetime, with a
91-word capture after each run; one failure in five was seen before the
correction. Every completed run is the expected outcome. A reason 3 return
with cleanup error 4 in CPU0's record at `0x42018a00`, or 7 in a
secondary's record at `0x42018a00 + 512·n`, would mean RWP never cleared
within the deadline, a different fault. Profile 2 is not needed for
the observed fault. The frozen rank 7 and rank 8 bundles run the
uncorrected restorations and remain exposed to the same fault; refreeze
them with the correction before running them again. Both boards take power
and data from one USB-C cable, so the supply is not a difference between
them.

The deadline return has run on a board once, with reason 3, which captures
at EL3 without the EL1 cleaning step. The FIQ entry (reason 1) and the
cleaning helper (reasons 1 and 2) have run only in the model. After CPU0
leaves, the secondaries take no deadline exit of their own, so a run
without a return is still inconclusive: CPU0 could not take the deadline
interrupt, or the return hung. A forced-stall variant (profile 1 with one
secondary stopped before its completion record, expected to return with
reason 2) would prove the cleaning path; it is not built.

Profile 2 lowers the sustained load but does not isolate it: removing the
background loop's `SVC #0x6d` also changes interrupt timing on all eight
cores, and drops the check that a background task stayed on its core. A completed profile 2 run is consistent with the tick
and WFE candidates as well as with the load.

## Outcomes of one launch

- **Completed.** The runner validates the workload as before, restores DRAM
  and closes the claim. The board stays usable.
- **Deadline return.** The runner prints `CPU0 returned through the deadline
  unit after …`, saves `report.bin` and `owned-writable-after.bin`, writes
  nothing to the board, quarantines the instance and exits nonzero; the
  queue claim stays held.
  `session.json` holds the decoded record under `deadline_return`. The
  secondaries were not stopped, and the return leaves EL3 translation
  controls, VBAR_EL3 and CPU0's PPI 29 configuration changed (listed in the
  design), so remove every power source from port 3-3 before its next run.
  - Reason 1: CPU0 took no interrupt after the startup deadline;
    `elr_el3`, `elr_el1` and `spsr_el3` locate it. Stage 3.
  - Reason 2 with `x0` 3: the IRQ handler's deadline exit, `x1` its
    interrupt (30 for the tick, 0 for an SGI). Stage 3.
  - Reason 3: `identity_primary_run` returned 0 at EL3 (`x30` is the
    gate's return address `0x4102175c`). Stage 1: no cleaning, because CPU0
    had already left EL1, so `owned-writable-after.bin` may lack lines that
    were still dirty in a secondary's cache. Locate the failing check with
    the identity header at `0x42018100` (state, completed mask, core, and
    restored SCTLR_EL1/TCR_EL1 words, which stay 0 if the CPU0 local probe
    failed), CPU0's local-probe record at `0x42018a00` (state, cleanup
    error at word 30, before and after snapshots from word 10 and word 20)
    and the secondaries' slots at `0x42018200 + 256·n` (ready, request,
    ack, off). A secondary that never set ready has its own local-probe
    record at `0x42018a00 + 512·n` (cleanup error at word 11, before and
    after snapshots from word 17 and word 27).
  - Reason 4: `el1_return_checkpoint` refused CPU0's completion SMC; its
    record at `0x4201a800` holds the refusal. Stage 1.
- **No return within the runner's 10 s.** Inconclusive: CPU0 could not take
  an EL3 interrupt (a hard stop, a stalled access or an EL3 park outside the
  unit), or the return hung. Remove power and do not retry.

## Names

Set the variables of [the README](README.md#names-used-in-every-runbook)
with `PORT=3-3`, then:

```sh
B=~/.cache/sunstoneos/evidence/workload-bisect-20261025/rwp4
S=<the session directory of the new lifetime>
```

`$B` holds the corrected bundles; the bundles that ran on 2026-10-25 are
in `$B/..`, `$B/../rwp-fix` and `$B/../rwp3` and are kept unchanged. The
overlays in `$B` equal those in `$B/../rwp3` byte for byte; only comments
and the runner's quarantine handling changed. `$B/bundle-*/` holds `build/` (overlay and manifest), `host/software.json`
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

5. Repeat steps 1 (cache-off and survey only) to 4 in the same lifetime,
   with a 91-word capture after each run and a new output directory per
   run, until the coordinator's planned count; stop at the first deadline
   return or non-return.
6. Profile 2 (`$B/bundle-quiet`) is not needed for the observed fault.

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
