# Design: recovering the microSD card from 1.8 V signalling without removing board power

Status:
- **Re-entry of a card at 1.8 V, SDR25 and SDR104 reads, and the close that returns the bank to 3.3 V: qualified on the board** for read-only lifetimes on port 3-3.
- **Session-start resolution algorithm: host-tested.** Only its 1.8 V branch has run on a board; the 3.3 V probe branch is covered by the model.
- **Card supply cycle through the PMIC switch DC1SW2: open.** No board run is approved, and the rail change needs the coordinator's separate approval.

A card that has switched to 1.8 V signalling keeps it until its supply is cycled ([UHS-I design](sd-uhs.md)). The board is powered from the USB cable and nobody handles it, so software must recover a card left at 1.8 V. The design answers three questions: which rail powers the card, whether a card at 1.8 V can be used without a power cycle, and what state each interrupted switch leaves.

## The card supply is not settled

The card's VDD is the net `VCC-CARD` through R51 (1 ohm). No load switch or GPIO gates it: the `card-pwr-gpios` property in the board tree is commented out and has no hardware behind it. The PMIC (AXP8191, address `0x36`) enables DC1SW2 with register `0x11` bit 4 ([PMIC runtime](../pmic-runtime.md)). The schematic and the vendor tree disagree on what DC1SW2 powers:

- Sheet 2 (power sequence) and the vendor device tree put `VCC-CARD` and `VCC-IO` on DC1SW2 and `VCC-PL` (the PMIC I2C pads) and the USB0 supply on ALDO1.
- Sheet 6 (the PMIC drawing) puts `VCC-CARD` on DCDC1 and `VCC-IO`, `VCC-PL`, the USB0 supply, `VCC-PD` and `VCC-PE` on DC1SW2. Its stub list also puts four rails that sheet 2 gives four different outputs on one net, so it is treated as unreliable, but nothing settles the question except the rail itself.

If sheet 2 is right, a DC1SW2 cycle resets the card and also drops `VCC-IO` (the PB bank, the FEL and JTAG-SEL pads), the USB2 host PHY, the 3.3 V header, the fan and the PF bank's 3.3 V supply. If sheet 6 is right, the cycle leaves the card powered and drops the PMIC link and the USB0 supply, so software could not re-enable the rail and the FEL link would be gone: the board would need power removal. The risk that the board does not return is the sheet 6 risk. The full consumer table, the hold-time estimate (30 to 50 uF on the switched net, 0.25 to 1.2 s to fall below 0.5 V, a 3 s hold to cover it) and the analysis of whether the board returns are in the record `sd-uhs-recovery-supply-analysis` in [the evidence file](../evidence/sd-uhs-recovery.jsonl). No cycle is run until the net is settled.

## A card at 1.8 V is re-initialised at 1.8 V

- **Specification** (SD Physical Layer 9.10, section 3.9.4): once the card has switched it continues 1.8 V signalling regardless of CMD0 and, after CMD0, returns to idle with SDR12 timing. CMD11 at 1.8 V is an illegal command and gets no response (section 4.2.4.4). Only a power cycle returns the card to 3.3 V.
- **Session start:** a new session needs the host level to match the card's. It moves the PF bank to 1.8 V (`GPIO_POW_VAL_SET_CTL` bit 0 = 0, with the clock stopped around the write as in the switch), then runs CMD0, CMD8, ACMD41, CMD2 and on, with no CMD11. Linux and the vendor driver treat a card already at 1.8 V as something the power-up cycle clears, which is a no-op on this board.
- **Level mismatch:** the card's pads are powered from the 3.3 V VDD and convert the signalling level, so a 3.3 V host level on a card at 1.8 V is within the card's pad rating. The opposite is unsafe: the PF bank at 1.8 V is rated 1.1 x 1.8 V on inputs (datasheet Table 5-3), and a card signalling 3.3 V that parses a command drives 3.3 V into it. A session therefore probes at 3.3 V first and goes to 1.8 V only after the card has not answered at 3.3 V.
- **ROM and FEL:** a watchdog return reaches FEL through the host's redirect and forced-FEL entry ([watchdog](../watchdog.md)). A card at 1.8 V can only make the ROM's media probe at 3.3 V fail, and a failed probe falls to FEL; boards returned to FEL with the card at 1.8 V in every measured case. Autonomous boot from the card is different: boot0 reads the package at 3.3 V, so after any reset while the card is at 1.8 V the card does not boot, and such an OS needs the supply cycle before an intentional reset.
- **Unknown:** whether a 3.3 V host reads a card that already signals 1.8 V at all. The card on port 3-3 answers ACMD41 at 1.8 V with S18A clear.

