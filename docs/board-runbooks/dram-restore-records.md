# DRAM training records for the development flow

Status: **implemented and host-checked; the restore-by-SID default has not run
on a board.** The restore it runs passed on both boards on 2026-10-31 with
explicit builds and records (see [the first restore test](dram-restore-test-1.md)).

`tools/run-ddr.py` without `--build` brings up DRAM by restoring the board's
recorded training. The restore replaces boot0's 4.4 s P-state 0 training with
an initialization-only firmware run and a write of 879 trained PHY registers.
On port 3-3 the whole runner command took about 2.0 s, against about 6.6 s
for a plain full-training run on the same board and about 14.5 s for a
failed restore with its fallback (evidence file times in
`~/.cache/sunstoneos/evidence/dev-runs-1031`).

## The runner restores by SID and trains fully when it cannot

```sh
FEL=~/.cache/sunstoneos/build/sunxi-a733/sunxi-fel
SUNSTONE_USB_PORT=3-3 python3 tools/run-ddr.py --fel $FEL
```

The runner reads the board's SID chip ID (`0x03006200`–`0x0300620c`) through
FEL and looks for the record
`~/.cache/sunstoneos/ddr-records/chip-<four SID words in hex>.bin`, for
example `chip-32c05100-4c004900-58a30214-40892413.bin` for the port 3-3
board. The USB device node cannot identify a board, because its identity
changes on every enumeration. The SID is fixed per chip, and the record
holds the same four words.

| Outcome | Exit status | Printed |
| --- | --- | --- |
| Restored, quick check and both 4 KiB RAM pages pass | 0 | `PASS: DRAM restored; quick check … ms; both 4 KiB pages match.` |
| The restore ran and failed; one WDT0 reset and one full training | 3 | `RESTORE FAILED: <reason>. Training fully once.`, then `FALLBACK PASS: … DRAM is trained, not restored.` and the recapture hint |
| No record for this SID, an invalid record, no restore build, or no reconnect access for the port; one full training, no reset | 3 | `NO RESTORE: <reason>. Training fully once.`, then `FULL TRAINING PASS: … DRAM is trained, not restored.` |
| A refusal before any device access: a missing or stale build (no CPU clock return, no clocked readback, another boot0), a payload that differs from its manifest, or conflicting options | 1 | A message naming the cause; no lock is kept and no instance is quarantined |
| Any other failure | other | `Stopped: …`; the runner keeps the port lock and quarantines the instance, as before |

Exit status 3 always leaves trained DRAM, so the next image can run. Every
pass ends with `DDR evidence for the next image: DIR`. After a fallback
through WDT0 that is `EVIDENCE/fallback`, whose `session.json` describes the
new USB instance and its full training; the top-level `session.json`
describes the failed restore on the old instance. Pass the printed
directory as `--ddr-evidence` to later runners.
`session.json` in the evidence directory records `mode`
(`restore-by-sid`), `record_path`, `outcome` (`restored`,
`fallback-full-training` or `full-training-without-restore`) and, when
no restore ran, `restore_skipped`.

Options:

- `--full-training` trains fully with the plain build and never restores.
- `--records DIR`, `--restore-build DIR` and `--fallback-build DIR` replace
  the defaults `~/.cache/sunstoneos/ddr-records`,
  `~/.cache/sunstoneos/build/a733-ddr-restore` and
  `~/.cache/sunstoneos/build/a733-ddr`.
- `--build DIR` runs one build explicitly, as before: a plain build, a
  capture variant, or a restore build with `--record` and
  `--fallback-build`.

The restore needs the port's reconnect access
(`tools/usb-access.py check-session --port $PORT`), because a failed
restore recovers through a WDT0 reset and a new USB instance.

## One-time setup on the host

Build both wrappers from the current tree and rebuild them after pulling
DRAM changes. The runner refuses builds without the CPU clock return or
without the clocked readback after `init_DRAM`. The plain build at
`~/.cache/sunstoneos/build/a733-ddr` from 2026-10-15 predates the clock
return, so `prepare-ddr.py` must run once before the fallback can work:

```sh
PY=~/.cache/sunstoneos/venv/bin/python
SET=~/.cache/sunstoneos/evidence/dram-final-readback-1031/restore-set-v5.json
$PY tools/prepare-ddr.py
$PY tools/prepare-ddr.py --variant restore --restore-set $SET \
  --output ~/.cache/sunstoneos/build/a733-ddr-restore
```

The records of the two current boards come from P-state-0-only captures
taken on 2026-10-25 and are already built. Install them under their SID
names (installed on 2026-10-31):

```sh
R=~/.cache/sunstoneos/ddr-records; mkdir -p $R
F=~/.cache/sunstoneos/evidence/dram-final-readback-1031/records
cp $F/record-3-1.bin $R/chip-32c05100-4c004900-58a30214-40792413.bin
cp $F/record-3-3.bin $R/chip-32c05100-4c004900-58a30214-40892413.bin
```

## Capturing a board's record

A record holds one board's trained PHY values, the boot0 and parameter-set
identity, and the SID. It is built on the host from a register capture taken
after a P-state-0-only training. The capture writes no storage; it needs one
lifetime of the board.

