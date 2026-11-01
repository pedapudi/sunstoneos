# First DRAM training-restore test (FEL path)

Status: **built and host-checked; the first board run failed its PHY
readback and fell back to full training (below); the restore set and helper
are revised and not yet run.** The record builder,
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

Reference for the register names: TF-A v2.12.0
`drivers/st/ddr/phy/phyinit/include/ddrphy_csr_all_cdefines.h`
(BSD-3-Clause, SHA-256 `96402f78fab3ba132a78e91a5144e6c921991f9a04bfabab34bb7b70cfd453a3`),
cached in `~/.cache/sunstoneos/sources/tf-a-st-ddrphy-v2.12.0`. It describes
the DDR4/LPDDR4 member of the family, so its absence of a byte-lane `0x73`
is supporting evidence only.

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
`~/.cache/sunstoneos/evidence/dram-clock-point-1031/`, with the commands in
`commands.txt` there. Its restore set, records and restore helper equal those
of `dram-restore-1031b/`; its wrapper adds the return of CPU-L and the DSU to
the boot ROM clock point
([design](../designs/dram-training-restore.md#the-wrapper-returns-cpu-l-and-the-dsu-to-the-rom-clock-point)).
`tools/run-ddr.py` refuses the `dram-restore-1031b/` builds, which lack it.
The 1,036-register build that ran on port 3-1 stays frozen in
`dram-restore-1031/`. No check opens a physical device.

| Check | Result and scope |
| --- | --- |
| `checks/ddr-capture-analysis.py` (`capture-analysis`) | Synthetic captures: the classes, the kind diff, and the two lane-offset rules, a status offset in one lane dropping the same offset in another lane and an excluded offset dropping its register. |
| `checks/training-record-capture.py` (`from-capture`) | Both recorded P-state-0 references produce 12,320-byte records; seven altered identity/data cases refuse. Both captures of each lifetime give the same record. |
| `checks/ddr-wrapper.py` with the restore build (`native-3-3`, `native-3-1`) | Twenty instruction cases pass for each board's record, with byte-lane register `0x73` of every lane modelled as reading 0. They include the actual patched Thumb BLX, full-width parameters, wrong SID, header/payload CRC, malformed sections, arbitrary address/mask rejection, size, missing hook, three readback failures (one mismatch, three mismatches reported with their indices, and a mismatch after `init_DRAM`) and three quick-check failures: a stuck word, an aliased address line 20 and a stuck data bit in the last window. Every case also ends with CPU-L and the DSU returned to the ROM words in the driver's order and mailbox word 15 at 0. boot0 calls and MMIO effects are modeled. |
| `checks/ddr-restore.py --reproduce` on the 1,036-register build (`reproduce-v3-3-1`) | Restore words 7–13 equal the port 3-1 mailbox: error 7, 20 writes, one hook call, `0x0a9200e6`, expected 6, read 0. |
| `checks/ddr-host-retention.py` (`host`) | Thirty-three cases pass, including a plain run that leaves CPU-L off the ROM point, one with the DSU's LFM on, a restore that ends off the ROM point and falls back, and builds without the clock return refused before any device access. Restore cases model WDT0, the SID and both USB instances: success, a SID mismatch, four returned failures (the readback failure with two reported mismatches), a hang, a disarm that does not take, a failed fallback, no new instance, an unexplained reset, an arming readback mismatch, an already enabled WDT0, upload mismatches and missing arguments. |
| `checks/training-record-copies.py` | Thirty-two paired-record cases and 25 sector interruption points pass with the 12,320-byte records. |
| `checks/ddr-wrapper.py` with the plain build (`wrapper-plain.txt`) | Passes, with the CPU PLL block modelled: the driver's order and waits for CPU-L and the DSU, CPU-B untouched, a counter wrap, a lock that drops once, and ten failure and skip cases reported in mailbox word 15. Apart from the clock return, the plain wrapper's code equals that of the full trainings that passed on both boards after the 2026-10-31 watchdog reset. |
| `checks/card-ddr.py`, `card-ddr-pio.py`, `card-ddr-host.py` | Twenty experimental and one default instruction case, thirteen timer/PIO cases and nineteen host cases pass with the shared quick check. |

The nominal instruction case executes 2,236,828 instructions under a
4,000,000-instruction fixture limit. It performs 921 masked PHY writes,
immediate readback, a second readback pass after modeled `init_DRAM` and the
quick check: 31 address-line words and sixteen 4 KiB windows 192 MiB apart
below 4 GiB, 262,392 bytes written and read each way. The quick check
detects gross errors; it reaches only the first 3 GiB of the 12 GiB map.

| Artifact or reservation | Address and size |
| --- | --- |
| Wrapper | `0x85000`, 1,004 bytes (restore) and 892 bytes (plain); limit 4 KiB |
| Mailbox | `0x86000`, 128 bytes |
| Record | `0x87000`, 12,320 bytes; reservation 24 KiB |
| FEL head shadow | `0x8e000`, 12 KiB |
| Helper and address/mask table | `0x92000`, 9,484 bytes; reservation 32 KiB |
| Private stack | `0x9b000..0x9f000`, 16 KiB |

| File | SHA-256 |
| --- | --- |
| `restore-set-v4.json` | `3464c8421cdeb23af51c831b38cab729c7a2ced9ecee3a746a08a57531da1cb4` |
| `build-restore/ddr-restore.bin` | `fa27f208629932419ba110931ae8d9ff862c2c8429c1dd701cfc1a0785dbe40a` |
| `build-restore/ddr-entry.bin` | `3e7e62aec993da2dd2e7825c958f3be35472c26fa6d6af951f4324aac0ce6e5f` |
| `build-restore/manifest.json` | `9488adbe9bf1e4f9cb15335d8de7cd5c934c350718345b7847f1f81cac1e668d` |
| `build-full/ddr-entry.bin` | `c40264b9961a5420dc218389931ee0a6d5e91296ca0608e028fe56a5563cf961` |
| `build-full/manifest.json` | `635db5a01ef60c5e8168f37b10a2e878e3d4a45eb3184e370694c8402121a10f` |
| `records/record-3-3.bin` | `44dccfa5f2e98f3beb2148b34dded8587e7772a1a9e5aa6a83a8840e98c3ecf3` |
| `records/record-3-1.bin` | `47562252745cc4b2242bb7a5c11428aa3c9dfbb7aea2534138eca4643210e436` |

The records use generation 1, boot0 checksum `0x2fce7bb3`, parameter-set
index 5 and strap CRC `0xf874a6a5`. Unknown temperature is encoded as
`INT32_MIN` with limit zero; this experiment has no temperature admission
measurement and remains limited to room-temperature starts.

Admission failures return error 1 (geometry), 2 (header CRC), 3 (payload
CRC), 4 (SID), 5 (boot0) or 6 (strap) before parameter edits.
Runtime failures return 7 (PHY readback), 8 (size), 9 (quick check),
10 (missing or repeated hook) or 11 (cache/MMU state after boot0).
A readback failure inside boot0 restores the wrapper context from SRAM,
swaps the FEL head back and returns without resuming boot0. The runner
requires success, 921 writes, one hook call, 262,392 checked bytes
and the 12,288 MiB size before its RAM-page checks; anything else falls
back through WDT0. After a readback failure the runner records the
mismatch count and the addresses and values of the second to fifth
mismatches in `restore_result`. After every run it records the CPU PLL,
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
`F=~/.cache/sunstoneos/evidence/dram-clock-point-1031` and `S` the session's
evidence directory:

1. Restore DDR start (`rs-ddr`), for port 3-3:
   `tools/run-ddr.py --fel ~/.cache/sunstoneos/build/sunxi-a733/sunxi-fel --build $F/build-restore --record $F/records/record-3-3.bin --fallback-build $F/build-full --evidence $S/rs-ddr`.
   Port 3-1 uses `record-3-1.bin`. Exit status 0 means restored, 3 means
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
| Restore mailbox | Every record check passes, the read-back of all restored registers matches, the quick check passes in about 30 ms (words 25 and 26) |
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