## States an interrupted switch leaves

| Where it stopped | Card state | Next session's path |
| --- | --- | --- |
| Before CMD11, or S18A clear | 3.3 V, idle or ready | CMD0 |
| CMD11 answered with no response | 3.3 V (the card did not start the switch) | CMD0 |
| R1 received, then the session ended or DAT was not seen low | holding CMD and DAT low, waiting for the clock to stop | lines read low: stop the clock, bank to 1.8 V, 10 ms, restart, probe at 1.8 V |
| Clock stopped, bank write failed or reset before it | switched to 1.8 V internally, host at 3.3 V | 3.3 V probe gets no answer or a wrong one: stop the clock, bank to 1.8 V, probe |
| Bank at 1.8 V, clock not restarted or DAT not high | switching or switched | gate again 10 ms, restart, probe at 1.8 V |
| Switched, SDR104 not selected (CMD6 busy or failed) | 1.8 V, SDR12 | continue at 1.8 V in SDR25 (outcome `NO_SDR104`) |
| No tuning window, CMD19 error, or a reset during tuning or reads | 1.8 V, SDR104 or SDR12 | CMD0 at 1.8 V (timing returns to SDR12), re-identify, tune again |
| Card that ignores CMD0 and commands at 1.8 V | 1.8 V, unresponsive | not recoverable by commands: the DC1SW2 cycle, or power removal |
| Card absent or hung at both levels | unknown | the DC1SW2 cycle, or power removal |

### The resolution algorithm

Every session that touches the card runs it ([adoption design](sd-uhs-adoption.md) gives the OS form).

1. Bank at 3.3 V; if a previous session left it at 1.8 V, gate the clock 10 ms and probe at 1.8 V, and on silence return to 3.3 V and continue. With the clock off, sample the lines: low means the card is mid-switch, so finish it as in the table.
2. Probe at 3.3 V: CMD0, CMD8, ACMD41 with S18R. S18A set: send CMD11; an R1 means a fresh switch; no response means the card already signals 1.8 V, so complete the level change (gate, bank to 1.8 V, restart) and continue without CMD11. S18A clear: operate at 3.3 V; a read at speed that fails escalates to the 1.8 V probe.
3. No answer at 3.3 V: gate, bank to 1.8 V, restart, probe at 1.8 V. An answer means the card is at 1.8 V. Silence means return the bank to 3.3 V and report `UNRESOLVED`, the only terminal outcome.

The platform's admission refuses a bank that does not read 3.3 V before any card command; the 1.8 V bank is admitted only for the probe in step 3 and the continuation after it. `checks/sd-uhs-recovery-model.py` runs the table and the algorithm over every cut of a session; the results are in the record `sd-uhs-recovery-model`.

## Implementation

The UHS-I lifetimes of the read-only probe (`apps/sd-backup-probe`, `--uhs-mode N`) and the platform provide these paths:

