# Runbooks for ranks 6 to 9 of the board test plan

These runbooks list every command the coordinator runs, on each board, for
ranks 6 to 9 of the [ranked board test plan](../audit/board-test-plan.md):

1. [Rank 6: returning thermal application](rank-6-thermal.md)
2. [Rank 7: all-core first-task diagnostic](rank-7-first-task.md)
3. [Rank 8: full returning eight-core workload](rank-8-workload.md)
4. [Rank 9: native USB device channel](rank-9-usb.md)

They start from the state that ranks 1 to 5 left on 2026-10-23. Both boards
trained 12,288 MiB of DDR, passed the single-core kernel check and passed the
forced-stall case. Each board now waits in the boot ROM's USB recovery mode
(FEL) on the instance that the forced-stall return created:

| Port | DDR instance | After the kernel check | After the forced stall (current) |
| --- | --- | --- | --- |
| 3-1 | `003/002` | `003/010` | `003/012`, identity `[7,1502,1096971,0,0]` |
| 3-3 | `003/004` | `003/009` | `003/011`, identity `[7,1498,1096970,0,0]` |

Nothing in these runbooks has run. The frozen bundles are under
`~/.cache/sunstoneos/evidence/board-bundles-20261023/`, one directory per
rank plus `common/`. Rank 9 uses `rank-9-usb-v2/`, whose input builder
accepts the fabric rates that the image accepts; `rank-9-usb/` is kept
unchanged and is not used. Each command runs once. A command that exits nonzero
stops that board's sequence; the other board continues independently.

## Names used in every runbook

The commands use these shell variables. Set `PORT` to `3-1` or `3-3`.

```sh
PORT=3-1
PY=~/.cache/sunstoneos/venv/bin/python
FEL=~/.cache/sunstoneos/build/sunxi-a733/sunxi-fel
V=~/.cache/sunstoneos/evidence/board-bundles-20261023
S=~/.cache/sunstoneos/evidence/board-session-20261023/port-$PORT
export PYTHONDONTWRITEBYTECODE=1
```

`$S` already holds the records of ranks 1 to 5: `ddr/`, `kernel/`,
`timeout/` and the surveys. New runs write new directories under `$S` whose
names start with the rank (`r6-`, `r7-`, `r8-`, `r9-`); every runner refuses
an existing evidence directory.

## Shared coordinator scripts

`$V/common/` holds the scripts that every runbook calls:

| Script | SHA-256 | Purpose |
| --- | --- | --- |
| `bundle-run.py` | `7a369d60…` | Writes a recovery record for the port's current USB instance, then submits, reviews and claims the queue entry, runs the runner once with `SUNSTONE_USB_PORT` set, and finishes the claim on success or keeps it on failure. It generalizes the coordinator's `queued-run.py` with extra prerequisites, extra artifacts and the `{claim}`, `{owner}` and `{database}` placeholders that claim-checking runners need. |
| `cache-off.py` | `0330c1da…` | Unchanged copy of the coordinator's helper: turns the instruction cache off after a warm ROM return. |
| `survey.py` | `9303d4de…` | Unchanged copy of the coordinator's rank 2 script: one returning SRAM survey of the current instance. |
| `exec-command.py` | `f50ab626…` | Prints and runs the argument list in a `command.json` that a preparation script wrote. |
| `primary.patch` | `eba24ff3…` | The runner change described below. |

`$V/manifest.json` records the full SHA-256 of every bundle file. The copies
of `cache-off.py` and `survey.py` are byte-identical to the coordinator's
scripts in `board-session-20261023/`.

After a warm ROM return, a survey fails its `mmu_caches_endian_off` gate
until the cache-off helper runs; `all-core-first-task-20261018/warm-010/`
records that pair of surveys. Each rank therefore begins with the same two
steps on the current instance:

```sh
$PY -B $V/common/bundle-run.py $PORT $S/rN-cache-off 120 \
  "Board $PORT: returning instruction-cache disable before rank N" \
  "The helper turns the instruction cache off and the 40 SRAM bytes at 0x73000 are restored." \
  -- $PY -B $V/common/cache-off.py $S/rN-cache-off
$PY -B $V/common/survey.py $PORT $S/rN-survey "Board $PORT before rank N"
```

Success prints `{"port": "PORT", "complete": true}` and then
`"result": "passed"` for the cache-off helper, and `"result": "passed"` for
the survey. Neither changes the USB instance. The cache-off helper and the
survey each write their own recovery record for the current instance; a
record written for an earlier instance is refused, as happened to the first
warm survey on port 3-3.

## Admission after the forced-stall return

