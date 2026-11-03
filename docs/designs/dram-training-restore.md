# Design: restoring DRAM training from a record on the microSD card

Status: **qualified on the board** for the RAM-only restore through FEL (explicit-record restores on both boards, the SID-selected default twice on port 3-1). **Host-tested** for the card path: the derived boot0, its helper and the paired-record planner. **Open:** full-map memory qualification after restore, temperature admission, lower P-states and physical card deployment.

A cold start trains the board's 12,288 MiB of LPDDR5 at four operating frequencies in about 5.1 s. A restore replaces boot0's 4.4 s P-state 0 training with an initialization-only firmware run and a write of 879 saved PHY registers, and keeps P-state 0 at 2040 MHz. [DRAM fast boot](dram-fast-boot.md) derives boot0's training flow and its cost. This design specifies how a saved record is identified, the restore sequence, its fallbacks, the record and its card region. Addresses written `0x5xxxx` are load addresses in the pinned boot0 (SHA-256 `829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3`).

Measured figures, the capture analysis, the offline card evidence and the qualification plan are records in [dram-training-restore.jsonl](../evidence/dram-training-restore.jsonl). The restore runner is described in the [record runbook](../board-runbooks/dram-restore-records.md); the derivation of the register set is in the [first-test runbook](../board-runbooks/dram-restore-test-1.md).

## The restore set comes from three kinds of capture

The read-only capture ([runbook](../board-runbooks/dram-training-capture.md)) reads the parameter block, the SID chip ID and thermal calibration, the controller, the PHY over boot0's own address extents, the firmware message block and PHY registers `0x0000`–`0xbfff`, which boot0 never accesses; all read zero. `tools/analyse-ddr-captures.py` classifies each register as status, lifetime, board or constant. Three kinds of capture feed the restore set:

1. **Four-P-state captures** after boot0's normal training. They show chip and lifetime variation and the status class, but are not the restore reference, because the restore runs boot0's P-state-0-only path, which differs: message byte 4 is `0x30` (`0x59de6`), `0x4bca8` branches on `dram_tpr13` bit 2 (`0x4bcb2`), and the call `0x581b4` (`0x57a08`) does not run. The block at `0x55a98` runs on neither path on this board, because `dram_tpr10` bit 29 is set during training (`0x55a9e`).
2. **P-state-0-only captures**, taken after a training with `dram_tpr13` bit 2 clear. These are the reference values.
3. **Device-initialization-only captures**, taken after a run with training-step value `0x0001`. Every PHY register that differs between kinds 2 and 3 was set by training, including values that repeat in every training. The restore set is those registers, minus the status class, each with the mask of the bits that differ.

The P-state-0-only and initialization-only kinds use DDR wrapper variants (`tools/prepare-ddr.py --variant pstate0` or `init-only`). The firmware accepts the initialization-only step value.

The fixed set has 899 entries: 402 byte-lane registers, 445 address/command sequencer (ACSM) words, 10 registers of the two `0x30000`-region blocks, 20 message-block fields, 20 parameter words and two others. It is derived by one recorded command over all captures, per board and united, with status pooled over both boards and with firmware controls, firmware inputs and controller read-result words left out. Exclusions:

