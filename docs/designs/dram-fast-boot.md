# Design: shorter cold boot by reusing DRAM training

Status: **analysis complete; capture tooling built, host-checked and frozen
for both boards; no board run.** The user approved the training-restore
path (option 2 below); [its design](dram-training-restore.md) specifies the
record, the restore sequence and the qualification. The captures that the
[runbook](../board-runbooks/dram-training-capture.md) specifies have not
taken place.

Every cold start trains the board's 12,288 MiB of LPDDR5 at four operating
frequencies, and that training takes about 5.1 s, most of the cold-boot time.
This design measures where the time goes, reads the pinned boot0
(SHA-256 `829ea8e3…`) for a mechanism that reuses earlier results, ranks the
ways to shorten the step and plans the board runs that test them. Readers are
assumed to know the [boot0 analysis](../vendor-firmware/boot-rom-and-boot0.md#dram-training)
and the FEL DDR wrapper in [boot.md](../boot.md#ddr-initialization-and-ram-transfer).
Addresses written `0x5xxxx` are boot0 load addresses; the curated facts are
in [`boot0-tables.json`](../vendor-firmware/data/boot0-tables.json) under
`dram_training_controls`.

Terms used throughout:

- **P-state.** One trained DRAM operating point. boot0 trains P-state 0 at
  2040 MHz and P-states 1, 2 and 3 at 1165, 816 and 408 MHz. The frequency
  of P-state *n* is 4 × 2040 MHz ÷ (byte *n* of `dram_tpr2` + 1), with
  `dram_tpr2` = `0x13090603` (`0x559e4`).
- **Parameter block.** The `dram_para` structure that boot0 passes to its
  DRAM initialization `init_DRAM` (`0x49bdc`). Words 0–31 are the 128-byte
  block that boot0 reports and copies to the monitor header; the boot
  parameter region below reserves 96 words. Word 23 is `dram_tpr1`, word 27
  `dram_tpr10` and word 30 `dram_tpr13`.
- **Training firmware.** The program that the PHY's embedded microcontroller
  runs to train one P-state. boot0 loads it into the PHY before each run.
- **Vref.** A reference voltage that a receiver compares data against.
  Training sets it for the SoC receivers (in the PHY) and for the DRAM
  receivers (in DRAM mode registers MR14 and MR15).

## The 2040 MHz training run takes 4.4 s of the 5.1 s

boot0 prints `Training result is = 7` when a P-state's training run ends and
the `DRAM Pstate n training` label after it (`0x4f4a8`, then `0x559fe` or
`0x56128`), so each result line closes the run of the label printed next.
The runs execute in the order P-state 1, 2, 3, 0 (`0x559b0`–`0x55a2a`, then
`0x55a2c`). The table measures 58 DDR logs from both boards, 2026-10-15 to
2026-10-25, on boot0's millisecond timestamps; every log reports 12,288 MiB
and `dram_tpr13 = 0x10065`.

| Interval | Minimum | Median | Maximum |
| --- | --- | --- | --- |
| `DRAM BOOT DRIVE INFO` to `DRAM Type` (supply, version prints) | 1 ms | 2 ms | 2 ms |
| P-state 1, 1165 MHz, including controller and PHY setup | 151 ms | 151 ms | 152 ms |
| P-state 2, 816 MHz | 176 ms | 177 ms | 177 ms |
| P-state 3, 408 MHz | 345 ms | 346 ms | 346 ms |
| P-state 0, 2040 MHz | 4,403 ms | 4,414 ms | 4,424 ms |
| Return to P-state 0, size computation and report | 4 ms | 4 ms | 5 ms |
| `DRAM BOOT DRIVE INFO` to `Actual DRAM SIZE` | 5,083 ms | 5,094 ms | 5,103 ms |
| boot0's simple memory test, after that | 7 ms | 7 ms | 8 ms |

Size detection is not part of the cost. Bit 0 of `dram_tpr13` is set, so
`init_DRAM` skips the automatic rank, width and size scan (`0x49c50`, which
otherwise calls `0x57e44`; each scan step is a full initialization at
`0x57b7c`). The size comes from `0x4b74c` in about 4 ms. The host's DDR step
takes about 6 s because the FEL upload of the 245,760-byte boot0 and the
readbacks add about 0.9 s. Example log:
`~/.cache/sunstoneos/evidence/board-session-20261025-5/port-3-3/s7-ddr/firmware.log`.

The P-state 0 run takes 29 times as long as the P-state 1 run. All four runs
use the same training step selection (`0x59d6a`, from `dram_tpr10` bits
28:16) and the same firmware load (`0x4bbb8`, `0x4b97c`). The message block
of the P-state 0 run differs in byte 4, `0xa0` against `0x91`, `0x82` and
`0x83` (`0x59d4e`–`0x59de4`), and in its mode-register fields. The binary
does not show which extra work the firmware performs for that run.

## boot0 runs a Synopsys-type PHY training firmware for every P-state

The DRAM PHY at `0x0a900000` matches the published Synopsys DesignWare
LPDDR4/5 PHY interface (inferred from the access pattern below):

- Each 16-bit register *n* sits at `0x0a900000 + 2n`, and boot0 uses the
  block numbers that the published PHY uses: byte lanes at `0x10000` and
  above, master at `0x20000`, address/command sequencer at `0x41000`,
  microcontroller data memory at `0x58000`, initialization engine at
  `0x90000`, `0xc0000` and APB-only registers at `0xd0000`.
- The run routine `0x4f47c` writes `MicroContMuxSel` (`0xd0000`) = 1 and
  `MicroReset` (`0xd0099`) = 9, 1, 0, polls `UctShadowRegs` (`0xd0004`)
  bit 0, reads the message from `0xd0032` and `0xd0034`, prints it, and
  acknowledges through `DctWriteProt` (`0xd0031`). Message 7 means success.
- The message block at `0x58000` holds `SequenceCtrl`, the bitmap of
  training steps, at byte `0x10`, taken from `dram_tpr10` bits 28:16
  (`0x125f` on this board), and `HdtCtrl` = `0xff` at byte `0x12`
  (`0x59d6a`–`0x59d84`).
- NXP's published driver for this PHY family uses the same registers:
  [U-Boot `drivers/ddr/imx/phy/helper.c`](https://github.com/u-boot/u-boot/blob/d2a46d3eb2c337b81084300e8931a9c42b76c0fc/drivers/ddr/imx/phy/helper.c#L172-L187)
  reads the trained registers after clearing `MicroContMuxSel` and setting
  `UcclkHclkEnables` (`0xc0080`) to 3.

The training routine `0x55990` has one branch: bit 2 of `dram_tpr13`. Set,
it trains P-states 1, 2, 3 and then 0 (`0x559ac`); clear, it trains
P-state 0 only (`0x560f0`). For each P-state it sets the clocks
(`0x4f58c`, `0x4f500`), writes the message block (`0x4f4f0`, which reaches
`0x59d00` for LPDDR5), loads the firmware, runs it (`0x4f47c`) and programs
per-P-state PHY timing (`0x4bca8`). Both branches run the firmware for
P-state 0, and no parameter bit bypasses `0x4f47c`.

## boot0's reuse mechanism carries Vref values only

boot0 keeps a 2 KiB *boot parameter region* on the card, and the vendor
U-Boot defines its format:

- **Location and checks.** `0x49ab4` reads four sectors at card sector
  24,496 (the region start 24,504 from the table at `0x5e7cc`, minus 8)
  into `0x82df8`. It requires the magic `bootpara`, boot0's own header
  checksum at offset `0x14`, the region's additive checksum (`0x49a44`, the
  same `0x5f0a6c39` rule as the TOC1 package) and four chip-ID words equal
  to SID `0x03006200`–`0x0300620c` (`0x5d9d8`). Any mismatch rebuilds the
  region in memory from the header block at `0x47038` and sets bit 0 of
  its transfer flag (`0x49b00`–`0x49b3a`).
- **Use.** `main` passes `0x831f8`, offset `0x400` of the region, to
  `init_DRAM` (`0x5e20e`–`0x5e216`), so a valid region on the card replaces
  the parameter set that boot0 selects from its table. `main` later copies
  128 bytes of it to the monitor header (`0x5e2c4`).
- **Writer.** In the pinned vendor U-Boot (`orangepi-xunlong` U-Boot
  `b791be84`), `include/boot_param.h` lines 21–57 give the layout,
  `drivers/sunxi_flash/sunxi_flashmap.c` lines 70–73 and
  `drivers/mmc/mmc_def.h` line 42 the sector, and
  `sprite/sunxi_boot_param.c` lines 42–52 set bit 31 of word 23
  (`dram_tpr1`) with the comment "0:training dram 1: not training dram".
  The normal-boot writer `sunxi_bootparam_down` returns at its first
  statement (line 99), so only the card-production path writes the region
  (`drivers/sunxi_flash/mmc/sdmmc.c` lines 306–333).
  `configs/sun60iw2p1_a733_defconfig` line 80 enables the feature.

Two parameter bits connect the region to training:

- **`dram_tpr10` bit 29, save.** With `dram_tpr1` bit 31 clear, `init_DRAM`
  calls `0x576e4` after training (`0x49c82`–`0x49c9e`). It clears
  `MicroContMuxSel`, reads registers `0x4e` and `0x4f` of each of nine lanes
  in four byte-lane blocks (`0x0a92009c` + `0x2000` × block + `0x200` ×
  lane). It packs the four blocks' 7-bit values into one word per lane:
  words 32–40 from register `0x4e` and words 41–49 from register `0x4f`.
  It reads MR14 and, for LPDDR5, MR15 of each channel and rank into words
  50 and 51, sets `MicroContMuxSel` back to 1, and then clears bit 29
  unless bit 30 is set.
- **`dram_tpr1` bit 31, the vendor's "not training" flag.** It changes only
  message-block inputs. With bit 31 set and bit 29 clear, the P-state 0
  message block takes its MR14 and MR15 fields from words 50 and 51
  (`0x59ec4`, `0x5a08e`–`0x5a0be`), and the multi-P-state block at
  `0x55a98` programs words 32 onwards whenever bit 29 is clear.
  `SequenceCtrl` does not change, so the firmware still runs every
  training step.

The board runs with `dram_tpr1` = 0 and `dram_tpr13` = `0x10065` (bits 0,
2, 5, 6 and 16; bit 6 selects two channels at `0x578ac`). The stored
parameter set 5 holds `dram_tpr10` = `0xb25f0000`, so bit 29 is set during
training and boot0 runs the save on every boot; it then clears the bit,
which is why the reported value is `0x925f0000` (confirmed on a board: the
first port 3-3 capture, `board-session-20261025-21`, reads words 32–51
filled and equal in both captures of the lifetime). With bit 29 set during
training, the block at `0x55a98` does not run. The vendor path therefore
saves and seeds Vref values and cannot remove the 4.4 s run.

Other Allwinner loaders show the same family of mechanisms:

| Loader | Finding |
| --- | --- |
| Generic A733 boot0 `boot0_sdcard_a733.fex` (`9213f1c5…`) | Same `bootpara`, P-state and result strings; differs from the Zero 3W boot0 in 102 bytes |
| A523/T527 boot0 `sun55iw3/bin/boot0_sdcard.fex-linux5.15` (`fbe97cf0…`) | The same boot parameter region and `update_fdt_dram_para_from_bootpara`, with Allwinner's own "soft training" |
| H616 boot0 `sun50iw9/bin/boot0_sdcard.fex` (`cc0336af…`) | "Use rtc to store dram tuning para": tuning values kept in RTC registers for standby resume |
| Mainline U-Boot `d2a46d3e`, 2026-07-26 | No A733 DRAM driver. [`dram_sun55i_a523.c`](https://github.com/u-boot/u-boot/blob/d2a46d3eb2c337b81084300e8931a9c42b76c0fc/arch/arm/mach-sunxi/dram_sun55i_a523.c#L1249-L1284) runs each training step only when its `tpr10` bit is set and takes per-board delays from Kconfig (lines 1502–1504) |

All three boot0 files come from Orange Pi build `a95129bf`,
`external/packages/pack-uboot/`, and are cached under
`~/.cache/sunstoneos/sources/boot0-others-a95129bf/`.

## Training P-state 0 alone is the first step; a register restore is the larger one

Each option keeps P-state 0 at 2040 MHz, the vendor's highest operating
point for this board (`dram_clk` = `0x7f8`, divider byte 3). Times are the
`DRAM BOOT DRIVE INFO` to `Actual DRAM SIZE` interval, today 5,094 ms.

1. **Train P-state 0 only (option b).** Clear `dram_tpr13` bit 2 before
   `init_DRAM`.
   - *Feasibility:* high; boot0 implements the branch (`0x560f0`). In the
     FEL path the wrapper changes one word after copying the parameter
     block (`ddr-entry.S`); on a card, a valid boot parameter region carries
     the changed block with boot0 unmodified.
   - *Prediction:* 4,400–4,600 ms, saving 0.5–0.7 s, if the P-state 0 run
     costs the same alone. A much shorter P-state 0 run would show that its
     4.4 s depends on running last.
   - *Risk:* low. P-states 1–3 stay untrained, so DRAM frequency scaling,
     an optional power-saving mode that SunstoneOS does not use, becomes
     unavailable until a full training. The post-training call `0x581b4`,
     made only with bit 2 set (`0x57a08`), is skipped; its purpose is not
     decoded.
   - *Needs:* a wrapper variant for the FEL path, then the card write below.
2. **Restore trained PHY registers in the FEL path (option c).** Run the
   firmware with `SequenceCtrl` = 1 (device initialization only, through
   `dram_tpr10` bits 28:16), seed MR14 and MR15 through the vendor's own
   `dram_tpr1` bit 31 path, write the captured trained registers, then let
   boot0 finish (`0x4bca8`, `0x4f46c`). The wrapper would replace the call
   at `0x56142` in RAM, as `tools/prepare-ddr.py` already patches boot0's
   UART output.
   - *Feasibility:* plausible but unproven. NXP saves and restores this PHY
     family's trained registers for DRAM retention, which proves the PHY
     side; a cold start also needs DRAM-side trained values that the
     message block may not expose.
   - *Prediction:* 200–500 ms, saving 4.6–4.9 s (*estimated*: one
     initialization-only firmware run plus a few thousand register writes).
   - *Risk:* high. A missing trained register gives marginal timing and
     silent bit errors; saved values drift with temperature and voltage; the
     register semantics are undocumented. The restore must fall back to full
     training on any check failure and be qualified with the full memory
     test.
   - *Needs:* two captures in separate cold starts to separate trained
     registers from fixed configuration, the restore hook and its host
     model, then runs 4 and 5 below.
3. **Fewer training steps (option d).** `dram_tpr10` bits 28:16 select the
   steps; bits 0–4, 6, 9 and 12 are set. Removing steps could shorten the
   4.4 s run, but the step meanings for this firmware are not published and
   removing a step lowers timing margin at the top frequency. *Prediction:*
   unknown until per-step times exist. *Risk:* high. It stays unranked
   above option c until the firmware's step list is derived from its
   embedded image.
4. **boot0's own boot parameter path (option a).** Writing a region with
   `dram_tpr10` bit 29 and later `dram_tpr1` bit 31, as the vendor's
   production flow does, seeds the Vref values and still trains every
   P-state. *Prediction:* 5.1 s, no saving. Its value is as the carrier for
   option 1 on a card boot.

## Five FEL runs test the options before any card write

All runs use port 3-3 and the FEL DDR path first. Each needs a fresh
lifetime after a power removal, except where a run continues one.

1. **Capture after normal training.** Cold DDR with the unchanged
   `tools/run-ddr.py`, then `tools/run-ddr-capture.py` in the same lifetime
   ([runbook](../board-runbooks/dram-training-capture.md)). It writes no
   storage and changes no training input; its only register writes are
   `MicroContMuxSel` and `UcclkHclkEnables`, each restored. boot0 itself
   clears and sets `MicroContMuxSel` around PHY reads after every training
   (`0x5767a`–`0x5768a`), and NXP's driver writes both. *Predictions:* training as in the table,
   12,288 MiB, all stages complete, `MicroContMuxSel` read as 1 and words
   0–31 of the parameter block equal to earlier captures.
2. **Second capture** in another lifetime. Registers that differ between
   the two captures are trained values; equal ones are configuration or
   values that training reproduces.
3. **P-state 0 only (option 1)** with a wrapper variant that clears
   `dram_tpr13` bit 2. *Predictions:* one `Training result is = 7` line
   followed by `DRAM Pstate 0 training, frequency is 2040 Mhz`, 12,288 MiB
   and 4,400–4,600 ms.
4. **Memory test** in the run 3 lifetime: the
   [memory qualification image](../memory-qualification.md), March C- over
   the 12,288 MiB map in three to five runs. Its first board run after
   normal training is the baseline. *Prediction:* pass in both.
5. **Restore prototype (option 2)**, only after runs 1–4 pass and a design
   review of the hook: capture, restore, then the memory test.
   *Predictions:* 200–500 ms and a pass, or a fallback to full training.

A card boot uses option 1 only after runs 3 and 4 pass. It writes the boot
parameter region, sectors 24,496–24,499, with the parameter block from run 3,
`dram_tpr13` bit 2 clear, boot0's header checksum and the board's chip ID,
under [docs/goal.md](../goal.md) section 4: a host backup of the card first,
then the backup-first update path. The region lies in the boot area but
outside the regions that [storage-update.md](../storage-update.md) owns, so
the planner needs the extent added first. It shares the 4 MiB unit at
sectors 16,384–24,575 with the slot B header; slot A remains the fallback
while it is written. boot0's own checks fall back to full training whenever
the region is invalid or boot0 changes. *Prediction:* the card's DRAM step
takes 4,400–4,600 ms and the memory test passes.

## boot0's DRAM interval is the measured budget

The measured quantity is boot0's `DRAM BOOT DRIVE INFO` to
`Actual DRAM SIZE` interval, compared with the 58-log baseline above. A fast
path counts only with a passing memory test over the full map; any training
result other than 7 or a size other than 12,288 MiB stops it. The capture
tooling adds no step to the normal boot path.

## The capture reads registers in six stages ordered by risk

- [`boot/a733/ddr-capture.S`](../../boot/a733/ddr-capture.S): a 388-byte
  table-driven reader run from FEL at `0x85000`. It reads the listed
  registers into DRAM at `0x43100000` and above and publishes a completion
  word last.
- [`tools/prepare-ddr-capture.py`](../../tools/prepare-ddr-capture.py):
  builds the reader and six stage tables, ordered by risk: the SID chip ID
  and thermal calibration (7 words), boot0's own save window (72 registers), the controller (1,842 words), the PHY blocks
  over boot0's address extents (32,007 registers) and the firmware message
  block (1,233 registers), then PHY registers `0x0000`–`0xbfff` (49,152),
  where the published PHY keeps address/command delays. Every range except
  the last is bounded by addresses that boot0 accesses.
- [`tools/run-ddr-capture.py`](../../tools/run-ddr-capture.py): requires the
  DDR run's mailbox in SRAM, reads the 2 KiB boot parameter buffer, then
  runs each stage with one FEL execution and stops at the first failure.
  It records the port and the coordinator's temperature note.
- [`tools/analyse-ddr-captures.py`](../../tools/analyse-ddr-captures.py):
  classifies each register across captures as constant, board, lifetime or
  status; [`checks/ddr-capture-analysis.py`](../../checks/ddr-capture-analysis.py)
  checks it on synthetic captures.
- [`checks/ddr-capture.py`](../../checks/ddr-capture.py) runs the reader in
  Unicorn against every stage table, and
  [`checks/ddr-capture-host.py`](../../checks/ddr-capture-host.py) runs the
  runner against a modeled FEL device.