Two runner families serve ranks 6 to 9:

- `tools/check-a733-kernel.py` runs rank 6. `tools/check-a733-usb-returning.py`
  runs rank 9 inside the same kernel runner. With `--ddr-evidence` the
  runner requires a chain of `--warm-return` records from the DDR session's
  USB instance to the current one, and it accepts only kernel-mode returns
  as links. The forced-stall session is a timeout-mode
  return, so the runner refuses the chain DDR → kernel check → forced
  stall. It prints `Warm-return evidence does not establish contiguous
  successful ROM return.` Omitting the forced-stall link is also refused,
  because the kernel check's returned instance (`003/010` or `003/009`) is
  no longer the live one.
- `tools/run-native-startup-probe.py` runs ranks 7 and 8. It checks no
  warm-return chain. It requires a survey and a recovery record of the live
  instance, a prepared admission bound to that instance whose DDR and
  platform records cite reviewed continuity evidence, and a queue claim with
  the prerequisites `supervised-ram-recovery` and `native-startup-software`.
  A forced-stall return needs no extra record for it.

A fresh kernel-mode run does not repair the chain: the kernel runner's own
admission would need the same chain, and any later link must start from
the forced-stall return's instance. The records that would repair it are a
fresh DDR initialization or a runner that accepts the forced-stall link.
After ranks 7 and 8 the chain also passes through two native-runner
records, which the current kernel runner does not accept either.

`$V/common/primary.patch` changes `validate_platform_admission` in
`tools/check-a733-kernel.py` to accept two more link kinds. Each must still
meet every existing continuity condition: the same port, an initial instance
equal to the previous returned instance, and a new returned instance. The
two kinds are:

- a forced-stall return of the kernel runner: mode `timeout`, result magic
  `0x534b4552`, stage 5, status 2, interrupt 29, no transport failure,
  automatic access and the pinned FEL hash;
- a session of `tools/run-native-startup-probe.py`: no mode field,
  `physical_access`, `rom_usable` and `dram_restored` all true.

Both return through the monitor's warm reset with DDR powered and trained,
as a kernel-mode return does. The patch also adds 13 cases to
`checks/kernel-platform-admission.py`. With port 3-3's saved records, all 50
cases pass (`common/kernel-admission-check/results.json`), including the
actual saved DDR → kernel → forced-stall chain and its five refusals and a
modeled native link and its six refusals. The rank 6 and rank 9 bundles
freeze the patched runner in their `host/tools/`. The coordinator must
review and accept the patch before using either bundle's runner.

### Cheapest valid order

With the patch, the plan's order 6, 7, 8, 9 needs no extra board operation
beyond the cache-off helper and survey that each rank needs anyway. Rank 9
stays last because its failure leaves the board unreachable until power is
removed.

Without the patch, ranks 6 and 9 have two options:

- **Fresh DDR initialization** with `tools/run-ddr.py` before rank 6, and
  again before rank 9. The DDR session then starts a new chain whose only
  link is the thermal run's kernel-mode return, or none for rank 9. Each
  repeat retrains a board whose DDR is already trained, which has not been
  demonstrated, and carries rank 3's risk of an unbounded hang inside the
  vendor routine. Earlier handoffs direct that DDR continuity be preserved
  rather than retrained
  ([handoff history](../handoff-history-20261019.md)).
- **The runner's signature path.** Without `--ddr-evidence`, the kernel
  runner reads 128 bytes at `0x42000000` and admits the run if they start
  with a probe signature. The forced-stall result left `0x534b4552` there
  for rank 6; the rank 6 result leaves the same signature, and ranks 7 and
  8 restore that page, so the runner would also admit rank 9. The run
  record then carries no DDR admission.

The patch is the cheapest option that keeps recorded DDR admission.

## The boot-clock report

Rank 6 names a boot-clock report. It refers to `a733_cpu_clock_report` in
[the CPU clock driver](../cpu-clock.md): eight read-only words (the
oscillator word `0x07090160`, the speed-bin word `0x0300624c`, and the PLL
and mux words of CPU-L, CPU-B and the DSU at `0x08871000`, `0x0887101c`,
`0x08872000`, `0x0887201c`, `0x08873000` and `0x0887301c`), decoded to the
configured rates. No runner exists for it: no native image calls the
function except the memory qualification image, which has no runner, and
the mimus clock report is compiled out of native images. Rank 6 therefore
runs the thermal application only. The memory qualification rows, which
require "rank 6 reporting the boot clock", stay blocked on that report.
Either a reviewed read-only FEL capture of the eight words or a call from
the thermal image would provide it; both need their own review.
