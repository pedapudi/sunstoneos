# First DRAM training-restore test (FEL path)

Status: **passed on both boards on 2026-10-31** with the helper that runs
the PHY microcontroller clock for its readback after `init_DRAM` (below).
The SID-selected default also passed twice on port 3-1, with quick checks
of 34.116 and 34.117 ms, under
`~/.cache/sunstoneos/evidence/dram-default-board-1031/`.
`tools/run-ddr.py` restores by default
([records runbook](dram-restore-records.md)). March C- on a restored boot
and the qualification below remain. The record builder,
SRAM restore helper and runner are implemented. The runner falls back
through one WDT0 reset and one full training, so a failed restore costs
about 11 s instead of the board's lifetime. The March C- step still requires
acceptance of the memory qualification image's three residuals.
Independent correctness review approved the offline implementation on
2026-10-25. No launch approval or recovery qualification follows from the
host checks or that review.

This boot-time experiment replaces boot0's 4.4 s P-state 0 training with the
initialization-only firmware run (40 ms on both boards) and a write of the
board's recorded training results, then shows on the board that DRAM works
at 2040 MHz. It writes no storage: the record comes from the host.

## The first board run stopped at a byte-lane register that does not hold a write

- **Conditions.** Port 3-1, 2026-10-31, record `record-3-1.bin` of the
  1,036-register set, about 56 minutes after power-on and directly after a
  WDT0 reset: a warm start.
- **Result.** The initialization-only firmware run reported 7. The helper
  wrote and read back profile entries 1 to 19, then wrote entry 20 and read
  0: register `0x0a9200e6`, expected 6 under mask `0x37`. The runner reset
  the board through WDT0 and the fallback trained fully to 12,288 MiB in
  14.4 s. Evidence: `~/.cache/sunstoneos/evidence/dev-runs-1031/3-1-ddr-restore`.
