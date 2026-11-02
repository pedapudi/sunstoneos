# Design: recovering the microSD card from 1.8 V signalling without removing board power

Status: research and host model only. No board run is approved. The plan at the end needs the coordinator's approval, and
its one rail change (DC1SW2) needs it separately.

The OS reads the card at 21.7 MB/s in High Speed mode, which is the ceiling of 50 MHz on four lines (25 MB/s raw). SDR104 at
150 MHz would raise the raw limit to 75 MB/s, but [UHS-I](sd-uhs.md) needs a switch to 1.8 V signalling that the card keeps
until its supply is cycled. The board is powered from the USB cable and nobody can handle it, so a card left at 1.8 V is
a problem unless software can recover it. This note answers three questions from the sources.

## 1. The card supply can be switched in software, but it is shared

The card's VDD is the net `VCC-CARD`, fed through R51 (1 ohm) from the PMIC output DC1SW2 (schematic revision 1.2, sheet 12;
the power-tree sheet lists `VCC-IO / VCC-3V3 / VCC-SENSOR / VCC-eMMC / VCC-CARD / VCC-PH / VCC-USB-2 / VCC33-18-USB-2`
on DC-DC1->SW2, 3.3 V, 1 A). There is no load switch and no GPIO that gates the card: the socket has VDD, the five signals
and the detect switch, and C236/C237 (100 nF, 10 uF) at the pin. The vendor tree's commented `card-pwr-gpios` property
(`sun60i-a733-orangepi-zero3w.dts:1630`) has no hardware behind it on this board.

- **Rail and control:** the PMIC is the AXP8191 at address `0x36`. DC1SW2 is enabled by register `0x11` bit 4
  (`AXP8191_DCDC_CTL2`, `AXP_DESC_SW(AXP8191, DC1SW2, ...)` in the board kernel's `axp2101-regulator.c`; our provider reads
  the same bit, [pmic-runtime.md](../pmic-runtime.md)). Its output voltage is the shared DCDC1 selector at `0x12`.
- **Vendor `vmmc` handling:** the SDC0 node sets `vmmc-supply`, `vqmmc33sw-supply` and `vdmmc33sw-supply` to `reg_dc1sw2`
  (`dts:1646-1648`), and `reg_dc1sw2` is `regulator-always-on` (`dts:2397`). `sunxi_mmc_set_ios` calls
  `regulator_disable` on it for `MMC_POWER_OFF` (`sunxi-mmc.c:2078-2080`), and Linux's `mmc_power_cycle` is power off,
  at least 1 ms, power on (`core.c:1388-1393`). The always-on flag turns the disable into a no-op, and the fan, two TWI
  buses and the Ethernet node share the regulator (`dts:1531`, `1864`, `1942`, `3314`). The vendor kernel therefore
  never power-cycles the card on this board; it has the same limitation we have.
- **What else loses power:** SoC bank `VCC-IO` (PB0-PB10, the FEL and JTAG-SEL pads, WREQIN), `VCC-PH`, the USB2 host PHY
  supplies (`VCC-USB-2`, `VCC33-18-USB-2`), the 3.3 V header rail, the fan and eMMC. It does not touch USB0, the FEL
  path (`VCC33-USB` is on ALDO1), the PMIC's I2C (VCC-PL, ALDO1), the DRAM, the CPU rails or the PF bank's 1.8 V supply
  (CLDO5). The datasheet's power-down figure (5.10.2, an image) must be read before relying on `VCC-IO` going down alone;
  the text says nothing about it.
- **Requirement to meet:** the specification wants VDD below 0.5 V for at least 1 ms, and DAT, CMD and CLK driven low or
  disconnected meanwhile (section 6.4.1.3 and 6.4.1.5). Nothing on the board discharges the rail actively that we have
  found, so the hold time is set by the loads and is unmeasured. A hold is proven long enough only by the card resetting
  (below).

So yes: a software power cycle of the card exists (DC1SW2 off, wait, on), at the cost of dropping the rail the other
consumers share. That is a rail change and needs the coordinator's approval. It is the fallback, not the first plan.

## 2. A card at 1.8 V can be re-initialised at 1.8 V

- **Specification:** once the card has switched, it "continues 1.8V signaling regardless of CMD0"; after CMD0 "card returns
  to Idle state but still work with SDR12 timing" (3.9.4, `spec.txt:2488` and `2521`). CMD11 at 1.8 V is an illegal command
  and gets no response (4.2.4.4, case 4). Only a power cycle returns the card to 3.3 V.
- **So a new session needs only the host's level to match the card's:** move the PF bank to 1.8 V
  (`GPIO_POW_VAL_SET_CTL` bit 0 = 0, the selector the driver already writes; the clock stopped around the write as in the
  switch), then CMD0, CMD8, ACMD41, CMD2 and on, with no CMD11.