- **Switch lifetime** (mode 1): ACMD41 with S18R, CMD11, bank to 1.8 V, identification at 1.8 V, SDR25 only (`sunxi_mmc_sdr25_only`), reads, then the close.
- **Re-entry lifetime** (mode 2): the platform stage `ENTER_1V8` (`start_1v8`) writes the selector to 1.8 V once with the card clock stopped, waits for the bank and keeps the clock stopped 10 ms. The core then opens in re-entry mode (`sunxi_mmc_reenter`): CMD0, CMD8, ACMD41 with S18R, no CMD11, CMD2 onward.
- **SDR104 lifetime** (mode 3): re-entry, CMD6 to SDR104, the module clock at the fastest the platform verifies (PERI0_300M / 1 for 150 MHz, else PERI0_400M / 2 for 100 MHz), the CMD19 sweep, then the reads. The platform verifies the PERI0_300M gates and does not enable them.
- **Close:** card clock off, selector written back to 3.3 V once and the bank waited to 3.3 V (platform stages `CLOSE_BANK`, `CLOSE_WAIT`), then module, bus, pins, rail and clock released. The card stays at 1.8 V.
- **Selector rule:** the native register filter admits the selector write to 1.8 V once and the write back to 3.3 V once, only after the first, only with the controller's clock enable bits clear, only in the UHS-I profile, and only by the owning task; it stays read-only in every other profile and for eMMC (`checks/a733-mmc-native-filter.c`). The close restore of the inherited module setting is admitted for the high-speed and UHS-I profiles only with the module off and the bus in reset, by one pure function (`a733_mmc_platform_native_module_write_allowed`) that the adapter and the host models both apply.
- **Stop word:** a lifetime that cannot close cannot return its report, so the fault record's `esr` carries a stop word: the platform's fault line (10 bits), why (2), platform stage (5), S18A, the five speed-mode support bits, whether the card signals 1.8 V, and a fold of the 32 read CRCs; the site byte is the probe's reason. For a failure before identification it carries the core's error (4 bits), protocol step (6) and half the sample delay reached (5). `apps/sd-backup-probe/input.py stop --site N --esr W` decodes it.
- **Models:** `checks/sd-return-close.py --lifetimes` runs the lifetimes through the real platform, core and providers against a modeled card and controller, cutting after every poll, and a shadow adapter in `checks/sd-return-close.c` applies the native adapter's register filter to every register write. A model of a layer's callee must be driven through that layer ([storage status](../status/storage.md)).

## The tuning sweep follows the vendor loop

The CMD19 sweep (`sunxi_mmc.c`) matches the board kernel's `sunxi_mmc_execute_tuning` (`bsp/drivers/mmc/sunxi-mmc.c:2432-2640`): each of the 64 sample delays issues CMD19 up to 20 times and stops at the first error or pattern mismatch; the window is the first longest passing run of at least 12 delays, and the delay is its midpoint. Differences from the vendor loop, each kept for a stated reason:

- **Reset only after a failed command**, because a passing command leaves the controller idle. The reset is the `GCTRL` soft, FIFO and DMA reset followed by a rewrite and read-back of the SDR104 registers, not the vendor's CCU gate toggle (which belongs to the platform and needs a new platform operation).
- **The clock update after a reset masks DAT0**, as the vendor's `oclk_en(1)` does, because a card that still holds DAT0 low stalls an update that does not.
- **Low-power clock gating stays off**, because the vendor's own comment for this chip wants the clock always on to avoid a sample bug.
- **The CMD19 data timeout is 1.7 ms** (`TMOUT` `0x040000ff`), which bounds a command that gets no data.
- **No wait for the command or data state machine before the reset**, and no CMD12 or abort.
- **R1 is not judged.** A delay fails on any host command or data error, the pattern compare, or no completion within 10 ms (`TUNE_ATTEMPT_US`).

While a CMD19 is in flight any failure the attempt can produce fails that delay and never ends the sweep (`fault()`, while `tune_attempt` is set). Failures that stay terminal, through `fatal()`: a refused register access, a clock or deadline limit, lost ownership, card removal (interrupt bits 18 and 31), 12 consecutive hung attempts (`TUNE_HUNG_CAP`), 12 consecutive recoveries that do not complete (`TUNE_FORCED_RESETS`), and the recovery's own read-back checks. The sweep returns the no-window outcome (`NO_WINDOW`: clock to 50 MHz, CMD6 SDR25) when no run of 12 delays passes. The complete diff table, the terminal-path table and the host fuzz result (8,868 injected faults, 737 failures before the audit, none after) are in the record `sd-uhs-recovery-tuning-audit`.

## Qualified figures

