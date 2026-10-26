# First DRAM training-restore test (FEL path)

Status: **designed; not built and not run.** It needs the restore hook and
the record builder below, their host checks and the independent review that
[the restore design](../designs/dram-training-restore.md) requires, and a
fresh power-cycled lifetime per board. No power cycle is available after
the 2026-10-25 reconnect, so this test waits; it is written now so that it
can run and be qualified as soon as power cycles are possible again.

The test replaces boot0's 4.4 s P-state 0 training with the
initialization-only firmware run (40 ms on both boards) and a write of the
board's recorded training results, then shows on the board that DRAM works
at 2040 MHz. It writes no storage: the record comes from the host.

## The restore set comes from the existing lifetimes

Each board has two four-P-state, two P-state-0-only and one
initialization-only lifetime ([capture runbook](dram-training-capture.md)).
The set is fixed from those, in
`~/.cache/sunstoneos/evidence/dram-training-analysis-20261025/restore-set-final.json`:

- **Registers: 1,069.** The union of both boards' P-state-0-only minus
  initialization-only diffs, without the `0xa0000` block, holds 1,070
  registers (`port-3-3-restore-set-v2.json`, `port-3-1-restore-set-v2.json`).
  The final four-P-state lifetimes class one of them, channel 1 controller
  word `0x0a51022c`, as status, and it is removed. Of the 1,069, about 250
  also vary between the two four-P-state lifetimes of each board, and the
  rest are equal there; the four-P-state captures change nothing else,
  because the restore runs boot0's P-state-0-only path.
- **The `0xa0000` block stays out.** After a four-P-state training, 8–9% of
  its words change between lifetimes (941 on 3-3, 1,039 on 3-1), much as
  trained registers do. After a P-state-0-only training nearly every word
  of all four copies changes (about 3,500 per copy). So the four-P-state
  path writes the block and the P-state-0-only path leaves it at its
  power-on contents; the post-training call `0x581b4`, which runs only with
  `dram_tpr13` bit 2 set (`0x57a08`), is the likely writer (inferred). The
  restore runs the P-state-0-only path, so it leaves the block as that
  path does, and P-state-0-only training already passes boot0's simple
  test and the two page checks with it.
- **Values:** one capture of the board's later P-state-0-only lifetime
  (`-27` for 3-3, `-35` for 3-1). Values from one training are restored
  together, because a byte-lane register can move by one coarse step of
  `0x40` between trainings while its fine partner moves with it; mixing
  lifetimes could pair values that no training produced.
- **Mode registers:** MR14 and MR15 from that capture's parameter words 50
  and 51, through boot0's own `dram_tpr1` bit 31 path. MR12 is not captured.

## What has to be built first

1. **Record builder:** `tools/training-record.py from-capture`, turning a
   capture directory and the set file into the record format the design
   specifies (chip ID, boot0 checksum, strap CRC, temperature fields left
   as "unknown", registers as runs, masks, MR14/MR15).
2. **Restore wrapper variant:** `tools/prepare-ddr.py --variant restore`.
   It uploads the record to SRAM, applies the P-state-0-only and
   initialization-only parameter edits plus `dram_tpr1` bit 31 and words
   50–51, redirects the call at `0x56142` to a table-driven writer
   (`MicroContMuxSel` cleared, writes in record order, set to 1, then
   `0x4bca8`), and after `init_DRAM` reads every restored register back and
   runs quick-check part A below 4 GiB. It reports each step in the
   mailbox. On any failure it stops and reports; this first test makes no
   in-place retraining, because retraining within a lifetime has never run
   on these boards.
3. **Host checks:** the wrapper in Unicorn with the patched boot0 calls
   modeled (edit order, the record checks, the writer's accesses, every
   refusal), and `run-ddr.py` with a modeled FEL device for the restore mode.

## Commands for one board, once built

In a fresh lifetime after a power removal, at room temperature after at
least one hour unpowered, matching the captures:

1. Restore DDR start (`rs-ddr`): `run-ddr.py` with the restore build and
   the board's record.
2. The v5 capture (`rs-capture-1`) with `--ddr-evidence $S/rs-ddr`.
3. The [memory qualification image](../memory-qualification.md), March C-
   over the 12,288 MiB map, three to five runs.

## Predictions

| Item | Prediction |
| --- | --- |
| `rs-ddr/firmware.log` | `Training result is = 7` about 36 ms after `DRAM BOOT DRIVE INFO`; `Actual DRAM SIZE =12288 M` about 40 ms after it; `DRAM simple test OK.` |
| Restore mailbox | Every record check passes, the read-back of all restored registers matches, part A passes |
| Both 4 KiB pages of `run-ddr.py` | Read back |
| `rs-capture-1` against the record | Every restored register equal to the record; the other registers equal the initialization-only capture or within the P-state-0-only lifetime spread |
| March C- | Pass over the full map |
| DRAM interval | About 60 ms with the quick check, against 4,437–4,451 ms for P-state-0-only training |

A failure at any step stops the board's session, as every runner does
today, and the board needs a power removal.

## Qualifying the restore later

The first test shows that one restored boot works. Qualification, once
power cycles are possible, adds per board:

1. Ten restored lifetimes at room temperature, each with March C-.
2. Restored lifetimes after cold and warm starts, each with March C- and a
   stress run; these also give the temperature limit the record needs.
3. The fallback cases of the design: a wrong record CRC, the other board's
   record, a changed strap CRC and a forced read-back failure, each
   refusing before any parameter edit.
4. An in-place retraining after a refused restore, once a single run shows
   that `init_DRAM` can train twice in one lifetime.
5. The card path only after these pass, under
   [docs/goal.md](../goal.md) section 4.

## Confidence and residual risk

- **High confidence** that the 1,007 registers in both boards' diffs,
  after the status removal, are training state: they differ between
  trained and untrained PHYs on two chips, and about 660 of them never
  varied between trainings. The other 62 are in one board's diff only and
  are restored as harmless on the other.
- **Registers the set may miss:**
  - a trained register that equals its initialization value in every
    captured lifetime on both boards (2–4% of the set match that value on
    one board, so about two of 1,070 could be missed on both);
  - registers outside the captured address ranges, which follow boot0's
    own accesses (`0x0000`–`0xbfff` read zero);
  - the `0xa0000` block, whose words change between power-cycled
    lifetimes and whose function is unknown;
  - MR12 and any mode register that the message block does not carry.
  The memory test is the check for all of these.
- **The reference path has no full memory test.** P-state-0-only training
  has passed boot0's simple test and two 4 KiB pages per lifetime, never
  March C-. The restore inherits that path, so qualification needs a
  March C- baseline after a P-state-0-only training as well as after the
  standard four-P-state one.
- **Temperature:** every capture is a room-temperature cold start. Until
  cold and warm lifetimes exist, a record is valid only for room-temperature
  cold starts, and the temperature gate needs a THS reading that is not
  built yet.
- **Spread:** two P-state-0-only lifetimes per board give a rough spread
  (median 2 codes, maximum 13 on the saved Vref bytes and a coarse step of
  `0x40` on a few byte-lane delays); the retraining interval waits for more
  data.