- **Linux and the vendor driver do not do this.** Both treat a CMD11 failure as "power cycle the card" (`core.c:1260-1265`)
  and a card already at 1.8 V at start as something the power-up cycle clears. On platforms where that cycle is a no-op
  (this board) the card stays unreadable at 3.3 V until power is removed. There is no S18A retry at 1.8 V in either.
- **Level mismatch:** the card's pads are powered from the 3.3 V VDD and convert the signalling level, so a 3.3 V host
  level on a card at 1.8 V is within the card's pad rating. The opposite is not safe: the PF bank at 1.8 V is rated for
  1.1 x 1.8 V on inputs (datasheet Table 5-3), and a card signalling 3.3 V that parses a command drives 3.3 V into it. The
  session start therefore always probes at 3.3 V first and goes to 1.8 V only after the card has not answered at 3.3 V.
- **ROM and FEL:** a watchdog return reaches FEL through the host's redirect and forced-FEL entry
  ([watchdog.md](../watchdog.md), `boot-rom-and-boot0.md`), and a USB reconnect with a card inserted has reached FEL. The
  card at 1.8 V can only make the ROM's media probe at 3.3 V fail, and a failed probe falls to FEL. That is inferred from
  the dispatch, not measured with a 1.8 V card. A card that ignores the ROM is harmless for our FEL boot. It is not
  harmless for autonomous boot from the card: boot0 reads the package at 3.3 V, so after any reset while the card is
  at 1.8 V the card does not boot. An OS that leaves the card at 1.8 V needs the supply cycle before an intentional reset.
- **Unknown, and modelled both ways:** what a card that already signals 1.8 V answers to ACMD41 with S18R (the table in the
  specification is blank in the simplified edition), and whether a 3.3 V host reads such a card at all.

## 3. States a failed switch leaves, and the software path out of each

| Where it stopped | Card state | Next session's path |
| --- | --- | --- |
| Before CMD11, or S18A clear | 3.3 V, idle or ready | CMD0 |
| CMD11 answered no response | 3.3 V (the card did not start the switch) | CMD0 |
| R1 received, then the session ended or DAT was not seen low | holding CMD and DAT low, waiting for the clock to stop | lines read low: stop the clock, bank to 1.8 V, 10 ms, restart, then probe at 1.8 V |
| Clock stopped, bank write failed or reset before it | switched to 1.8 V internally, host at 3.3 V (lifetime B) | 3.3 V probe gets no answer or a wrong one: stop the clock, bank to 1.8 V, probe |
| Bank at 1.8 V, clock not restarted or DAT not high | switching or switched | gate again 10 ms, restart, probe at 1.8 V |
| Switched, SDR104 not selected (CMD6 busy or failed) | 1.8 V, SDR12 | continue at 1.8 V in SDR25 (existing `NO_SDR104` outcome) |
| No tuning window, or CMD19 error, or a reset during tuning or reads | 1.8 V, SDR104 or SDR12 | CMD0 at 1.8 V (timing returns to SDR12), re-identify, tune again |
| Card that ignores CMD0 and commands at 1.8 V | 1.8 V, unresponsive | not recoverable by commands: the DC1SW2 cycle, or power removal |
| Card absent or hung at both levels | unknown | the DC1SW2 cycle, or power removal |

`checks/sd-uhs-recovery-model.py` runs this table. It cuts a session after every step of the switch lifetime (CMD0
through the reads), for both unknowns above, for a reset that restores the bank to 3.3 V and one that does not, and for a
session that ends with the bank restored or left at 1.8 V, and starts the resolution algorithm below each time. All 256
sessions end usable at the card's own level, with no command sent at 1.8 V to a card signalling 3.3 V. A card without
UHS-I stays at 3.3 V. A stuck card is left unresolved by the algorithm and cleared by the supply-cycle model once the
hold exceeds the discharge time, as expected. Replacing the 1.8 V probe or probing at 1.8 V first makes the check fail.

### The resolution algorithm (every session that touches the card)

1. Bank at 3.3 V (or, if a previous session left it at 1.8 V, gate the clock 10 ms and probe at 1.8 V; on silence return
   to 3.3 V and continue). Clock off, lines sampled: low means the card is mid-switch, so finish it as in the table.
2. Probe at 3.3 V: CMD0, CMD8, ACMD41 with S18R. S18A set: send CMD11; an R1 means a fresh switch; no response means the
   card already signals 1.8 V, so complete the level change (gate, bank to 1.8 V, restart) and continue without CMD11.
   S18A clear: operate at 3.3 V; a read at speed that fails escalates to the 1.8 V probe.
