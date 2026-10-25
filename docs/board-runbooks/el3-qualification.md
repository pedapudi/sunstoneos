# Qualifying the EL3 security setup on port 3-3

Status: prepared offline; nothing here has run. It runs after
`work/gpu-vendor-default-1025` merges, in a fresh port 3-3 lifetime.

Every SunstoneOS EL3 entry now makes the vendor BL31's cold-boot security
writes 9 to 17 before EL1 starts
([`include/a733_el3_security.h`](../../include/a733_el3_security.h),
[design](../designs/gpu-vendor-default.md)). They have run on a board once,
in the vendor-state GPU image on port 3-1 (2026-10-25), without write 11
(S_TZMA `0x07003000`, `+0x4`, `+0x8` = 0) and only through the single-core
monitor. This lifetime runs them through the single-core monitor with
write 11 and then through the SMP cached image's own setup function, in
the six runs of the refreeze sequence, whose native image is now
`6fd7af86…` ([the second refreeze](../designs/native-refreeze-1025.md#the-second-refreeze-adds-the-el3-security-writes)).

No returning image enters EL1 through `a733_smp_boot` itself: the returning
diagnostics start the secondaries from their own overlay, and the native
supervised image does not return. The pre-scheduler overlay therefore calls
the native `a733_el3_security_setup` at its native place, after all seven
secondaries are released and before CPU0 enters EL1. The call site in
`a733_smp_boot` stays unrun on a board.

## Order and predictions

| Order | Run | Bundle | Prediction |
| --- | --- | --- | --- |
| 1 | Kernel check | `$K/kernel-check-counter-option/` | The monitor makes writes 9 to 17 (both DMA words skipped: both controllers are gated and in reset after a FEL boot), the 100 rounds pass, the monitor returns, and the ROM answers |
| 2 | Security switches | `$K/el3-security-switches/` | `0x02003f00` reads 7 and `0x07010290` reads 7: both were 0 after the power removal |
| 3 | All-core pre-scheduler | `$N/bundles-22501205/rank-7-pre-scheduler/` | As in the first refreeze qualification: the run returns with controls and saved DRAM restored. The setup writes the same values again |
| 4 | All-core first task | `$N/bundles-22501205/rank-7-corrected/` | Returns as in the first qualification |
| 5 | All-core task switch | `$N/bundles-22501205/cpu-next-task-switch/` | Returns as in the first qualification |
| 6–8 | Deadline workload, three times | `$N/bundles-22501205/bundle-deadline/` | Each returns as in the first qualification |

Every 91-word capture of the lifetime equals the clean capture: the
capture reads CPU power, CPU configuration and GIC-600 words, none of which
the setup writes.

What a departure would mean:

- **Run 1 does not return, or the ROM does not answer after it.** A write
  made a block that the Secure-state ROM or the monitor's return path uses
  inaccessible. Write 11 is the only write that run 1 adds to the board
  evidence; it marks SRAM areas non-secure, which a Secure-state master can
  still access, so this is not expected. Remove every power source and stop.
- **Run 2 reads 0 in either word.** The monitor did not make the writes, or
  the ROM return reset them. The kernel check's log does not show which.
- **Run 3 or later fails where the first qualification passed.** The setup
  changed the multicore path: the run's record names the step, and the
  native image differs from `a8228eaa…` only by the setup call.

## Names

Set the variables of [the README](README.md#names-used-in-every-runbook)
with `PORT=3-3`, then:

```sh
K=~/.cache/sunstoneos/evidence/board-bundles-20261025-9/port-3-3
N=~/.cache/sunstoneos/evidence/refreeze-el3-20261025
S=~/.cache/sunstoneos/evidence/board-session-20261025-7/port-3-3
```

`$K/runbook.md` is the prepared runbook of runs 1 and its lifetime start;
`$K/kernel-check-counter-option/runbook.md` and
`$K/el3-security-switches/runbook.md` give the commands of runs 1 and 2.
Every bundle under `$N/bundles-22501205/` holds `build/`,
`host/software.json`, `template/` (the admission records and prepared
bundle of `6fd7af86…`), `root-review-draft.json` and `cpu-prepare.py`,
which loads the runner and host modules from `$N/source-22501205/`.

## Commands

1. The start of `$K/runbook.md`: the power removal and the cold DDR start
   `$S/s7-ddr`.
2. The 91-word capture `$S/state-0`, as for rank 7 in this port's runbooks.
3. Run 1, the commands of `$K/kernel-check-counter-option/runbook.md`:
   `$S/q1-kc-cache-off`, `$S/q1-kc-survey`, then `$S/q1-kc-run` with
   `--ddr-evidence $S/s7-ddr/session.json` and no `--warm-return`.
4. Run 2, the command of `$K/el3-security-switches/runbook.md`:
   `$S/bb18-read`. Record both words.
5. The 91-word capture `$S/state-1`; compare it with `$S/state-0`.
6. Runs 3 to 8 follow steps 2.1 to 2.5 of the
   [refreeze qualification](refreeze-qualification.md#commands), with
   `$N/bundles-22501205/` in place of `$N/bundles-6cd9d008/` and the record
   names `r3` to `r8`. The continuity list starts with every record of
   steps 1 to 5 in order: `$S/s7-ddr`, `$S/state-0`, `$S/q1-kc-cache-off`,
   `$S/q1-kc-survey`, `$S/q1-kc-run`, `$S/bb18-read`, `$S/state-1`, then
   `$S/r3-cache-off` and `$S/r3-survey`.

The outcomes of each launch are those of the
[refreeze qualification](refreeze-qualification.md#outcomes-of-one-launch)
and of each prepared runbook. Stop at the first run that does not
complete, and do not retry in this lifetime.

The bundles were bound offline against the first survey of the
`board-session-20261025-5` port 3-3 lifetime
(`$N/offline-binding-22501205/`, never deployed); a binding for this
lifetime replaces those records.