All runs read only and write nothing to the card.

| Result | Value | Record |
| --- | --- | --- |
| 3-1 card (Phison SD64G, 2026) at 3.3 V: S18A, CMD6 group 1 support | S18A set; group 1 support `0x8003` (SDR104 support is not visible at 3.3 V, spec Table 4-11) | `storage.jsonl` `1793639891731890732-2700d966` |
| 3-3 card (Phison SD32G, 2020): switch, 32 SDR25 reads, close | switch and reads completed; the first close failed on the adapter's module restore, fixed | `storage.jsonl` `1793648082135831049-05f70d50` |
| 3-3 card re-entered at 1.8 V, SDR25 | S18A clear, group 1 support `0x801f`, 21.4 MB/s per request, bank 3.3 V after the close, normal return | `storage.jsonl` `1793649661736969334-9742cb48` |
| 3-3 SDR104 at 150 MHz, 64 MiB | window 16 to 63 (58 of 64 delays pass), delay 40; 47.6 MB/s per request median, 27.4 MB/s sustained; first 32 CRCs equal SDR25's | `storage.jsonl` `1793650532228649180-1fb49397` |
| 3-3 request-pattern experiment, eight phases | 20.4 MB/s over all phases; the card is slow in some regions and positions independent of bus speed and request order | `storage.jsonl` `1793652351260245353-286b63df` |

The 3-3 card stalls on its media in some regions (5 to 17 ms per 128 KiB request in 48 to 64 MiB and 200 MiB, none in 4 MiB). The stalls are the same at 50 and 150 MHz, are not caused by the request pattern or by a per-command cost, and depend on the card's position and history, so sustained SDR104 throughput on this card is about that of SDR25 on slow regions; the 47.6 MB/s per request is reached on fast regions.

## Limitations and open work

1. The 3.3 V probe branch of the resolution algorithm has not run on a board; it needs one read-only lifetime on 3-3 with the card at 1.8 V and the bank at 3.3 V (mode 0), which also settles whether a 3.3 V host reads a card at 1.8 V.
2. The same lifetimes on port 3-1, whose card has only been seen at 3.3 V.
3. The DC1SW2 cycle: settle the net the rail powers, then rehearse a 3 s hold at 3.3 V with the PF pins and clock low, re-enable, and test the card with CMD13 to its old RCA. It needs the user's decision and is required only for autonomous boot from a card that was switched.
4. A marker byte that outlives a watchdog reset and not a power removal would let a session skip the 3.3 V probe. The AXP8191 `BUFFER0` (`0x04`) holds the vendor boot flow's reboot mode and is not taken. The algorithm does not depend on a marker.
5. Cause of the position-dependent stalls: a descending or interleaved read order would separate position from seek history.

## Sources

- SD Physical Layer Simplified Specification 9.10 (`~/.cache/sunstoneos/sources/sd-uhs/sd-spec/spec.txt`): 3.9.4 (lines 2485-2535), 4.2.4 (3840-3935), 6.4.1.3 and 6.4.1.5 (15807-15835).
- Linux v6.12 MMC core (`sources/sd-uhs/linux-adc218676eef/drivers/mmc/core/core.c` 1150-1275, 1388; `sd.c` 850-880).
- Board kernel (`2ac08e8c`): `sun60i-a733-orangepi-zero3w.dts` (1623-1652, 2387-2398), `axp2101-regulator.c` (1368-1371), `bsp/drivers/mmc/sunxi-mmc.c` (1915-2090), `bsp/drivers/power/include/axp2101.h`.
- Schematic `sources/board-documents/schematic-v1.2.txt`: power tree (lines 130-175), sheet 12 (1510-1560). A733 datasheet `sources/A733-datasheet.txt`: pin domains (2535-2561), Table 5-3 (5870-5880).
- Vendor U-Boot `pmu_axp8191.c` and `.h` (`BUFFER0`, `0x55` power-down register).
- [The UHS-I design](sd-uhs.md), [the PMIC runtime provider](../pmic-runtime.md), [watchdog recovery](../watchdog.md).