3. No answer at 3.3 V: gate, bank to 1.8 V, restart, probe at 1.8 V. An answer means the card is at 1.8 V. Silence means
   return the bank to 3.3 V and report `UNRESOLVED`, the only terminal outcome.

The existing admission refuses a bank that does not read 3.3 V before any card command. The path needs a narrowly scoped
exception: the 1.8 V bank is admitted only for the probe in step 3 and the continuation after it.

## Board plan

Each stage has one hypothesis, every failure branch ends in a state the next stage's algorithm resolves, and nothing runs
without the coordinator's approval. Stages 1 and 2 are read-only; there is no card write anywhere.

1. **Capability probe at 3.3 V (no state change).** ACMD41 with S18R, read S18A and the CMD6 check-mode support bits for
   group 1 (SDR50, SDR104, DDR50) and group 4, without CMD11. Hypothesis: the 62.5 GB Class 10 / U3 card reports S18A and SDR104.
   Failure ends at CMD0 in a 3.3 V card, as every High Speed lifetime does today. If S18A is clear, stop: there is nothing
   to gain.
2. **Supply-cycle rehearsal at 3.3 V (rail change: AXP8191 register `0x11` bit 4, DC1SW2 off then on).** Identify the card
   to the transfer state with an RCA, drive PF pins and clock low, hold DC1SW2 off for 300 ms, enable it, wait for the
   PMIC read-back, then CMD13 to the old RCA. A timeout and a clean CMD0 and ACMD41 mean the card lost power, which
   proves the hold; an answer means it did not, so repeat with a doubled hold (600 ms, 1.2 s) in later runs. The check
   also shows that USB0/FEL, the PMIC link and the runner survive the rail drop. Failure branches: DC1SW2 does not
   come back (retry the write; a watchdog reset leaves the rail as the PMIC holds it, and the runner's normalisation
   step re-enables it before the next run; the worst case needs power removal, which is why this stage is first and
   gets its own approval); the card does not reset (card still at 3.3 V, recoverable).
3. **The switch with in-session recovery, then the cycle.** Resolution algorithm, CMD11, SDR25 reads, SDR104 tuning and reads.
   Then, still in the session, CMD0 at 1.8 V, CMD8, ACMD41, CMD2 onward, SDR104 and a read, which proves re-entry on this
   card. Then the stage-2 cycle with the proven hold, and a CMD0 and ACMD41 at 3.3 V, which proves the card is back
   at 3.3 V. Failure branches by where the session ends: every row of the table above resolves in the next session by the
   algorithm; if re-entry at 1.8 V fails, the same session runs the cycle, and if the cycle fails the board needs power
   removal. That last branch needs two independent failures (a card that violates the CMD0 rule and a hold that does not
   reset it), and stage 2 has retired the second.
4. **Adopt.** Only after stage 3: the OS runs SDR104 by default, restores 3.3 V before any intentional reset, and every session
   starts with the resolution algorithm. The read-only `ss_storage_open_read` path gets it first; the write path follows
   with its own admission.

Marker for the card's level: a byte that outlives a watchdog reset and not a power removal would let a session skip step 2
of the algorithm. The AXP8191's `BUFFER0` (`0x04`) is used by the vendor boot flow for the reboot mode, so it is not taken
without a decision. The algorithm does not depend on a marker.

## Sources

- SD Physical Layer Simplified Specification 9.10, `~/.cache/sunstoneos/sources/sd-uhs/sd-spec/spec.txt`: 3.9.4 (lines
  2485-2535), 4.2.4 (3840-3935), 6.4.1.3 and 6.4.1.5 (15807-15835).
- Linux v6.12 MMC core, `sources/sd-uhs/linux-adc218676eef/drivers/mmc/core/core.c` (1150-1275, 1388), `sd.c` (850-880).
- Board kernel (`2ac08e8c`): `sources/board-kernel/sun60i-a733-orangepi-zero3w.dts` (1623-1652, 2387-2398),
  `axp2101-regulator.c` (1368-1371), `bsp/drivers/mmc/sunxi-mmc.c` (1915-2090), `bsp/drivers/power/include/axp2101.h`.
- Schematic `sources/board-documents/schematic-v1.2.txt`: power tree (lines 130-175) and sheet 12 (1510-1560).
- A733 datasheet `sources/A733-datasheet.txt`: pin domains (2535-2561), Table 5-3 (5870-5880), 5.10.
- Vendor U-Boot `pmu_axp8191.c` and `.h` (`BUFFER0`, `0x55` power-down register).
- [The UHS-I design](sd-uhs.md), [the PMIC runtime provider](../pmic-runtime.md), [watchdog recovery](../watchdog.md).