- **The register.** `0x0a9200e6` is PHY CSR `0x10073`, register `0x73` of
  lane 0 in byte block 0. The published register maps for this PHY family
  (STMicroelectronics' TF-A phyinit header, pinned below) define no byte-lane
  register at `0x73`, and boot0 never addresses it. Its captured values
  identify it as hardware status. After any training, each of the 32 data
  lanes holds one small value, the same in all sixteen trained captures of
  both boards (6, 0, 2, 2, 0x14 and so on for the lanes of block 0); trained
  delay registers differ between boards and lifetimes. After initialization
  only, the values vary irregularly per lane (0x30, 0x2c, 0x45, 0x04, 0xcc
  on 3-1). The set held 31 of the 32 lanes. The neighbouring offsets
  `0xe0`, `0xe2` and `0xe8` already change between two captures of one
  lifetime in byte blocks 0 and 1.
- **The cause is the restore set, not the warm start.** Entries 1 to 19 are
  registers of the same lane whose recorded values differ from their
  initialization values under the mask, and each read back the written
  value, so writes reached the byte lane. Some of them vary between
  lifetimes, so values left over from the previous training cannot explain
  the matches. boot0's clock routine `0x4f58c` sets `UcclkHclkEnables` to 3
  at `0x553c6` on every path to its return, before the firmware run, and no
  other boot0 write changes it before the hook at `0x56142`, so the enable
  the capture used was present. The register read 0 in all bits, including
  bits outside the mask, which a dropped write to a storage register would
  not produce. The set derivation treated it as a training result because
  its trained value differs from its initialization value; no capture could
  show that a write does not hold.
- **Revision.** The set leaves out byte-lane offset `0xe6` in every lane,
  and every byte-lane offset that is status in any lane (`0xe0`, `0xe2`,
  `0xe8`, `0x1c4`), because the lanes share one register map: 95 registers
  fewer. The helper now finishes the pass after a mismatch and reports the
  number of mismatches and the first five, so a further register of this
  kind costs one run, not one run each.
  `checks/ddr-restore.py --reproduce` runs the frozen 1,036-register build
  with byte-lane register `0x73` modelled as reading 0 and reproduces the
  board's restore words 7–13.

## The second board run found 125 registers that do not hold a write

- **Conditions.** Port 3-1, 2026-10-31, the 941-entry set (`record-3-1.bin`
  in `dram-restore-1031b/`), directly after a WDT0 reset.
- **Result.** The helper wrote all 921 PHY entries in one hook call and 125
  read back wrong (error 7). The mailbox names five, by profile index:

  | Index | Register | Byte-lane offset | Written | Read |
  | --- | --- | --- | --- | --- |
  | 19 | `0x0a92011e` | `0x11e` (lane 0, CSR `0x8f`) | `0x9a` | `0x0000` |
  | 24 | `0x0a9201ac` | `0x1ac` (lane 0, CSR `0xd6`) | `0x52` | `0x0053` |
  | 25 | `0x0a9201ae` | `0x1ae` (lane 0, CSR `0xd7`) | `0x381` | `0x031f` |
  | 26 | `0x0a9201b6` | `0x1b6` (lane 0, CSR `0xdb`) | `0xf9f` | `0x05a1` |
  | 27 | `0x0a9201b8` | `0x1b8` (lane 0, CSR `0xdc`) | `0xfa` | `0x0096` |

  The other 23 entries of lane 0 in byte block 0 held. The runner reset the
  board through WDT0 and the fallback trained fully (exit status 3).
  Evidence: `~/.cache/sunstoneos/evidence/dev-runs-1031/3-1-ddr-restore-v4`.
- **Hardware copies.** In all twenty captures, lane 0's offset `0x11e`
  equals lane 0's offset `0x4c` (CSR `0x26`), and lane 1's `0x11e` equals
  lane 0's `0x4e` (CSR `0x27`), in each of the four byte blocks. These eight
  registers are copies that the PHY updates. At the hook the copy reads 0,
  although the source had just been written and held.
- **PHY-produced values.** Lane-0 offsets `0x1aa`–`0x1be` (CSRs
  `0xd5`–`0xdf`) of each byte block form one group whose values differ
  between lifetimes of the same kind, also after initialization only.
  Offset `0x1ae` reads `0x31f` after initialization only in both boards'
  captures, and read `0x31f` again after the write. Offsets `0x1b6` and
  `0x1b8` read values that appear in no capture. None of the four held its
  write, and the values come from the PHY. The set has 21 registers in this
  group.
- **Not explained.** Those two rules cover at most 29 entries, so at least
  96 mismatches lie elsewhere, all at indices 28 and above: among the other
  byte-lane entries, the master and `0x30000` blocks, the 445 ACSM entries
  (`0x0a982238`–`0x0a98270e`), the message block or the initialization
  engine. The mailbox does not name them. Variation between lifetimes does
  not predict a failure: six registers of lane 0 in byte block 0 vary
  between lifetimes on both boards and held.
- **Next.** Whether this hook can restore training depends on which
  registers the 96 are. Copies and PHY-produced values do not need
  restoring; a trained delay or VREF register that does not hold would mean
  this hook point is not viable. One diagnostic run answers it: the helper
  stores, for every entry, the value read before the write and the value
  read after it, and the runner reads that array
  ([survey run](#the-survey-run-classifies-every-entry)).
- **Repeat.** The clock-return build ran the same restore on port 3-1 after
  a WDT0 reset (`dev-runs-1031/3-1-ddr-clock-restore`): error 7, 124
  mismatches, the same first mismatch, and `0x0a9201ac` and `0x0a9201ae`
  read `0x53` and `0x31f` again. `0x0a9201b6` and `0x0a9201b8` read `0xa6`
  and `0xc2`, against `0x5a1` and `0x96` in the earlier run, as values the
  PHY produces would. The fallback passed, and CPU-L and the DSU ended on the
  ROM clock point.

## The survey run classifies every entry

The diagnostic restore build stores each entry's value before its write,
straight after it and at the end of the pass, and the runner decodes every
entry into a class
([design](../designs/dram-training-restore.md#a-diagnostic-build-records-every-entry-around-its-write)).
It changes nothing else about the restore, so the run is expected to fail
the readback as before (exit status 3), now with `survey.json` naming every
mismatch. With `D=~/.cache/sunstoneos/evidence/dram-survey-1031`, after a
WDT0 reset when DRAM is already trained, on port 3-1:

`tools/run-ddr.py --fel ~/.cache/sunstoneos/build/sunxi-a733/sunxi-fel --build $D/build-survey --record $D/records/record-3-1.bin --fallback-build $D/build-full --evidence $S/rs-survey`

Port 3-3 uses `record-3-3.bin`. The result decides the hook point:

- If every mismatch is a copy, a PHY-produced value or a register that
  changes later, those registers leave the set and the restore can be tried
  again at this hook.
- If a trained delay or VREF register is unchanged after its write, this
  hook cannot restore training, and the restore has to move to a point
  before the firmware run.


Reference for the register names: TF-A v2.12.0
`drivers/st/ddr/phy/phyinit/include/ddrphy_csr_all_cdefines.h`
(BSD-3-Clause, SHA-256 `96402f78fab3ba132a78e91a5144e6c921991f9a04bfabab34bb7b70cfd453a3`),
cached in `~/.cache/sunstoneos/sources/tf-a-st-ddrphy-v2.12.0`. It describes
the DDR4/LPDDR4 member of the family, so its absence of a byte-lane `0x73`
is supporting evidence only.

## The survey run separated wide-memory words from registers the PHY owns

- **Conditions.** Port 3-1, 2026-10-31, after a WDT0 reset; the 941-entry
  set with the diagnostic helper of `dram-survey-1031/`. The fallback trained
  fully. Evidence: `~/.cache/sunstoneos/evidence/dev-runs-1031/3-1-ddr-survey`.
- **Hook-time PHY state.** `MicroContMuxSel` 1 (the helper then clears it),
  `UcclkHclkEnables` 3, `MicroReset` 1 (the PHY microcontroller stalled),
  `DctWriteProt` 1 and `UctShadowRegs` 3. The last two are the firmware
  message mailbox handshake, which no restore entry addresses; the clocks
  are on and the APB owns the CSRs.
- **Result.** Of the 921 entries, 399 held their write and 397 already had
  their value. The other 125 read their old value straight after the write.
  83 of those 125 read the written value in the end-of-pass read and 42
  never did:

  | Group | Entries | Wrong at the end |
  | --- | --- | --- |
  | ACSM memory, `0x0a982000` block (CSR `0x41000`) | 80 | 1 |
  | PHY data memory, `0x0a9b0000` block (CSR `0x58000`, message block) | 8 | 4 |
  | Blocks `0x0a960000`, `0x0a962000` (CSR `0x30000`, `0x31000`), offsets `0x15e`–`0x17c` | 10 | 10 |
  | Byte-lane offset `0x11e` | 8 | 8 |
  | Byte-lane offsets `0x1ac`–`0x1ba` and `0x130` | 19 | 19 |

- **Wide memories.** In the ACSM block every late entry sits in halfword 0
  or 1 of an 8-byte word whose halfword 3 is also an entry; the 52 entries
  at halfword 3 held at once and the lower halfwords read right at the end.
  In the data memory the late entries sit in the lower halfword of a 4-byte
  word whose upper halfword is an entry. The memories hold a halfword write
  until the word's highest halfword is written, which stores the word, and
  reads return the stored word until then. The five entries wrong at the
  end are the only ones whose highest halfword is not in the set
  (`0x0a9822b8`, `0x0a9b0030`, `0x0a9b0058`, `0x0a9b00dc`, `0x0a9b00e0`), so
  their words were never stored. boot0's PIE and message-block loaders
  (`0x59100`, `0x590c4`) write these memories one halfword at a time in
  ascending order, which always completes each word. These entries differ
  between initialization-only and trained lifetimes and are restorable.
- **Registers the PHY owns.** The other 37 never took a write. Byte-lane
  offset `0x11e` copies offset `0x4c` or `0x4e` in all twenty captures. The
  lane-0 group `0x1aa`–`0x1be`, offset `0x130`, and offsets `0x15e`–`0x17c`
  of the two `0x30000`-region blocks change between lifetimes of the same
  kind, also after initialization only; `0x0a9201b6` and `0x0a9201b8` read
  `0x0fb5` and `0x0196` here, a third pair of values in three runs. None of
  them is a trained delay or VREF: their values exist without training and
  the PHY replaces them. Every other byte-lane register of the set, which
  holds the trained delays and VREF settings, held its write or already had
  its value.
- **Rule and expected result.** The v5 set leaves out every register in
  these PHY-owned groups, 42 in all (the 37, and five of the same groups that
  happened to hold their value already, including `0x0a940014`, whose values
  vary between lifetimes as those of the `0x30000`-region offset `0x17c` do). The helper writes whole
  ACSM and data-memory words and reads every entry back after the last
  write. Against the port 3-1 survey, every one of its 42 mismatches is
  either left out (37) or completed by the whole-word writes (5); no host
  check reproduces this. The expected result of the v5 restore is no readback mismatch, a
  return into boot0, the post-`init_DRAM` readback and the quick check;
  whether boot0's later steps then leave working memory is what the run
  tests.

## The clocked readback restored DRAM on both boards

Builds from `dram-final-readback-1031/` (commit `30e575a0`), 2026-10-31.

| Run | Result |
| --- | --- |
| Port 3-1, survey build, after a WDT0 reset (`dev-runs-1031/3-1-ddr-final-survey`) | Error 0, 879 writes, one hook call, 262,392 checked bytes, quick check 36.374 ms, both RAM pages match, exit 0. Survey: 398 held, 393 already at value, 88 stored later, no mismatch; after `init_DRAM` the readback found `MicroContMuxSel` 1 and `UcclkHclkEnables` 0. |
| Port 3-3, restore build (`dev-runs-1031/3-3-ddr-final-restore`) | Error 0, 879 writes, quick check 34.028 ms, both RAM pages match, exit 0; about 2.0 s for the runner command against about 6.6 s for a plain full-training run on 3-3 (`3-3-ddr-clock-plain-2`). |

Both logs show `Training result is = 7` 36 ms after `DRAM BOOT DRIVE INFO`,
the 12,288 MiB size line 6–7 ms later with `dram_tpr13 = 10061`, and
`DRAM simple test OK.`; CPU clocks ended at the entry point (word 15 = 0).

## The v5 run held every write; the readback after init_DRAM read the data memory with its clock stopped

- **Conditions.** Port 3-1, 2026-10-31, after a WDT0 reset; the v5 survey
  build from `dram-restore-v5-1031/`. Evidence:
  `~/.cache/sunstoneos/evidence/dev-runs-1031/3-1-ddr-v5-survey`.
- **Result at the hook.** Of the 879 PHY entries, 398 held their write, 393
  already had their value and 88 were stored with their wide-memory word;
  none mismatched, and the helper rewrote 89 halfwords. boot0 resumed: the
  firmware reported result 7 at 4,947 ms, the size line followed at
  4,954 ms with `dram_tpr13 = 10061`, and `DRAM simple test OK.` at
  4,961 ms. With initialization only and no restore, that simple test fails.
  It writes 4,096 words at `0x40000000` and 4,096 at `0xc0000000` (2 GiB
  higher) and reads both back (`0x49cc8`, `0x4b4cc`), so the restored state
  gave working memory at those two places; no wider test ran.
- **Result after `init_DRAM`.** The helper's second readback reported
  18 mismatches, all in the PHY data memory: the first at `0x0a9b0030`
  (expected `0x0100`, read 0), then `0x0a9b0034`–`0x0a9b003a`, all 0. The
  set has 20 data-memory entries, and the other two expect 0 under their
  masks. The helper returned error 7 about 1 ms into `restore_check`, so
  its quick check did not run (word 10 is 0) and the runner skipped its RAM
  pages, reset through WDT0 and trained fully (result 7, 12,288 MiB,
  `dram_tpr13 = 10065`). CPU clocks ended at the entry point.
- **Cause.** boot0 does not clear or reuse the message block after the
  hook. Its final PHY stage ends with `UcclkHclkEnables` = 2, and `0x57310`
  sets it to 3, reads thirteen message-block halfwords to program controller
  timing, and leaves it at 0. With the PHY microcontroller clock off, the
  data memory reads 0 over APB; the other 859 entries, in the ACSM memory
  and the registers, read their restored values. The capture reader always set the clock to 3 before reading,
  which is why every capture shows the values
  ([design](../designs/dram-training-restore.md#ram-only-implementation-contract)).
  The data memory keeps its contents meanwhile (inferred, because boot0
  reads it after stopping the clock). `checks/ddr-restore.py` reproduces
  the board's restore words 8–13 and 27–31, with error 15 in place of 7,
  when the helper's clock write has no effect.
- **`dram_tpr13` bit 2.** The bit selects boot0's four-P-state training;
  clear, boot0 trains and runs P-state 0 (2040 MHz) only (`0x55990`,
  `0x4bcb2`). The restore clears it, as the record's P-state-0-only
  captures require; the stored set 5 has it set, so the fallback prints
  `10065`. With the bit clear, boot0 trains no lower P-state, so a restored
  boot cannot switch DRAM frequency (inferred).
- **Change.** The second readback clears `MicroContMuxSel`, sets
  `UcclkHclkEnables` to 3, reads every entry, writes back the value it found
  and sets the mux to 1, as `0x57310` does. A mismatch there now returns
  error 15, so the mailbox names the failing pass. The survey build records
  the mux and clock values that pass found in survey header words 8–10.

## The restore set comes from the existing lifetimes

Each board has two four-P-state, two P-state-0-only and one
initialization-only lifetime ([capture runbook](dram-training-capture.md)).
The set is fixed from those, in `~/.cache/sunstoneos/evidence/dram-restore-1031b/restore-set-v4.json`,
by the single command recorded beside it in `restore-set-v4.command`
(`tools/analyse-ddr-captures.py` over all twenty captures):

- **Registers: 941, all 16-bit PHY registers or parameter words.** For
  each board, the registers whose P-state-0-only values all differ from its
  initialization-only values (920 on 3-3, 925 on 3-1), united. Status is
  decided over all twenty captures of both boards, which removes the
  controller word `0x0a51022c`; it changed within a port 3-1 lifetime.
- **Left out, with the reason:**
  - the `0xa0000` block (below);
  - the APB-only and DRTUB blocks, which are firmware controls;
  - `dram_tpr10` itself (parameter word 27), which the restore edits;
  - 27 message-block fields that hold one value in every P-state-0-only
    capture, such as the training-step value at `0x0a9b0010`: they are
    firmware inputs that the step value sets, not results;
  - the controller's mode-register-read result words `0x0a02002c` and
    `0x0a020040` and the MR14 copies `0x0a110094` and `0x0a510094`, which a
    restore cannot meaningfully write; MR14 travels in parameter word 50;
  - byte-lane offsets `0xe0`, `0xe2`, `0xe8` and `0x1c4` in every lane
    (`--status-by-lane-offset`), status in the lanes of byte blocks 0 and 1;
  - byte-lane offset `0xe6` in every lane (`--exclude-lane-offset 0xe6`),
    which does not hold a written value on port 3-1 (above).
- **Two groups.** 394 registers vary between trainings: they are training
  results. 547 hold one value in every P-state-0-only capture of both
  boards and differ after the initialization-only run; they are the
  P-state-0-only path's training state, which the restore writes over the
  initialization-only path's. The 1,036-register `restore-set-v3.json` and
  the earlier `restore-set-final.json` (1,069 registers) are superseded.
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

## Implemented RAM-only preparation

1. **Record builder:** `tools/training-record.py from-capture`, turning a
   capture directory and the set file into the record format the design
   specifies (chip ID, boot0 checksum, strap CRC, temperature fields left
   as "unknown", registers as runs, masks, MR14/MR15).
2. **Restore wrapper variant:** `tools/prepare-ddr.py --variant restore`.
   It uploads the record to SRAM, applies the P-state-0-only and
   initialization-only parameter edits plus `dram_tpr1` bit 31 and words
   32–51, redirects the call at `0x56142` to a table-driven writer
   (`MicroContMuxSel` cleared, writes in record order, set to 1, then
   `0x4bca8`), and after `init_DRAM` reads every restored register back and
   runs quick-check part A below 4 GiB. It reports each step in the
   mailbox. A readback mismatch does not end the write pass: the helper
   writes and reads every entry, then fails with the count of mismatches
   in mailbox word 27, the first in words 11–13 and the next four in words
   28–31 as profile index and value read. On any failure it stops and reports; it makes no
   in-place retraining, because retraining within a lifetime has never run
   on these boards. `tools/run-ddr.py` then resets the board through WDT0
   and runs the plain four-P-state wrapper once on the new FEL instance
   ([design](../designs/dram-training-restore.md#the-fel-runner-falls-back-through-one-watchdog-reset)).
3. **Host checks:** the wrapper in Unicorn with the patched boot0 calls
   modeled (edit order, the record checks, the writer's accesses, every
   refusal), and `run-ddr.py` with a modeled FEL device for the restore mode.

## Host evidence and SRAM bounds

The frozen artifacts and check results are in
`~/.cache/sunstoneos/evidence/dram-final-readback-1031/`, with the commands
in `commands.txt` there: the 899-entry set, both boards' records, the
restore helper with whole-word writes and the clocked readback after
`init_DRAM`, its survey build, and the plain wrapper with the return of
CPU-L and the DSU to the boot ROM clock point
([design](../designs/dram-training-restore.md#the-wrapper-returns-cpu-l-and-the-dsu-to-the-rom-clock-point)).
The set, records, wrappers and plain build equal those of
`dram-restore-v5-1031/`; only the helper differs. `tools/run-ddr.py`
refuses restore builds without the `final_readback` error, which includes
the `dram-restore-v5-1031/` builds, and the `dram-restore-1031b/` builds,
which lack the clock return. Earlier builds stay frozen in
`dram-restore-v5-1031/`, `dram-clock-point-1031/` and `dram-survey-1031/`.
The 1,036-register build that ran on port 3-1 stays frozen in
`dram-restore-1031/`. No check opens a physical device.

| Check | Result and scope |
| --- | --- |
| `checks/ddr-capture-analysis.py` (`capture-analysis`) | Synthetic captures: the classes, the kind diff, and the two lane-offset rules, a status offset in one lane dropping the same offset in another lane and an excluded offset dropping its register. |
| `checks/training-record-capture.py` (`from-capture`) | Both recorded P-state-0 references produce 11,640-byte records; seven altered identity/data cases refuse. Both captures of each lifetime give the same record. |
| `checks/ddr-restore.py` with the restore build (`native-restore-3-3`, `native-restore-3-1`) and the survey build (`native-survey-3-3`, `native-survey-3-1`) | Twenty-two instruction cases pass for each board's record, 23 with the survey build, with byte-lane register `0x73` of every lane modelled as reading 0 and the ACSM and data memories modelled as storing a word on its highest halfword. Each touched wide word must be written once, whole and in ascending order, with the other halfwords' values kept; five helper mutations of the whole-word writes are caught. They include the actual patched Thumb BLX, full-width parameters, wrong SID, header/payload CRC, malformed sections, arbitrary address/mask rejection, size, missing hook, three readback failures (one mismatch, three mismatches reported with their indices, and a mismatch after `init_DRAM`, error 15), the data memory reading 0 while its clock is off, with the clock-stopped case reproducing the port 3-1 v5 mailbox (error 15 in place of 7, `0x0a9b0030`, 18 mismatches, indices `0x35c`–`0x35f`), and three quick-check failures: a stuck word, an aliased address line 20 and a stuck data bit in the last window. Every case also ends with CPU-L and the DSU returned to the ROM words in the driver's order and mailbox word 15 at 0, and every case that reaches the readback after `init_DRAM` writes `UcclkHclkEnables` 3 and then the value boot0 left, 0, or 2 in one case. The check fails four helper variants: the previous helper (commit `f0595d83`), one that leaves the clock on, one that sets the clock only after its reads and one that writes 0 back instead of the value it read. boot0 calls and MMIO effects are modeled. |
| `checks/ddr-restore.py --reproduce` on the 1,036-register build (`reproduce-v3-3-1`) | Restore words 7–13 equal the port 3-1 mailbox: error 7, 20 writes, one hook call, `0x0a9200e6`, expected 6, read 0. |
| `checks/ddr-host-retention.py` (`host`) | Thirty-nine cases pass, four of them with the survey build, including a plain run that leaves CPU-L off the ROM point, one with the DSU's LFM on, a restore that ends off the ROM point and falls back, and builds without the clock return refused before any device access. Restore cases model WDT0, the SID and both USB instances: success, a SID mismatch, five returned failures (a readback failure at the hook and one after `init_DRAM`, each with two reported mismatches and the failing pass recorded; a survey that agrees with an after-`init_DRAM` failure and shows the clock state that pass found), a restore build without the `final_readback` error refused before device access, a hang, a disarm that does not take, a failed fallback, no new instance, an unexplained reset, an arming readback mismatch, an already enabled WDT0, upload mismatches and missing arguments. |
| `checks/training-record-copies.py` | Thirty-two paired-record cases and 24 sector interruption points pass with the 11,640-byte records. |
| `checks/ddr-wrapper.py` with the plain build (`wrapper-plain.txt`) | Passes, with the CPU PLL block modelled: the driver's order and waits for CPU-L and the DSU, CPU-B untouched, a counter wrap, a lock that drops once, and ten failure and skip cases reported in mailbox word 15. Apart from the clock return, the plain wrapper's code equals that of the full trainings that passed on both boards after the 2026-10-31 watchdog reset. |
| `checks/card-ddr.py`, `card-ddr-host.py` | Twenty experimental and one default instruction case and nineteen host cases pass with the shared quick check; the instruction cases model boot0 leaving `UcclkHclkEnables` at 0 and require the helper to write 3 and then 0. |

The nominal instruction case executes 2,247,330 instructions under a
4,000,000-instruction fixture limit. It performs 879 masked PHY writes and
89 wide-memory rewrites, a readback pass, a second readback pass after
modeled `init_DRAM` and the
quick check: 31 address-line words and sixteen 4 KiB windows 192 MiB apart
below 4 GiB, 262,392 bytes written and read each way. The quick check
detects gross errors; it reaches only the first 3 GiB of the 12 GiB map.

| Artifact or reservation | Address and size |
| --- | --- |
| Wrapper | `0x85000`, 1,004 bytes (restore) and 892 bytes (plain); limit 4 KiB |
| Mailbox | `0x86000`, 128 bytes |
| Record | `0x87000`, 11,640 bytes; reservation 24 KiB |
| FEL head shadow | `0x8e000`, 12 KiB |
| Helper and address/mask table | `0x92000`, 9,684 bytes (9,912 with the survey); reservation 32 KiB |
| Survey (survey build only) | `0x96000`, 7,096 bytes |
| Private stack | `0x9b000..0x9f000`, 16 KiB |

| File | SHA-256 |
| --- | --- |
| `restore-set-v5.json` | `7c8ae56d522515bc50acce99793436dfaed75805a33b6b87aa7c8e4681d40868` |
| `build-restore/ddr-restore.bin` | `364ba9f1dd62f203beeb1e9f6245254c7583c79a0aaf53b42fb0fe3871d98cf0` |
| `build-restore/ddr-entry.bin` | `3e7e62aec993da2dd2e7825c958f3be35472c26fa6d6af951f4324aac0ce6e5f` |
| `build-restore/manifest.json` | `9ba0eb09b85fe44345748dbf318caf364c6eca4f17de373e2ad94f395c773e65` |
| `build-survey/ddr-restore.bin` | `06b277aca5083f07427eb42a5ce50fe1060cc5048fb1c747f2a6b8a04742c3d7` |
| `build-survey/manifest.json` | `f4f0c909495cb65b9a0abeacaa2b28ab774694fa914ffeeedf72935aeda71f14` |
| `build-full/ddr-entry.bin` | `c40264b9961a5420dc218389931ee0a6d5e91296ca0608e028fe56a5563cf961` |
| `build-full/manifest.json` | `635db5a01ef60c5e8168f37b10a2e878e3d4a45eb3184e370694c8402121a10f` |
| `records/record-3-3.bin` | `c82e0a324c9748984b98081d61a3deb65ffd9e73e823d86fb883ab771cce22fa` |
| `records/record-3-1.bin` | `932c68a42f9c3da996359855b21bdd8287b66899d1d85deb901f2ea2265c130a` |

The records use generation 1, boot0 checksum `0x2fce7bb3`, parameter-set
index 5 and strap CRC `0xf874a6a5`. Unknown temperature is encoded as
`INT32_MIN` with limit zero; this experiment has no temperature admission
measurement and remains limited to room-temperature starts.

Admission failures return error 1 (geometry), 2 (header CRC), 3 (payload
CRC), 4 (SID), 5 (boot0) or 6 (strap) before parameter edits.
Runtime failures return 7 (PHY readback at the hook), 8 (size), 9 (quick
check), 10 (missing or repeated hook), 11 (cache/MMU state after boot0) or
15 (PHY readback after `init_DRAM`); the card path's own stop codes are
12–14.
A readback failure inside boot0 restores the wrapper context from SRAM,
swaps the FEL head back and returns without resuming boot0. The runner
requires success, 879 writes, one hook call, 262,392 checked bytes
and the 12,288 MiB size before its RAM-page checks; anything else falls
back through WDT0. After a readback failure the runner records the
failing pass, the mismatch count and the addresses and values of the
second to fifth mismatches in `restore_result`. After every run it records the CPU PLL,
pattern, LFM and mux words as `cpu_clocks` and mailbox word 15 as
`clock_point_result`; a restore that ends off the entry clock point also
falls back, and a plain or fallback run that does so fails.

## Commands for one board

The per-board record lives on the host, in the frozen `records` directory,
and the runner uploads it to SRAM `0x87000` on every run. The reconnect
access rule must cover the port, because the fallback re-enumerates USB.
Both records come from room-temperature cold starts. The first run should
match them: a fresh lifetime after at least one hour unpowered. A run after
a watchdog recovery is a warm start with the PMIC rails still set, which no
capture covers; the quick check and the fallback bound its cost, but a pass
there does not qualify warm restores. A board whose DRAM is already
trained in the current FEL instance first resets through WDT0 to a new FEL
instance, as the port 3-1 run of 2026-10-31 did, so that `init_DRAM` starts
from reset controller and PHY state. With
`F=~/.cache/sunstoneos/evidence/dram-final-readback-1031` and `S` the session's
evidence directory:

1. Restore DDR start (`rs-ddr`), first with the survey build, for port 3-1:
   `tools/run-ddr.py --fel ~/.cache/sunstoneos/build/sunxi-a733/sunxi-fel --build $F/build-survey --record $F/records/record-3-1.bin --fallback-build $F/build-full --evidence $S/rs-ddr`.
   The survey build restores as the restore build does and also leaves
   `survey.json`. Port 3-3 uses `record-3-3.bin`; `$F/build-restore` is the
   same restore without the survey. Exit status 0 means restored, 3 means
   the restore failed and the fallback training passed; either leaves
   trained DRAM for the next image.
2. The v5 capture (`rs-capture-1`) with `--ddr-evidence $S/rs-ddr`, only
   after exit status 0.
3. The [memory qualification image](../memory-qualification.md), March C-
   over the 12,288 MiB map, three to five runs.

## Predictions

| Item | Prediction |
| --- | --- |
| `rs-ddr/firmware.log` | `Training result is = 7` about 36 ms after `DRAM BOOT DRIVE INFO`; `Actual DRAM SIZE =12288 M` about 40 ms after it; `DRAM simple test OK.` |
| Restore mailbox | Every record check passes, both readbacks of all restored registers match, the quick check passes in about 30 ms (words 25 and 26); error 0, 879 writes, one hook call, 262,392 checked bytes |
| Survey (survey build) | No mismatch at the hook, with the class counts of the v5 run (398 held, 393 already at value, 88 stored later, 89 rewrites); `after_init` shows `MicroContMuxSel` 1 and `UcclkHclkEnables` 0; `agrees_with_mailbox` true |
| Runner | `PASS: DRAM restored; quick check … ms; both 4 KiB pages match.`, exit status 0; CPU clocks at the entry point |
| WDT0 | Reads `0x41` after arming and `0` in the same request that returns from the wrapper |
| Both 4 KiB pages of `run-ddr.py` | Read back |
| `rs-capture-1` against the record | Every restored register equal to the record; the other registers equal the initialization-only capture or within the P-state-0-only lifetime spread |
| March C- | Pass over the full map |
| DRAM interval | About 70 ms with the quick check, against 4,437–4,451 ms for P-state-0-only training and 5,088–5,102 ms for the four-P-state training it replaces |

A restore failure or hang ends in one WDT0 reset, about 6 s, and one full
training, about 5.1 s. A failure of that fallback, or of any later step,
stops the board's session as every runner does today.

## Qualifying the restore later

The first test shows that one restored boot works. Qualification, once
power cycles are possible, adds per board:

1. Ten restored lifetimes at room temperature, each with March C-.
2. Restored lifetimes after cold and warm starts, each with March C- and a
   stress run; these also give the temperature limit the record needs.
3. The fallback cases of the design: a wrong record CRC, the other board's
   record, a changed strap CRC and a forced read-back failure, with identity and CRC failures
   refusing before parameter edits and the readback failure stopping before
   further DRAM use.
4. An in-place retraining after a refused restore, once a single run shows
   that `init_DRAM` can train twice in one lifetime.
5. The card path only after these pass, under
   [docs/goal.md](../goal.md) section 4.

## Confidence and residual risk

- **High confidence** that the 904 registers in both boards' diffs differ
  between trained and untrained PHYs on two chips. The other 37 are in one
  board's diff only and are restored as harmless on the other, where they
  hold their initialization value. A difference does not prove that a
  register holds a written value: byte-lane register `0x73` differed and
  did not.
- **Hardware-owned registers that stay in the set:** status is decided
  from two captures per lifetime, and `0x0a51022c` changed in only one of
  ten lifetimes. A status register that flips as rarely, or one that never
  changes within a lifetime, as byte-lane register `0x73` did not, may
  still be in the set. Its readback then mismatches; the run reports every
  such register, falls back through WDT0 and costs about 14 s but no
  safety.
- **Registers the set may miss:**
  - a trained register that equals its initialization value in every
    captured lifetime on both boards (2–4% of the set match that value on
    one board, so about two of 941 could be missed on both);
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