The restore set (`restore-set-v5.json`) lists which registers a record holds.
It was derived from twenty captures of the two current boards
([design](../designs/dram-training-restore.md#the-restore-set-comes-from-three-kinds-of-capture)),
and the record builder accepts only those two SIDs. A third board needs its
own P-state-0-only and initialization-only captures, a new restore set, and
a new helper build and review before it can restore; until then it trains
fully.

To capture or recapture a current board, with the variables of
[the runbook README](README.md#names-used-in-every-runbook):

```sh
C=~/.cache/sunstoneos/evidence/dram-training-capture-v5-20261025/port-$PORT
S=<a new session directory>
T="<temperature note: room temperature, minutes since power-on, load or heating>"
$PY tools/prepare-ddr.py --variant pstate0 --output ~/.cache/sunstoneos/build/a733-ddr-pstate0
```

1. Start a fresh lifetime of the board. All current records come from cold
   starts at room temperature after a power removal; a lifetime after a
   WDT0 reset is a warm start, and a record captured warm has not been
   tested.
2. Train P-state 0 only:
   `$PY tools/run-ddr.py --fel $FEL --build ~/.cache/sunstoneos/build/a733-ddr-pstate0 --evidence $S/p0-ddr`.
   The log shows one `Training result is = 7`, then `DRAM Pstate 0
   training, frequency is 2040 Mhz`, `dram_tpr13 = 10061` and
   `DRAM simple test OK.`; training takes about 4.4 s.
3. Capture, in the same lifetime:
   `$PY tools/run-ddr-capture.py --fel $FEL --build $C/capture-build --ddr-evidence $S/p0-ddr --evidence $S/p0-capture-1 --temperature-note "$T"`.
   All six stages must complete; the record builder refuses a capture
   with fewer stages.
4. Build the record, with a generation one higher than the record it
   replaces (the current records have generation 1):

   ```sh
   $PY tools/training-record.py from-capture --recapture --capture $S/p0-capture-1 \
     --restore-set $SET --boot0 ~/.cache/sunstoneos/sources/board-boot/boot0_sdcard_orangepizero3w.fex \
     --generation 2 --output $S/record.bin
   ```

   It prints `record_name`, the file name the runner looks for. It refuses
   a capture whose stage tables, training kind, boot0, parameter block or
   completeness differ from the reviewed capture, or whose SID is not a
   board of the restore set.
5. Move the old record out of the records directory into
   `~/.cache/sunstoneos/ddr-records/old/`, keeping it, and copy
   `$S/record.bin` to the printed name.
6. Run `tools/run-ddr.py --fel $FEL` in a new lifetime; it must print
   `PASS: DRAM restored`.

Without `--recapture`, `from-capture` accepts only the eight
P-state-0-only reference captures of the restore set's analysis, and
requires each value to equal the frozen analysis. With it, the capture must
also be bound to its DDR run: the capture's SRAM mailbox, USB device and
node identity must equal the DDR run's, so the values come from that
P-state-0-only training.

## When a record is stale

A record is stale when the PHY values that worked at capture no longer give
working memory. That shows as a quick-check failure (mailbox error 9), a RAM
page mismatch, a restore that does not return, or memory errors later in
the boot, each followed by the fallback training. A readback failure (error
7 at the hook, 15 after `init_DRAM`) is different: a register did not hold
its written value, which points at the restore set or the helper, not at
ageing; report it and do not recapture.

- **After a failure at a temperature far from room temperature** (a board
  that has run hot, or a cold room): let the board return to room
  temperature and run once more. Recapture only if the restore fails again
  at room temperature.
- **After two fallbacks of a board at room temperature with the same
  record:** recapture it.
- **After a boot0, parameter-set or board hardware change:** the record no
  longer applies. A boot0 change also needs a new review of the hook
  addresses; the builder refuses another boot0.

The fallback leaves the board trained in every case, so a stale record
costs about 11 s per boot (one WDT0 reset and one full training) until it
is replaced.

## What limits the restore

- **No DRAM frequency switching.** The restore runs boot0's P-state 0 path:
  `dram_tpr13` bit 2 is cleared, so boot0 trains and programs only the
  2040 MHz P-state (log: `dram_tpr13 = 10061`; full training reports
  `10065` and trains 1165, 816 and 408 MHz as well). A restored boot cannot
  switch the DRAM to a lower frequency, so DRAM frequency scaling for power
  saving needs full training until lower P-states are captured and restored
  too (inferred from boot0's code; never tested).
- **Temperature range.** Every record comes from a room-temperature cold
  start, and its temperature field is "unknown". The runner has no
  temperature gate: it restores at any temperature. Two restores have
  passed, one per board on 2026-10-31; the port 3-1 one followed a WDT0
  reset, so one warm start at room temperature has worked. No restore has
  run on a cold or hot board. Training compensates for temperature and a restore does not,
  so a board far from its capture temperature can fail its quick check or
  corrupt memory that the quick check does not test.
- **Record ageing.** The records were captured on 2026-10-25 and restored
  successfully six days later. Between two P-state-0-only lifetimes of one
  board the trained values moved by a median of 2 codes and at most 13 on
  the saved VREF bytes, and some byte-lane delays by one coarse step of
  `0x40`. No data covers longer periods or aged hardware. The runner does
  not track a record's age or failures; the operator decides when to
  recapture.
- **Memory coverage.** The quick check covers 31 address lines and sixteen
  4 KiB windows in the first 3 GiB of the 12 GiB map, and boot0's own
  simple test 32 KiB more. The March C- test of the full map has not run on
  a restored boot.
- **Boards.** Only the two boards the restore set was derived from can be
  restored (see above).