- The `0xa0000` block (ACSM sequence memory loaded per kind): its words change between lifetimes of the same kind, including words that boot0 writes with fixed values. The restore relies on boot0 rewriting it on its own path.
- Byte-lane status registers, decided per register offset because the lanes share one register map: `0xe0`, `0xe2`, `0xe8` and `0x1c4`, and offset `0xe6` (register `0x73`), which does not hold a written value ([runbook](../board-runbooks/dram-restore-test-1.md#the-first-board-run-stopped-at-a-byte-lane-register-that-does-not-hold-a-write)).
- Registers the PHY owns, which do not keep an APB write at the hook ([runbook](../board-runbooks/dram-restore-test-1.md#the-survey-run-separated-wide-memory-words-from-registers-the-phy-owns)): byte-lane offset `0x11e` (a copy of offset `0x4c` or `0x4e`), offsets `0x1aa`–`0x1be` and `0x130`, offsets `0x15e`–`0x17c` of the blocks at CSR `0x30000` and `0x31000` (`0x0a960000`, `0x0a962000`) and `0x0a940014`.

Of the 899 entries, 20 are parameter words 32–51 and 879 are masked PHY halfwords. The chip IDs of the two boards differ in their last word only (`0x40892413`, `0x40792413`), and trained byte-lane values differ between the boards by a median of 2 codes, so a record belongs to one board, as the chip-ID check requires. The records have no qualified temperature range; a record is valid for room-temperature cold starts.

The LPDDR5 mode registers MR14 and MR15 of each channel and rank are already in every capture: boot0's stored parameter set has `dram_tpr10` bit 29 set, so every boot runs the save routine `0x576e4`, and parameter words 32–49 hold the receive Vref values and words 50 and 51 the mode registers, read with MR16 set to 5 (`0x57796`). MR12 is not saved; a capture variant would read it through `0x4b5ac` after `init_DRAM`.

## The hook wraps init_DRAM and writes the PHY registers inside it

boot0's P-state-0-only LPDDR5 path runs `init_DRAM` (`0x49bdc`): DRAM supply (`0x4b618`), clock and reset setup (`0x57a4c`), controller setup (`0x578a4`, `0x57238`, `0x570d4`), then `0x55990`: PHY setup (`0x55654`, `0x4f57c`), P-state 0 clocks (`0x4f58c`, `0x4f500`), message block (`0x59d00`, which reads the training-step value at `0x59d6a`), firmware load (`0x4bbb8`, `0x4b97c`), firmware run (`0x4f47c`), per-P-state timing (`0x4bca8`, called at `0x56142`) and the final PHY stage (`0x4f46c`), then post-training settings (`0x57310`, `0x4b7ec`, `0x561a0`). A firmware result other than 7 returns 0 from `0x55990` (`0x5612e`) and `main` then stops without a reset (`0x5e21e`), so every check and fallback lives around the `init_DRAM` call. The restore uses two patch sites:

- **Around `init_DRAM`** (the call at `0x5e216` on a card boot; the DDR wrapper on the FEL path). Before the call: compute the strap CRC over parameter words 0–31 as boot0 selected them, run every record check below, and only then edit the parameter block: clear `dram_tpr13` bit 2, set `dram_tpr10` bits 28:16 to `0x0001`, set `dram_tpr1` bit 31 and words 50–51 to the recorded MR14 and MR15 values (boot0's own path, `0x5a08e`). After the call: require a nonzero size equal to the record's, read the restore set back and run the quick check.
- **The call at `0x56142`**: clear `MicroContMuxSel`, write the restore set under its masks in record order, set `MicroContMuxSel` to 1 and call `0x4bca8` as boot0 would.

No path retries `init_DRAM` after a partial restore: the PHY state is unknown and retraining within one lifetime has not run on these boards.

## Pre-admission rejection trains fully; post-edit failure resets on the FEL path and stops on the card

| Check, before any parameter edit | Mismatch |
| --- | --- |
| Record magic, version, header CRC and payload CRC | Full training |
| Pinned boot0 checksum, a constant in the hook, equal to the record's | Full training |
| Chip ID, SID `0x03006200`–`0x0300620c` | Full training |
| Strap: selected set index and CRC of parameter words 0–31 | Full training |

Once restore changes parameters or PHY registers, readback, size and quick-check failures stop the attempt.

- **FEL path.** The wrapper reports the error through its SRAM mailbox and returns through the normal FEL swap. The host resets the board with WDT0 and runs the plain four-P-state wrapper once on the new FEL instance ([FEL runner](#the-fel-runner-falls-back-through-one-watchdog-reset)).
- **Card path.** The hook stops in SRAM. A warm reset after a card boot has no qualified recovery destination.

Runtime temperature admission is not implemented: the captures have unknown temperature and no qualified range. A default card image uses full training; the experimental image requires an explicit host build option.

## The record format

Little-endian, in one contiguous card extent.

| Offset | Field |
| --- | --- |
| 0 | Magic `AS73DTRN` |
| 8, 12 | Version 1; header size 128 |
| 16, 20 | Payload length; payload CRC-32 |
| 24 | Generation, incremented by every record write |
| 32 | Chip ID, four words |
| 48 | Pinned boot0 checksum (eGON word `0x0c`) |
| 52, 56 | Selected parameter-set index; CRC-32 of parameter words 0–31 |
| 60, 64 | Capture temperature in m°C; allowed difference in m°C |
| 68, 72 | Training-step value of the reference training; DRAM size in MiB |
| 76..99 | Section offsets and counts: register runs, masks, mode-register writes |
| 100..123 | Reserved, zero |
| 124 | Header CRC-32 over bytes 0–123 |

Registers are stored as runs: base address, count, then 16-bit values padded to 4 bytes, with a list of `(address, u16 mask, u16 zero)` for registers restored under a mask. Mode-register writes are `(channel, rank, register, value)` in write order, including MR16. At about 2 bytes per register, 64 KiB holds over 30,000 registers. On the card the header fills the unit's first sector and the payload starts at the second.

## The record has its own 4 MiB unit on the card

The unit 16,384–24,575 holds the slot B header, which [boot-slots.md](../boot-slots.md#card-layout) keeps in a 4 MiB unit apart from other writes, and the vendor's SD parameter sector 24,504 and tuning pattern 24,512–24,571. The record therefore gets one 4 MiB unit at or beyond sector 40,960, assigned by the boot-slot allocation record like the slot regions (`boot-slots.py allocate --training-record`). `tools/storage-update.py` takes the `training-record` owned region from that allocation. Its plan input is a plan from `tools/training-record.py plan` or `invalidate`, every write lies inside the region, a host backup of the region precedes the write session under [docs/goal.md](../goal.md) section 4, and the payload is written before the header, so an interrupted write leaves a header whose CRC fails and boot0 trains fully. Slot fallback is unaffected, because the record shares no unit with a slot header. Each card's backup must show the unit unallocated by partitions and zero or holding an earlier record.

## Card boots need a host-built boot0 variant

`tools/prepare-card-ddr.py` derives an optional image from the pinned 245,760-byte boot0, leaves the default microSD packager input unchanged, recomputes the eGON additive checksum and publishes the original and derived identities. ROM acceptance, secure-boot fuse state and physical boot are not established.

| Pinned boot0 address | Derived contract |
| --- | --- |
| `0x5db08` | Thumb function: `r0` sector, `r1 = 1`, `r2` SRAM destination. Returns zero on success; rejects any count other than one. |
| `0x5baa8` | Lower reader: controller, sector, count, destination in `r0..r3`; returns the completed count or zero. |
| `0x5b88c..0x5b896` | Transfers above 512 bytes enter the DMA branch, whose descriptor buffer is at untrained DRAM `0x42000000`. Single-sector reads select PIO. |
| `0x49ab4..0x49ade` | boot0 initializes storage and performs four single-sector reads into SRAM before `init_DRAM`. The initialized flag is at `0x83c14`. |
| `0x5da64` | Returns the boot storage selector; the hook admits only selector zero, SD0. |
| `0x4b372..0x4b37e` | A parameter slot's first word determines whether it is populated. Code in empty slots preserves those words as zero. |
| `0x47b1c..0x47b34` | Card entry disables translation, data cache and instruction cache and starts its stack at `0x8d000`. |
| `0x5e216`, `0x56142` | Calls to `init_DRAM` and per-P-state timing; the two redirected existing calls. The timer entry at `0x48d6c` is also patched. |

Raw instruction excerpts stay in `~/.cache/sunstoneos/evidence/dram-card-restore-20261026/`.

### The bootstrap loads a separately reserved helper

Only 1,024 bytes in parameter slots 8–15 (`0x476f4..0x47af4`) are available. A small Thumb bootstrap occupies them while leaving the first four bytes of every slot zero; branches skip those words. The selected set 5 at `0x47574` stays untouched. A separate timing trampoline calls boot0's original `0x4bca8` whenever restore mode is inactive, preserving all four-P-state training calls after a pre-admission rejection.

Each paired 4 MiB record unit reserves its first 49 sectors for the record and sectors 64–127 for a helper of at most 32 KiB. A dedicated helper plan writes the two helper extents through the backup-first storage sessions and writes no record header, payload, boot slot or partition data. The derived boot0 embeds the unit addresses, helper length and CRC, so a stale helper cannot execute merely because a record is valid. Helper and boot0 updates can temporarily force full training without invalidating either saved record.

The bootstrap uses the one-sector PIO interface to load the helper into `0x92000..0x9a000` and checks the complete helper CRC before entering it. A successful read with a bad helper CRC may try the other copy; a card-read error or deadline expiry stores an SRAM error and stops, without retrying storage or entering full training with uncertain controller state. The bootstrap saves `r0`, `r1`, callee-saved registers, the return address and the original stack pointer, and restores them before the one fallback `init_DRAM` call with its original two arguments.

### Restore reads have one full-width deadline

The vendor timer at `0x48d6c` divides only the low 32 bits of the 24 MHz counter, which wraps about every 179 seconds, and PIO refreshes its timeout after every word. Neither bounds a complete transfer. The optional image redirects the timer entry to a bootstrap shim with an active flag that starts at zero in loaded boot0 bytes, outside every slot's presence word. Inactive calls reproduce the original counter instruction and continue at `0x48d70`.

Before its first sector read, restore captures the full 64-bit counter. Every read shares a 240,000,000-tick deadline (ten seconds at 24 MHz), checked before and after each read and by each vendor timer call while a read is active. Expiry records an SRAM error and stops with interrupts masked. This bounds a blocking read by the remaining budget plus one finite poll iteration, assuming the counter advances and MMIO accesses return; it gives no protection from a stalled bus or stopped CPU. The wrapper requires the vendor zero-success result.

The focused call graph is `0x5db08` → `0x5baa8` → `0x5b9cc`/`0x5ba1c` → `0x5b32c`. PIO FIFO, command completion, data completion, controller-reset and error-path clock-reset waits (the last in `0x5d0d8`) all call `0x48d6c`. Before reading, admission checks the initialized SD0 controller, the 512-byte block size, a nonzero chunk limit, the pinned host function pointer, SD0 register and FIFO addresses, card capacity and 32-bit LBA arithmetic, so malformed metadata cannot select DMA or a zero-progress loop. The wrapper saves and clears the log-enable byte at `0x60670` during each read (both log functions, `0x498e4` and `0x4993c`, then return early) and restores it on success, because UART output has a separate unbounded wait.

The helper uses a private stack at `0x9b000..0x9f000`. Record A and B buffers occupy `0xa0000..0xa7000` and `0xa7000..0xae000`, each with room for a 24 KiB record and a padded sector. The card-boot stack, boot0 body and BSS, the mailbox at `0x86000` and ROM scratch intervals stay disjoint; the build rejects allocated sections outside those intervals.

### Admission, fallback and temperature limits

The helper checks both copies without editing parameters, using the fixed address and mask profile, the actual SID, the original firmware identity, the selected parameter CRC and the highest nonzero generation (A first on a tie), and applies only the selected record. The record names the original boot0 checksum; the derivative's checksum differs. The host producer pins every unchanged byte and the allowed call and timer patches. A configuration block at `0x47ac8` supplies the temperature policy, unit sectors, helper length and CRC, and deadline; the packager checks those bytes against the manifest and the helper artifact.

A missing, corrupt, stale, wrong-board or otherwise ineligible record calls the original `init_DRAM` once with the original parameters. After restore starts, a readback, size or sampled-memory failure stores its error in SRAM and stops. The default derived image falls back to full training because the records have no qualified temperature. `--experimental-unknown-temperature` enables the room-temperature restore profile, with false physical and thermal qualification fields in its manifest; it is an offline prototype, not permission to boot a board.

The host interface takes `--boot0`, `--restore-set`, `--allocation`, `--backup` and `--output`. The allocation must bind the ordinary backup's hash and capacity and contain two disjoint reserved record units. The output holds `boot0-card-restore.bin`, `card-ddr-helper.bin`, `manifest.json` and a `helper-plan/plan.json` accepted by `tools/storage-update.py plan`. `tools/prepare-microsd-boot.py --restore-variant manifest.json` selects the derivative while still requiring the pinned `--boot0` input. Helper updates occupy only their two reserved subextents, each with its own backup-first write session.

Read caps: the first valid helper uses 20 sectors and the two records 50 sectors (35,840 bytes); a bad first helper followed by a valid second uses 46,080 bytes; the format caps both helpers and both records at 226 sectors, 115,712 bytes, under one non-refreshable ten-second budget. Restore performs 879 masked PHY writes and up to 89 rewrites of other wide-memory halfwords, a readback pass and a separate 879-read final pass; the quick check writes and reads 262,392 bytes each way. The checks are `checks/card-ddr.py`, `checks/card-ddr-pio.py` and `checks/card-ddr-host.py` (results in `dram-training-restore-card-offline`).

## RAM-only implementation contract

The implementation admits only the fixed 899-entry set and its recorded P-state-0 captures. The builder binds the analysis file by SHA-256, checks the capture chip ID, training kind and stage tables, and uses the pinned boot0 parameter set 5 for the strap CRC. Capture words 32–51 are 32-bit values: the record encodes their 40 little-endian halfwords as a run at `0x83278`. Temperature is unknown (`INT32_MIN`, limit zero), so the profile is restricted to the supervised room-temperature cold-start experiment.

The wrapper keeps its entry and mailbox at `0x85000` and `0x86000`. The record occupies at most 24 KiB at `0x87000`, ending before the FEL shadow at `0x8e000`. The restore helper and its compiled address and mask allowlist occupy at most 32 KiB at `0x92000`; a private 16 KiB stack ends at `0x9f000`. These intervals lie in CPU-routed shared SRAM (`0x74000..0xf3fff`, [ROM analysis](../vendor-firmware/boot-rom-and-boot0.md#sram-stack-and-the-shared-sram-routing)). The host backs up and verifies every added upload interval before launch.

Admission checks both CRCs, the record geometry, the actual SID, the pinned boot0 checksum, the set index and the selected parameter CRC before editing parameters, and requires the complete compiled address and mask sequence, so a valid CRC cannot authorize another MMIO destination. It admits no separate mode-register writes: boot0 consumes MR14 and MR15 from words 50–51. The wrapper clears training bits 29:16, selects initialization-only step 1 and P-state 0, and enables boot0's saved-value path through `dram_tpr1` bit 31. The call at `0x56142` becomes a Thumb BLX to an ARM trampoline that preserves the original arguments, applies and checks every PHY write with `MicroContMuxSel` clear, restores the mux to 1, then calls boot0's `0x4bca8`.

The helper writes every entry, then reads every entry back in a separate pass. Two PHY memories store words wider than an APB access: the ACSM memory (`0x0a982000`–`0x0a983fff`, 64-bit words) and the PHY data memory (64 KiB from `0x0a9b0000`, 32-bit words). Each holds a halfword write until the word's highest halfword is written, and reads return the stored word until then. The helper therefore writes the whole word of each entry in those ranges in ascending halfword order and rewrites the halfwords that are not entries with their stored values (89 rewrites for this set); boot0's PIE and message-block loaders also write every halfword in ascending order.

A readback mismatch does not end the pass: mailbox words 11–13 keep the first mismatch's address, expected and read values, word 27 counts mismatches, and words 28–31 hold the second to fifth as profile index (upper half) and value read (lower half). A failed pass exits directly to the wrapper's saved SRAM context with error 7 and must not resume boot0. After a successful `init_DRAM`, the wrapper checks the size, reads every entry back a second time and runs the [quick check](#the-quick-check-covers-address-lines-and-sixteen-windows-below-4-gib); a mismatch in the second readback fails with error 15. Failures have distinct mailbox codes for record geometry, header CRC, payload CRC, chip ID, boot0 identity, strap, PHY readback at the hook, PHY readback after `init_DRAM`, size and memory comparison. The host writes no RAM page after a reported restore failure.

The second readback runs the PHY microcontroller clock. After the hook, boot0's final PHY stage (`0x4f46c`, here `0x5a210`) ends with `UcclkHclkEnables` = 2 (`0x5ae9a`) and `MicroContMuxSel` = 1, and then, for an LPDDR5 set, `0x57310` runs twice on these boards (`dram_tpr13` bit 6, `0x579ce`). It clears `MicroContMuxSel`, sets `UcclkHclkEnables` to 3, reads thirteen halfwords of the message block from `0x0a9b0026` to `0x0a9b004c` (including `0x0a9b002c` and `0x0a9b0030`), programs controller timing from their maxima, writes `UcclkHclkEnables` = 0 (`0x5749a`) and sets the mux to 1 (`0x574ac`). No other boot0 code after the hook writes the data memory or `UcclkHclkEnables` (`0x4bca8`, `0x5a210`, `0x4b7ec`, `0x561a0`, the size probe `0x4b74c` and the simple test `0x4b4cc` do not), and no code writes `MicroReset`, which stays 1, so the training firmware stays stalled while the clock runs. With the clock off the data memory reads 0 over APB. The second readback therefore sets `UcclkHclkEnables` to 3 after clearing the mux, as `0x57310` and the capture reader do, and writes back the value it read (0 on boot0's path) before setting the mux to 1.

The quick check finds gross errors only; the March C- qualification stays required. Host instruction checks exercise the real wrapper and helper with modeled boot0 calls and MMIO, require exact fault codes and zero later memory accesses for wrong-board, CRC, readback and quick-check failures, and establish software ordering only.

### The quick check covers address lines and sixteen windows below 4 GiB

`restore_check` in `boot/a733/ddr-restore.c` runs after the second readback pass with the MMU and caches off. In AArch32 it reaches DRAM only from `0x4000_0000` to `0xffff_ffff`, the first 3 GiB of the 12 GiB map. Both card and FEL restores use it.

- **Address lines 2–31.** It writes a distinct word at `0x4000_0000` and at `0x4000_0000 + 2^k` for k = 2..31, reads all 31 back, then repeats with the complements. A stuck or shorted address line makes two words share a location. For single-bit offsets this also tests the linear decode below 4 GiB.
- **Data.** Sixteen 4 KiB windows start at `0x4000_0000` and are 192 MiB apart; the last ends at `0xf400_0fff`. Each of four passes writes all windows before reading any: a rotating one (word i holds `1 << (i mod 32)`), its complement, the word address XOR `0xa55a5aa5`, and that value's complement.

The check touches 65,616 distinct bytes and writes and reads 262,392 bytes each way. A mismatch stores the bytes read so far, the address, and the expected and actual word in mailbox words 10–13. Mailbox words 25 and 26 hold the low word of the 24 MHz counter at the start and end of `restore_check`; the host reports the difference as the check time.

### The FEL runner falls back through one watchdog reset

`tools/run-ddr.py` with a restore build also takes `--fallback-build`, the plain four-P-state wrapper for the same boot0. The run has at most one WDT0 reset and one full training:

1. Before any launch it requires the reconnect access rule for the port, reads the SID over FEL and compares it with the record's chip ID. A mismatch skips the restore and runs the plain wrapper on the same instance.
2. It requires WDT0 disabled, then arms it: output 2, configuration 1 (system reset), mode `0x41` (enabled, four seconds) and restart word `0x14af`, all under key `0x16aa0000`, with readback.
3. One FEL request executes the wrapper and then writes mode `0x16aa0000`, so WDT0 is disabled as soon as the wrapper returns. The runner records the request's duration as `exe_seconds`.
4. A clean restore result, the expected size and the host's two RAM pages end the run with exit status 0.
5. Any other returned result arms WDT0 again. A hang, or a request that fails while WDT0 counts, needs no second arming; the runner sends nothing more to the old instance.
6. Within 20 s of arming, the runner adopts the new FEL instance on the same port. An enabled mode register shows that WDT0 fired. The runner disarms WDT0 and runs the plain wrapper once. Success gives exit status 3.

A missing new instance, an instance whose WDT0 was not enabled, a failed full training or any FEL transport failure stops the run and quarantines the current instance. The WDT0 register values match the launchers in [watchdog.md](../watchdog.md); the runner writes no RTC FEL flag or ROM redirect word.

### The wrapper returns CPU-L and the DSU to the ROM clock point

boot0's board setup leaves CPU-L at 1014 MHz (PLL `0xf8802700`, mux `0x03000000`) and the DSU at 780 MHz (`0xf8801e00`, `0x03000105`). The boot ROM sets both to 494 MHz (PLL `0xf8802600`, muxes `0x03010000` and `0x03010105`) when it is entered. The wrapper returns to the live FEL instance with `bx lr`, so at `finish` in `boot/a733/ddr-entry.S`, whenever the run reached board setup (mailbox word 1 at least 2), it writes the ROM words back. This covers the plain, restore and fallback runs. CPU-B keeps boot0's word, as it does after a ROM entry.

CPU0 runs on CPU-L while this code runs, so each domain follows the CPU clock driver's sequence (`drivers/clock/a733_cpu_clock.c`), timed with the 24 MHz counter:

1. Park the mux on the oscillator, read it back and wait 20 µs.
2. Write N = 38, then write it again with the update bit (bit 26) set.
3. Wait up to 10 ms for the update bit to clear.
4. Wait up to 70 ms for the lock bit (bit 28) on three consecutive reads at least 3 µs apart, then wait 20 µs.
5. Write the final mux with the source field clear (the divider alone), then the final mux, and wait 20 µs.

A domain that already holds the ROM words is left alone. Mailbox word 15 is 0 on success; otherwise it is `0x100` × domain (1 CPU-L, 3 DSU) plus a code: 1, the PLL is not enabled or its update bit is set (nothing is written); 2, update timeout; 3, lock timeout (both leave that mux on the oscillator); 4, the counter does not advance within `0x1000000` polls (the mux stays on the oscillator unless the stop comes during the last 20 µs wait); 5, the mux did not read back as parked (the PLL is not written). A CPU-L failure leaves the DSU untouched. In every failure case FEL still runs, at 24 MHz at worst. With the CPU on the oscillator the 16,777,216 polls take an estimated tens of seconds (1–3 µs per poll), longer than the four-second WDT0 interval of a restore run, so a stopped counter there ends in the watchdog reset.

`tools/run-ddr.py` refuses a build or fallback build whose manifest lacks `clock_point_word` 15, before any device access. After each run it reads, in the same FEL transaction as the mailbox, the PLL, pattern (+4, +8), LFM (+0x18) and mux words of the three domains, records them as `cpu_clocks` with word 15 as `clock_point_result`, and requires the words that the OS entry guard (`apps/os/cpu-return-entry.c`) requires: the ROM words for CPU-L and the DSU, `0xf8802700`/`0x03000000` for CPU-B, and pattern and LFM off. A plain or fallback run that does not meet them fails before the RAM-page checks; a restore run falls back through the watchdog reset, whose ROM entry sets CPU-L and the DSU again.

`checks/ddr-wrapper.py` runs the wrapper against a model of the PLL block (`checks/ddr_clock_model.py`) in which the old lock bit stays set until the update bit clears. It checks the order of writes, the waits and the final words, including a wrapping counter and a lock bit that drops once, a lock timeout, an update timeout, a disabled PLL, a mux that does not park, a domain already at the ROM point, and a stopped counter before the first wait, during the update wait and during the lock wait (with the poll limit lowered to 2,000).

### A diagnostic build records every entry around its write

`tools/prepare-ddr.py --variant restore --survey` builds the restore helper with `RESTORE_SURVEY` defined; without the flag the helper's bytes do not change. The diagnostic helper fills a survey at SRAM `0x96000`, inside the helper reservation above the helper's 9,912 bytes:

- **Header, 64 bytes.** Word 0 is `0x59565253` once the write pass starts, word 1 counts the entries recorded, word 2 holds `MicroContMuxSel` before the helper clears it, and words 3–6 hold `UcclkHclkEnables` (`0x0aa80100`), `MicroReset` (`0x0aaa0132`), `DctWriteProt` (`0x0aaa0062`) and `UctShadowRegs` (`0x0aaa0008`) at the hook. Word 7 counts the wide-memory halfwords rewritten with their stored values. Word 8 is 1 once the readback after `init_DRAM` starts; words 9 and 10 hold the `MicroContMuxSel` and `UcclkHclkEnables` values that pass found (1 and 0 on boot0's path).
- **Entries, 8 bytes each from `0x96040`.** For each entry in profile order: the value read before the write, the value read straight after it, the value the readback pass reads, and the mask.

For the 879 PHY entries the survey takes 7,096 bytes, ending at `0x97bb8`. `tools/run-ddr.py` uploads zeros over the survey with the helper and reads it back, so a helper that stops before its write pass leaves no earlier values. `tools/ddr-survey.py` puts each entry in one class, from the masked comparison with the record's value:

| Class | Values read | Meaning |
| --- | --- | --- |
| held | right straight after the write and in the readback pass | The register stores the value |
| already-at-value | as held, and the value before already matched | The write could not be observed |
| stored-later | wrong straight after the write, right in the readback pass | A wide-memory halfword stored with its word |
| changed-later | right straight after the write, wrong in the readback pass | The PHY or a later write changed it |
| unchanged | wrong in the readback pass and equal to the value before | The write did not reach a storage register |
| reads-zero | 0 in the readback pass, although the value before was not | A copy or status register the PHY clears |
| other-value | neither the value before nor the written value | The PHY produced the value |

The last four are mismatches. The runner writes every entry to `survey.json`, records the class counts, the mismatches by register family and the control words in `restore_result.survey`, and checks that the survey's mismatches equal mailbox word 27 after error 7 and 0 otherwise. A survey without its magic word, or with fewer entries than the profile, is an error and the run falls back as usual. `tools/ddr-survey.py SURVEY.BIN --record RECORD.BIN` decodes a saved survey offline.

## Two-copy host selection and standby update

The host tools reuse `boot-slots.py` allocations and `storage-update.py` backup-first sessions. The record bytes remain version 1, and single-unit allocations keep their behavior. The allocation field `training_records` assigns copies A and B to two separate 4 MiB units; it cannot coexist with `training_record`. Both units lie beyond the boot area, outside slots and partitions, and inside the backed-up image. Allocation reads the actual ordinary-file backup and records its capacity and hash; it does not establish a physical reservation.

`training-record.py select --allocation ALLOCATION --card-image IMAGE --record REFERENCE --restore-set ANALYSIS` reads each header before its bounded payload, checks both CRCs and the admitted RAM-restore profile (chip ID, boot0 identity, selected parameter CRC, temperature sentinel, size, register addresses and masks) and chooses the eligible copy with the highest 64-bit generation, A first on a tie; none eligible means `full-training`, before any parameter or PHY edit. The reference is a record from `from-capture` and the fixed analysis file is `restore-set-v4.json`. `plan-standby` takes the same arguments and `--output DIRECTORY`; its `plan.json` becomes the `--slot-plan` input to `storage-update.py plan`.

`plan-standby` targets the other copy, or A when neither is eligible. It refuses a valid record from another board, a repeated active payload and generation exhaustion, assigns the generation one above the eligible copies, writes the standby payload first and the header sector last, and binds the non-target copy's inspected header and payload hashes to the current card image. A changed source copy requires a fresh plan. Target ranges must equal their original bytes or the planned bytes; a torn range matching neither refuses conversion and needs a fresh backup and plan. A CRC-invalid standby copy cannot replace an eligible older copy, and preparing an identical update after the new header validates is refused so that it cannot overwrite the fallback. Plan ordering and CRCs do not establish card power-loss atomicity.

## Budgets

| Quantity | Budget | Basis |
| --- | --- | --- |
| DRAM interval with restore | 500 ms | 40 ms measured for the initialization-only path, 34.1 ms quick check measured, 879 writes |
| Helper and record reads | At most 115,712 bytes; one ten-second attempt deadline | Single-sector PIO; physical latency unmeasured |
| Record CRC | Bounded by the 24 KiB record limit | Instruction-checked |
| Quick check | 31 address-line words and sixteen 4 KiB windows below 4 GiB; 262,392 bytes each way | Measured on the board by mailbox words 25 and 26 |
| FEL fallback | One WDT0 reset (about 6 s) and one four-P-state training (about 5.1 s) | Only after a failed or hung restore |
| Hook code | 668 bytes in 1,024 bytes of slots; separate 10,120-byte helper | Eight zero presence words; reserved helper extents |
| Pre-admission fallback on a card | One full training in the same boot, about 5.1 s plus any successful reads | Read latency unmeasured |

## Open work

1. Run the full-map March C- test after P-state-0-only training and after restore under the [memory qualification contract](../memory-qualification.md); the quick check does not establish full-capacity correctness.
2. Qualify repeated restored lifetimes, cold and warm starts, stress and a temperature range. The records have no qualified temperature limit and the runner has no runtime temperature gate.
3. Establish physical fallback behavior for corrupt or wrong-board records, readback failures and sampled-memory failures.
4. Capture and qualify lower P-states before DRAM frequency scaling; restored boots use only the 2040 MHz P-state.
5. Deploy to the card only after FEL qualification, board-specific backups, physical record reservations, temperature admission and recovery qualification. Choose the RTC general-purpose register and reset contract if a Part B retrain flag is adopted (the proposal is in `dram-training-restore-qualification-plan`); decide on the modified boot0 for card boots and the record's own 4 MiB unit in place of the `bootpara` sectors.
6. Periodic retraining every N boots: the OS counts boots in its data partition, and when the count reaches N clears the record header through the backup-first path so the next boot trains fully and a new capture produces the record. N comes from drift data. Until a native backup-first writer is qualified, the coordinator does both steps from the host.

Registers outside the captured ranges, MR12 and other mode registers, record ageing and memory not covered by the quick check remain risks.
