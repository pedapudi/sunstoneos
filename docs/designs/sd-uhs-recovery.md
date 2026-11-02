# Design: recovering the microSD card from 1.8 V signalling without removing board power

Status: research and host model only. No board run is approved. The plan at the end needs the coordinator's approval, and
its one rail change (DC1SW2) needs it separately.

The OS reads the card at 21.7 MB/s in High Speed mode, which is the ceiling of 50 MHz on four lines (25 MB/s raw). SDR104 at
150 MHz would raise the raw limit to 75 MB/s, but [UHS-I](sd-uhs.md) needs a switch to 1.8 V signalling that the card keeps
until its supply is cycled. The board is powered from the USB cable and nobody can handle it, so a card left at 1.8 V is
a problem unless software can recover it. This note answers three questions from the sources.

## 1. The card supply: a software switch may exist, and the documents disagree on what it powers

**The board documents conflict about which PMIC output feeds `VCC-CARD`, and about what else DC1SW2 powers. No cycle of DC1SW2
should be run until that is settled, because one reading makes the cycle useless and the other makes it unrecoverable.**

The card's VDD is the net `VCC-CARD` through R51 (1 ohm), with C236 and C237 (100 nF, 10 uF) at the socket (sheet 12). There is
no load switch and no GPIO that gates it: the commented `card-pwr-gpios` property (`sun60i-a733-orangepi-zero3w.dts:1630`) has
no hardware behind it. The PMIC (AXP8191, address `0x36`; the drawing calls it AXP318) enables DC1SW2 with register `0x11` bit 4
(`AXP8191_DCDC_CTL2`; [pmic-runtime.md](../pmic-runtime.md)); its voltage is the shared DCDC1 selector at `0x12`. DC1SW2 is
enabled now (`0x11` reads `0x17`; DC1SW1 is off).

Three sources, read for one question, which net each rail name is on:

| Source | `VCC-CARD` | `VCC-IO` | `VCC-PL` (PMIC I2C pads) | USB0 supply `VCC33-USB` | `VCC-PE` |
| --- | --- | --- | --- | --- | --- |
| Sheet 2, power sequence | DC1SW2 | DC1SW2 | ALDO1 | ALDO1 | BLDO1 |
| Vendor device tree | DC1SW2 (`vmmc-supply`, `dts:1646`) | not stated | not stated | not stated | BLDO1 (`dts:3628`) |
| Sheet 6, the PMIC drawing | **DCDC1 itself** (alias stub under `DCDC1`) | DC1SW2 (`SWOUT2` stub) | **DC1SW2** | **DC1SW2** | DC1SW2 |

Sheet 6 lists the rail names on each PMIC output as stubs. Its `SWOUT2` group holds `VCC33-18-USB`, `VCC33-USB`,
`VCC33-18-USB-2`, `VCC33-USB-2`, `VCC-PD`, `VCC-PL`, `VCC-IO` and `VCC-PE`; its `DCDC1` group holds `VCC-3V3-WIFI`,
`VCC_3V3_EXT`, `VCC_3V3_CSI`, `VCC-CARD`, `VCC_3V3_EMMC` and `VCC-LCD`. Sheet 2 and the vendor tree agree with each other and
not with that drawing. The stub list on sheet 6 also contradicts itself: it puts PD, PL, IO and PE, which sheet 2 gives four
different outputs (BLDO2, ALDO1, DC1SW2, BLDO1, of which two are 1.8 V and two 3.3 V), on one net. An earlier note
([pcie.md](../pcie.md)) read the same stub group as BLDO1. I read sheet 6 as unreliable for this question and sheets 2 and the
vendor tree as likely, but nothing in the documents settles it, and the only physical check is the rail itself.

What each reading means for a DC1SW2 cycle:

- **Sheet 2 and the vendor tree are right:** DC1SW2 powers the card, so a cycle can reset it. It also drops `VCC-IO` (the PB bank, the
  FEL and JTAG-SEL pads, WREQIN), `VCC-PH`, the USB2 host PHY supplies, the 3.3 V header rail, the fan and eMMC.
- **Sheet 6 is right:** the card is on DCDC1 and does not lose power; the cycle does nothing for the card, and it drops the
  supply of the PMIC's own I2C pads (`VCC-PL`: the S_TWI0 pins PL0 and PL1), the USB0 and USB2 analog supplies, and `VCC-IO`, `VCC-PD`
  and `VCC-PE`. With the I2C pads unpowered the software cannot re-enable the rail, and with USB0 unpowered the FEL link is
  gone, so the board would need power removal.

### What each consumer does when DC1SW2 drops

| Consumer (net) | What it is on this board | Effect of the drop | Source |
| --- | --- | --- | --- |
| `VCC-CARD`, the socket | the microSD card | card loses VDD; resets once VDD is below 0.5 V for 1 ms | sheets 2, 12; vendor tree |
| SoC `VCC-IO`: PB0-PB10 | UART0 on PB9 and PB10 (the console header), I2S0, TWI0, TWI1 on PB pins | pins lose their supply; UART0 and the header GPIOs go dead; the runner does not use them (FEL is over USB0) | datasheet pin table 2311-2343; sheet 9 |
| SoC `VCC-IO`: FEL pad (1W1), JTAG-SEL (1W2), WREQIN (1T24) | boot strap inputs with pull-ups from `VCC-IO` | with `VCC-IO` off the FEL pad no longer reads its pulled-up value; the ROM selects FEL on four low samples, so a reset during the off time is benign | datasheet 3305-3307, 2280; `boot-rom-and-boot0.md` |
| SoC `VCC-IO`: PF0-PF6 | SDC0 and card detect, supplied from `VCC-IO` or `VCC18-PF` by the adaptive selector | the bank follows CLDO5 (1.8 V) while `VCC-IO` is off, so the card's pins see 1.8 V logic while its supply is off | datasheet 2535-2561; sd-uhs.md |
| SoC `VCC-PH` | PH0-PH16, including `BOARD-ID-SEL-GPIO` (PH15) and `DDR-PARA-SEL-GPIO` (PH16) | pins unpowered; only boot0 reads the two straps, not our FEL path | sheet 9 |
| USB2 PHY (`VCC-USB-2`, `VCC33-18-USB-2`) | the USB host port (webcam, serial, radio when on USB) | the host link drops and the devices re-enumerate on return; the OS's USB2 task must be idle | sheets 2, 6 |
| 3.3 V header, `VCC-SENSOR`, `VCC-3V3` | the 40-pin header supply and the SoC sensor supply (also fed by CLDO5 per sheet 2) | header devices lose power | sheet 2 |
| Fan (`fan-supply`, PWM on PD5) | cooling fan | stops; the thermal controller must not rely on it during the hold | `dts:1531` |
| `VCC-eMMC` | no eMMC fitted | none | handoff |
| TWI pull-ups (`twi-supply`), GMAC 3.3 V | TWI5 and TWI12 nodes (disabled) and GMAC0 (disabled) | none | `dts:1864`, `1942`, `3314` |
| Under sheet 6 only: `VCC-PL`, `VCC33-USB`, `VCC-PD`, `VCC-PE` | PMIC I2C pads, USB0 analog, PD and PE banks | PMIC link and FEL link lost | sheet 6 |

The PMIC interrupt (`PMU-IRQ`) and the PMIC bus (`PMU-SDA`, `PMU-SCK`) are on PL pins, so their safety depends entirely on which
output `VCC-PL` is on (ALDO1 by sheet 2, DC1SW2 by sheet 6). The watchdog is inside the SoC and does not use a pad. The runner
uses USB0 only.

### Whether the board comes back

- **Hold and re-enable need the PMIC link.** The cycle writes register `0x11` over S_TWI0 on PL0 and PL1. If `VCC-PL` is on
  ALDO1 the link is untouched and the re-enable is a normal write. If it is on DC1SW2 the write cannot reach the PMIC.
- **A reset during the hold.** The PMIC keeps its rail registers across a watchdog reset (the CPU rails do), so DC1SW2 would stay
  off. The ROM then runs its FEL sample on a pad whose supply is off (reads as FEL), and the FEL path needs only USB0, which is on
  ALDO1 by sheet 2 and on DC1SW2 by sheet 6. Under sheet 2 the runner reaches FEL and the next image re-enables the rail (the
  rail normalisation would need the DC1SW2 bit added); under sheet 6 it does not.
- **Does anything in the boot or FEL path depend on `VCC-IO`?** The FEL pad and the PF bank (the ROM's SD probe) do, and both fail
  safe (FEL on a low pad, a failed probe falls to FEL). DRAM, the CPU rails and the PMIC link do not, by sheet 2.
- **Verdict.** The risk that the board does not return is the sheet 6 risk, and the documents cannot exclude it.

### How long the hold must be

The hold is set by the capacitance on the switched net and the loads that discharge it. From the schematic, on the sheet 2
reading: the card side has 10.1 uF at the socket; the SoC's VCC-IO, VCC-PH, VCC-PD, VCC-PE and VCC-PL pins each have 100 nF to
1 uF and one 10 uF part (C183 to C197, about 12 uF); the USB supply pins have four pairs of 1 uF and 100 nF (about 5 uF); the
header, sensor and fan supplies are not drawn with values. The total is on the order of 30 to 50 uF. No active discharge is
known: the PMIC's switch pull-down bit is not in the sources.

With no pull-down the decay is through the loads. The card's own idle current is the largest known load (about 0.2 to 1 mA at
3.3 V, falling as VDD falls), so an effective resistance of 3 to 16 kohm; with 40 uF that is a time constant of 0.13 to 0.65 s,
and falling from 3.3 V to 0.5 V takes 1.9 time constants: 0.25 to 1.2 s. Leakage from the SoC pins, the PHYs and the
sensor supply only slows the tail. So **300 ms is below the likely time and gives no margin; a hold of 3 s would cover the
whole range, and is checked by the card resetting, not assumed.** A figure measured on the board would replace this estimate;
nothing here measures it.

## 1b. A smaller test first

The rail cycle answers one question, how to return a card that is stuck at 1.8 V. It is needed only if the card cannot be
re-initialised at 1.8 V. That is the question to ask first, and it needs no rail change:

1. **Stage 1, done** (below): S18A is set, so the switch is available.
2. **Re-entry at 1.8 V, with no cycle.** One lifetime on one board: the normal switch, SDR25 reads at 1.8 V, then CMD0 and
   a full re-initialisation at 1.8 V (CMD8, ACMD41, CMD2 to CMD7, ACMD6, CMD6, a read), SDR104 if the card reports it. The
   lifetime ends with a normal CPU-local return, which enters forced FEL and never runs the ROM's storage probe. The
   run leaves the card at 1.8 V, which is the state under test.
   - **If the card re-initialises** the algorithm of section 3 is proved on this card, nothing in the OS needs power removal, and the
     supply cycle is never needed for reading. It is still needed for autonomous boot from the card (below).
   - **If it does not,** the card stays at 1.8 V until power removal. The cost is that this board's SD is unusable until the user
     can replug; FEL, DRAM and the CPU work are unaffected, because a normal return does not touch the card and the only ROM path
     that probes the card is a watchdog reset. Running it first on the board whose card matters less (port 3-3, 32 GB, quarantined
     today) limits that cost to that board.
3. **Only then** consider the DC1SW2 cycle, and only after the net it powers is established (section 1), because an experiment
   that settles the net is itself the cycle. The decision is the user's, with the two readings and their outcomes above.

A card at 1.8 V and the ROM: a watchdog reset runs the ROM's SMHC0 probe at 3.3 V against it. The probe can only fail and fall to
FEL, as it does with no card, but that is inferred from the dispatch and not measured with a 1.8 V card. Any lifetime that
leaves the card at 1.8 V must therefore end in a normal return and avoid a deliberate watchdog reset afterwards.

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

## Stage 1 prediction (written before the run)

The probe (`apps/sd-backup-probe`, now with `observe_s18`) runs the identification in profile 2 as before, with S18R in every
ACMD41 and no CMD11, and reports the CMD6 response of the high-speed switch. One run on port 3-1, no card write, the card stays at
3.3 V. Predicted from the card's earlier identification (CID `2750485344363447606832ea2f01a61d`, OCR `0xc0ff8000`, SCR `0x83848502`,
Class 10, U3, V30, A1, 4 MiB allocation unit):

- **S18A set:** OCR `0xc1ff8000` (bit 24 added). A U3 card must be UHS-I capable.
- **Group 1 support bits:** `0x001f`, that is SDR12, SDR25, SDR50, SDR104 and DDR50. SDR104 is the bit that matters; DDR50 is the
  least certain. The function selected by the switch is 1 (high speed).
- **Identity:** manufacturer id `0x27` (Phison), OEM `PH`, product `SD64G`, revision 6.0, serial `6832ea2f`, made June 2026.
- **Allocation unit:** 8,192 sectors (4 MiB), and no restoration word changed.

What would refute it: S18A clear (the card then has nothing to gain from the switch), SDR104 absent, or any change in the
reported identity.

## Stage 1 result

Entry `1793639891731890732-2700d966`, port 3-1, image `01b10763` (the probe built from this branch), returned normally: 1 sector
read, no restoration word changed, card clock stopped, PMIC parked. The queue marks it `result-mismatch` because its text
expectation is the older probe's; the report is complete and decodes.

| Item | Predicted | Measured |
| --- | --- | --- |
| S18A (OCR bit 24, ACMD41 with S18R, no CMD11) | set | **set**; OCR `0xc1ff8000` (CCS set) |
| CMD6 group 1 support field | `0x001f` | **`0x8003`**: functions 0 and 1 only (SDR12 and SDR25), bit 15 is the marker |
| Selected function | 1 | 1 |
| Manufacturer, OEM, product, revision, serial, date | `0x27`, `PH`, `SD64G`, 6.0, `6832ea2f`, 2026-06 | the same; CID `2750485344363447606832ea2f01a61d` |
| Allocation unit | 8,192 sectors | 8,192 (`AU_SIZE` and `UHS_AU_SIZE`) |
| Restoration words | unchanged | unchanged |

The group 1 prediction was wrong, and the specification says why: "If the card is initialized in 3.3V signal level, Default Speed
and High Speed are assigned to function 0 and 1. Then support bits of function 2 to 4 (SDR50, SDR104 and DDR50) are set to 0"
(Table 4-11 text, `spec.txt:5634`). SDR104 support is therefore not visible at 3.3 V and is established only after the switch.
What the card does show: it accepts the switch (S18A), it is a Phison `SD64G`, SCR says SD 4.xx or later, SD Status says speed
class 10, UHS speed grade 3 (U3), video class 30, 4 MiB allocation unit, and CMD6 reports 50 mA maximum. A U3 card must support
UHS-I, but whether it supports SDR104 rather than SDR50 stays open until a lifetime at 1.8 V reads the field there.

The decoder (`apps/sd-backup-probe/input.py`) now says so: `bus_speed_support` is meaningful only at 1.8 V.

## The re-entry test: two lifetimes on port 3-3 (approved, read-only, no rail change)

Both lifetimes run the one probe image (`apps/sd-backup-probe`, `--uhs-mode 1` and `--uhs-mode 2`) and write nothing to the card. Each
reads 32 requests of 128 KiB from sector 0 (4 MiB) at SDR25, 50 MHz, so the two lifetimes' read CRCs compare, and ends with a normal
CPU-local return. They run back to back; lifetime 2 is sent only when lifetime 1 has returned normally and its report shows the card
signalling 1.8 V.

What the image does, and what the host models check:

- **Lifetime 1 (`--uhs-mode 1`).** The UHS-I profile: ACMD41 with S18R, CMD11, the bank to 1.8 V, identification at 1.8 V, SDR25 and no
  SDR104 or tuning (`sunxi_mmc_sdr25_only`), reads, then the close: the card clock off, the selector written back to 3.3 V once and
  the bank waited to 3.3 V (new platform stage `CLOSE_BANK`), then the module, bus, pins, rail and clock released as before. The card
  is left at 1.8 V.
- **Lifetime 2 (`--uhs-mode 2`).** The bank-level entry (`start_1v8`, platform stage `ENTER_1V8`): with the card clock stopped, the
  selector written to 1.8 V once, the bank waited to 1.8 V and the clock kept stopped 10 ms; then the core opens in re-entry
  mode (`sunxi_mmc_reenter`): CMD0, CMD8, ACMD41 with S18R, no CMD11 (the card would answer it with nothing), CMD2 onward,
  SDR25, reads, the same close. This is the 1.8 V branch of the resolution algorithm; its 3.3 V probe is covered by the model, not
  exercised on the board, because lifetime 1 has told us where the card is.
- **The selector rule.** The native register filter admits the selector write to 1.8 V once and the write back to 3.3 V once, only
  after the first, only with the controller's clock enable bits clear, only in the UHS-I profile, and only by the owning task
  (`checks/a733-mmc-native-filter.c`). It stays read-only in every other profile and for eMMC.
- **Models.** `checks/sd-return-close.py --lifetimes` runs both lifetimes through the real platform, core and shared providers against a
  modeled card and controller, and cuts each after every poll (605 cuts): at every cut no command went out with the bank at 1.8 V and the
  card at 3.3 V, the bank selector, the platform's record and the card's level agree, and at most one write to each level was made. A
  mutation that skips the restore write, and one that sends CMD11 in re-entry, each fail it. `checks/sunxi-mmc-uhs.py` covers the core
  options (both card answers to S18R, the SDR104 path after re-entry, the refusals). `checks/sd-uhs-recovery-model.py` now has the close
  as its last two steps: all 144 sessions (every cut x both unknowns x both reset behaviours) end usable.

Predictions (written before either run; the 3-3 card is the user-reported 32 GB card, whose CID I have not read):

| | Lifetime 1 | Lifetime 2 |
| --- | --- | --- |
| Returns normally, restoration words unchanged | yes | yes |
| Card identified | S18A set, CMD11 answered, at 1.8 V | no CMD11; identified at 1.8 V |
| S18A (OCR bit 24) in the last ACMD41 | set | set (clear is possible; it would not stop the test) |
| Bank at identification (`GPIO_POW_VAL` bits 11:10, selector) | 3 (1.8 V), selector 0 | 3, selector 0 |
| Bank after the close | 2 (3.3 V), selector 1 | 2, selector 1 |
| Outcome | `limited_to_sdr25`, 50 MHz | `limited_to_sdr25`, 50 MHz, `reentered` |
| CMD6 group 1 support field at 1.8 V | `0x801f` if the card is SDR104 and DDR50 capable, at least `0x8007` | the same value as lifetime 1 |
| Read rate per request | about 21 MB/s (what 50 MHz four-bit gives at 3.3 V) | the same |
| CRCs of the 32 reads | any | equal to lifetime 1's, read by read |

The test fails, and nothing is retried, when:

- lifetime 1 does not return normally (the watchdog resets it, or its report has `reason` other than 0),
- lifetime 1 returns but the card did not switch (`card_signals_1v8` false, outcome `declined`): lifetime 2 is then not run,
- lifetime 2 stops at identification (`reason` 3): the card did not answer at 1.8 V, so it refuses CMD0 re-entry, and the SD on this
  board stays unusable until power is removed.

### Lifetime 1 on 3-3 (entry `1793648082135831049-05f70d50`): everything ran, the close failed

The lifetime did not return. Its fault record decodes, through the probe's stop word, as: reason 11 (the close failed), platform fault 1, platform
stage 18 (`CLOSE_WAIT`), probe stage 8 (the end of the plan), 32 reads done. So identification, the switch, 32 SDR25 reads and the capture
all completed, the card was switched (it is taken to be at 1.8 V), the card clock was stopped, the bank was written back to 3.3 V and seen
to report it (`CLOSE_BANK` finished), and the next stage failed a check. The report was written to RAM and lost, because a lifetime that cannot
close cannot return.

Cause, found from lifetime 2's stop word (entry `1793649204965774597-d1ed3e86`, below): the native adapter's register filter refused the close's
last write, the restore of the inherited module setting. Its terminal-close exception named the high-speed profile only; in the UHS-I profile the
setting had to be one of the three running settings, so `update()` failed in `CLOSE_WAIT` (platform source line 838, stage 18). The platform
models write registers through their own hook, not the adapter's filter, so no host check applied the rule: the same gap as the first CMD25
(see "A class of host-coverage gap" in the storage status). The rule is now one pure function (`a733_mmc_platform_native_module_write_allowed`)
that the adapter and the models both apply: the close restore is admitted for the high-speed and UHS-I profiles only with the module off and the bus
in reset. `checks/sd-return-close.c` asserts it on every module write of a close, and `checks/a733-mmc-native-filter.c` tests it directly; the
old rule fails both. A first hypothesis, that the withstand-mode word changes with the supply, also stopped the model in `CLOSE_WAIT`; I added a
relaxation for it and removed it when the real cause was found, because nothing showed the board does that.

A failed lifetime now carries its site. The stop word published in the fault record's esr is the platform's fault line (10 bits), why (2),
platform stage (5), S18A, the five speed-mode support bits, whether the card signals 1.8 V, and a fold of the 32 read CRCs; the site byte is the
probe's reason. `apps/sd-backup-probe/input.py stop --site N --esr W` decodes it. The report keeps the same word and the platform's fault line,
why, stage and the withstand mode word at identification and after the close. A lifetime that has not closed cannot return its report, by
design: the SD controller, pins and bank are still held. So the stop word is what a failed close can publish, and it is made to hold the results.

### Lifetime 2 (entry `1793649204965774597-d1ed3e86`): the card re-entered at 1.8 V; the close failed the same way

Stop word `0xd1893f5d`, site 11, decoded with `input.py stop`: the card signals 1.8 V, the last ACMD41 answered **S18A clear**, the CMD6 group 1 support
field read at 1.8 V is **`0x1f`** (SDR12, SDR25, SDR50, SDR104 and DDR50), the 32 reads completed (CRC fold `0x5d`), and the close stopped at
platform stage 18, source line 838: the adapter's module restore, as above. So the card accepts CMD0, CMD8 and ACMD41 at 1.8 V from a host that moved the
bank to 1.8 V first, identifies and reads at SDR25, and it supports SDR104. Lifetime 1 never reported its S18A or support bits, and its stop word
carried no fault line; lifetime 2's data are the ones that exist.

Prediction for lifetime 2 (written before the run; the card is taken to be at 1.8 V and the bank at 3.3 V after the watchdog reset):

- The platform moves the bank to 1.8 V first (`ENTER_1V8`), the core identifies without CMD11, and CMD0, CMD8, ACMD41 with S18R, CMD2 to CMD7
  and ACMD6 succeed at 1.8 V and 400 kHz. A refusal of re-entry shows as a stop at identification (probe reason 3).
- S18A in the last ACMD41 is set, or clear without stopping the test; the card is identified as the same 3-3 card; its CMD6 group 1 field at 1.8 V is
  at least `0x8007`, probably `0x801f`.
- 32 reads of 128 KiB at SDR25, 50 MHz, about 21 MB/s, no CRC or timeout.
- The close writes the selector to 3.3 V, the bank reports 3.3 V, the later close stages pass, and the lifetime returns normally, leaving the card
  at 1.8 V with the bank at 3.3 V (`GPIO_POW_VAL` bits 11:10 = 2, selector 1). If the close fails again, the stop word says where, and it holds
  S18A, the support bits and the read CRC fold.

### Lifetime 2 again, after the close fix (entry `1793649661736969334-9742cb48`): returned normally

| Item | Predicted | Measured |
| --- | --- | --- |
| Returns normally, restoration words unchanged | yes | yes; `baseline_changed` empty |
| Bank at identification | value 3 (1.8 V), selector 0 | value 3, selector 0 (`GPIO_POW_VAL` `0x020ffd80`) |
| Identified at 1.8 V without CMD11 | yes | yes: `reentered`, outcome `limited_to_sdr25`, 50 MHz |
| S18A in the last ACMD41 | set, or clear without stopping | **clear** |
| CMD6 group 1 support at 1.8 V | at least `0x8007`, probably `0x801f` | **`0x801f`**: SDR12, SDR25, SDR50, SDR104, DDR50 |
| 32 reads of 128 KiB | about 21 MB/s, no error | median 21.4 MB/s per request (6.1 ms), 4 MiB in 0.30 s with the first request slower; no error |
| Bank after the close | value 2, selector 1 | value 2 (`0x020ff980`), selector 1 |
| Withstand mode (`GPIO_POW_MOD_SEL`) | not predicted | `0x01155550` at identification and after the close |
| Identification time | | 309 ms |

The card on 3-3 is a Phison `SD32G`, revision 1.2, serial `0000029d`, made December 2020, 61,067,264 sectors. It accepts re-entry at 1.8 V from a host that
moved the bank to 1.8 V first, answers ACMD41 at 1.8 V with S18A clear (so a session must not wait for it), and supports SDR104 and DDR50. The card is
still at 1.8 V; the bank is at 3.3 V; the board returned to FEL.

## SDR104 trial on port 3-3 (approved, read only)

One lifetime (`--uhs-mode 3`): the bank to 1.8 V, re-entry without CMD11, CMD6 to SDR104, the module clock to the fastest the platform verifies
(PERI0_300M / 1 for a 150 MHz card clock, else PERI0_400M / 2 for 100 MHz), the CMD19 sweep of the 64 sample delays, then 512 requests of 128 KiB
(64 MiB) from sector 0, and the close back to 3.3 V on the bank. The first 32 requests read the same sectors as the SDR25 lifetime, so their CRCs
compare (the SDR25 CRCs are kept in `sdr25-crc-3-3.txt` next to the build; their fold is `0x5d`). The platform only verifies the PERI0_300M
gates, it does not enable them, so whether the card runs at 150 or at 100 MHz depends on what the launcher leaves; the report says which
(`card_clock_hz`, `module_clock_hz`).

Host evidence before the run: the lifetimes model runs this lifetime at 150 MHz, at 100 MHz and with no passing delay (the fallback to SDR25 at
50 MHz), each with a cut at every poll except every 9th inside the sweep (2,868 cuts in all), and applies the native adapter's real register
filter to every register write of every lifetime (a shadow adapter in `checks/sd-return-close.c`), which found nothing to refuse; a mutation that
removes the CMD6 SDR104 argument from the filter fails it. That closes the gap that the close failure exposed for the rest of the SDR104 path.

Prediction (written before the run):

- **Re-entry** as in the SDR25 lifetime; the CMD6 SDR104 check shows function 3 supported and the set selects it.
- **Tuning:** a passing run of at least 12 consecutive delays among the 64; the chosen delay is the midpoint of the first longest run. I expect a
  window of 15 to 40 delays at 150 MHz and a wider one at 100 MHz.
- **Clock:** 150 MHz if the PERI0_300M gates are up, otherwise 100 MHz. I do not know which; the previous lifetimes only used 50 MHz.
- **Rates.** The raw limit of four lines is 75 MB/s at 150 MHz and 50 MB/s at 100 MHz (the 104 MB/s of the SDR104 mode is for 208 MHz, which this
  board's clock tree does not offer). With the per-request command cost of about 0.1 ms, the best a request can do is about 70 MB/s at 150 MHz.
  **Success is** a sustained rate over the 64 MiB of at least 45 MB/s at 150 MHz (twice the 21.7 MB/s of 50 MHz) or at least 30 MB/s at 100 MHz,
  with no error and the first 32 CRCs equal to the SDR25 CRCs. I expect 55 to 70 MB/s at 150 MHz if the card sustains it; this 2020 card may not.
- **Failure branches.** No passing run of 12 delays: the core clocks down to 50 MHz, selects SDR25 and goes on (outcome `no_window`), with no power
  cycle; the reads then run at about 21 MB/s and the lifetime returns normally. A CMD19 or data error that the core cannot recover is a retained
  fault; the stop word names it, the card stays at 1.8 V and the next lifetime re-enters it. Either way the report is the result and I stop.
- **Close:** the selector back to 3.3 V, the bank reports it, the module and bus restored, a normal return with no restoration word changed.

### SDR104 trial (entry `1793650091979550389-eb009a18`): stopped in the core during SDR104 selection

The lifetime did not return and its report was lost. Its stop word `0x8a070000`, site 3 (the probe's identification stage), decodes (with the layout in
force at that image, the fields of a reason-11 word) to platform source line 552, why 0, platform stage 14 (`IDENTIFY`), no S18A, no card. Line 552 in
`a733_mmc_platform.c` is where the platform sees the core in its FAULT state; because the platform records the line of the first `fail()` and why 0 means no
deadline, **no platform callback failed first and the platform's own deadline had not passed: the core faulted by itself.** The bank had already moved to 1.8 V
(stage 14 is after `ENTER_1V8`), the SDR25 re-entry had worked on the same path in the previous lifetimes, so the fault lies in what the trial adds: the CMD6
SDR104 check and set, the module clock change, the register programming, or the CMD19 sweep. The stop word did not carry the core's error or protocol step.

State left: the card at 1.8 V (possibly with SDR104 selected; CMD0 at 1.8 V ends that), the bank at 1.8 V until the watchdog reset (whether the selector is restored is
the first thing the next lifetime's baseline will show), the board back in FEL. No retry has been made.

Reading the stop again: platform stage `IDENTIFY` spans everything the core does from CMD0 to the card being ready for reads, which in the UHS-I profile includes CMD6, the
module clock change and the CMD19 sweep; the probe's report only becomes valid when the platform reports READY. So the stop was not before CMD6; it was somewhere in that whole
span. What the trial does that the SDR25 re-entry does not is only what follows ACMD6 (`sdr25_only` is read at one place, after ACMD6): CMD6 check and set, the module clock, the
register programming and the sweep. Every branch before CMD2 depends on the profile and the mode the same way in both: ACMD41 carries S18R (`reenter`), and a ready answer takes the
re-entry branch before the S18A test, so S18A clear (what 3-3 answered) is not examined at all, and CMD11 is never sent.

**Host reproduction.** The model's failing sample delays produced three kinds of error (data CRC, wrong pattern, no data), all of which the core tolerates. The vendor driver counts
every sampling error as a failed delay (`SDXC_INTERRUPT_ERROR_BIT`), among them the FIFO run error (bit 11), start-bit and end-bit errors. With a card model whose failing delays
include a FIFO run error, the core faults in the sweep at the first such delay: state FAULT, error CONTROLLER, protocol step `P_SD_TUNE`, sample delay 3. That is what the platform saw
(`fault` in the core, no platform callback first, why 0). The core's `TUNE_TOLERATED` mask omitted bit 11; it now includes it (bit 12, hardware locked, stays terminal), and the card
model's terminal error in the sweep became bit 12. The cause on the board is inferred from that reproduction, not observed: the stop word of the first trial did not carry the core's
error or step. The rerun's stop word does.

The probe now publishes, for a lifetime that fails before identification, the core's error (4 bits), its protocol step (6) and half the sample delay it had reached (5)
next to the platform's line, why and stage (`input.py stop --site 3`). The models run the sweep with all five kinds of sampling error.

### SDR104 trial, rerun after the sweep fix (entry `1793650532228649180-1fb49397`): SDR104 at 150 MHz works; the success threshold is met per request and missed sustained

Returned normally; `baseline_changed` empty; the board returned to FEL. Re-entry as before (bank 1.8 V at identification, 3.3 V after the close, selector 0 then 1,
withstand-mode word unchanged `0x01155550`). Identification including the sweep took 893 ms.

| Item | Predicted | Measured |
| --- | --- | --- |
| Module and card clock | 150 MHz if the PERI0_300M gates are up, else 100 MHz | **150 MHz** (module 300 MHz): the gates were up |
| CMD6 | function 3 supported, selected | selected function 3, outcome `sdr104` |
| Sweep window | at least 12 consecutive delays, 15 to 40 expected | passing delays 0-9 and 16-63 (58 of 64; map `ffffffffffff03ff`); first longest run 16 to 63, **48 wide**, chosen delay **40**; the gap 10-15 is the only failure |
| Errors | none | none: 512 requests done, reason 0, no CRC or timeout |
| Data integrity | first 32 CRCs equal the SDR25 CRCs | **equal** (all 32, including the one non-repeating block) |
| Per-request rate | | first 32 requests median 2.51 ms = **52.2 MB/s** (SDR25: 6.1 ms, 21.4 MB/s); all 512 median 2.76 ms = **47.6 MB/s**; 25% of the later requests take 5 to 17 ms |
| Sustained over 64 MiB | at least 45 MB/s at 150 MHz; 55 to 70 expected | **27.4 MB/s** (2.446 s): **below the threshold** |
| Close | selector to 3.3 V, normal return | bank 3.3 V (value 2, selector 1), normal return |

The sustained figure misses the threshold I wrote, and the prediction of 55 to 70 MB/s was too high for this card. Where the time goes, from the per-request times: the 512
requests add up to 2.10 s (mean 4.1 ms; median 2.76 ms), and the probe's own gap between requests (take, CRC, submit, one task step each) adds 0.34 s (0.67 ms per request).
A quarter of the requests take 5 to 17 ms, which is the card (a 2020 Phison SD32G) stalling on its media, not the bus: the requests that run clean complete at 47 to 52 MB/s,
against a raw limit of 75 MB/s at 150 MHz and the per-request command cost. So the per-request rate meets the 45 MB/s mark and the sustained rate does not; the bus is not the
limit, the card is. The OS loader overlaps requests, so it would recover the 0.34 s of gaps but not the card's stalls: about 30 MB/s sustained on this card, against 21.7 MB/s at
50 MHz (14.4 MB/s sustained by the same probe over 4 MiB). A card with better sequential reads would show more. The decision to adopt SDR104 rests on a gain of 1.4 to 2.4 times,
depending on whether the card's stalls count.

### Where the 5 to 17 ms requests are (from the 512 requests already measured)

Requests are 128 KiB from sector 0 on, so request n starts at n x 128 KiB; 32 requests make one 4 MiB allocation unit. Of the 512, 127 took over 5 ms; their positions:

| Requests | Span | Time per request | Note |
| --- | --- | --- | --- |
| 0 to 7 | first 1 MiB | 9.3 to 17.4 ms | cold start: the SDR25 run's first 8 requests on the same sectors took 12.8 to 21.0 ms against 6.1 ms after, **the same extra time (6.7 to 14.9 ms) at 50 and at 150 MHz** |
| 8 to 384 | 1 MiB to 48 MiB | 2.43 to 2.80 ms, flat, 2.51 ms in the first AU, 2.76 ms after | clean: 47 to 52 MB/s |
| 385 to 511 | 48 MiB to 64 MiB | 5.0 to 17.4 ms, 5.1, 7.5, 10.0, 12.4 ms recurring | 119 of 127 requests; a few clean ones between |

- **Not periodic.** The stalls are one block of 8 at the start and one zone from request 385 on (119 of its 127 requests slow, in runs of 67, 8, 22, 16, 5 and 1), not spaced by the 512 KiB, 1 MiB, 2 MiB or 4 MiB
  position (counts by position are flat), not at allocation-unit boundaries (AU 0 stalls in 8 of 32 requests, AUs 1 to 11 in none, AUs 12 to 15 in 28 to 32 of 32), and not after a fixed count of requests or bytes
  apart from the one change at 48 MiB.
- **Not the bus.** The extra time of the first eight requests is the same in absolute terms at SDR25 and SDR104, so it is not transfer time.
- **Quantised.** In the slow zone the times cluster at 2.5 ms plus multiples of about 2.5 ms (5.1, 7.5, 10.0, 12.4), so the card inserts waits in steps, not a smooth transfer.
- **Not data content.** The slow requests are mostly blocks of zeros (constant CRC `0x7ee8cdcd`: 125 of the 127) and fast requests include 263 of the 265 non-constant blocks, so it is not slow-to-read data versus fast zeros.
- **What this says.** The pattern follows the address (a slow first MiB, a fast 1 to 48 MiB, a slow 48 MiB onwards) and the time is independent of the bus speed: a card behaviour tied to regions of the flash translation or the card's read-ahead, which no address periodicity or fixed count explains. What it cannot separate is the card's cost per
  command (a read-ahead that does not carry across our CMD12 and next CMD18) from its cost per byte of media. The SDR25 64 MiB run was never made, so whether the slow zone is slow at 50 MHz is not known; only the first 4 MiB were read there.

### Request-pattern experiment, first submission (entry `1793650854687556052-db9aee9e`): stopped in the sweep, before any phase

Stop word `0x8a071c06`, site 3, decoded with the identification layout: platform stage 14, source line 552, core error 3 (TIMEOUT), core protocol step 32 (`P_SD_TUNE_RECOVER`), sample delay reached
12 or 13. The sweep had reached its first failing delays and the recovery after a failed CMD19 waited more than 10 ms for the command or data state machine to finish, then gave up with TIMEOUT.
The previous trial recovered from the same delays (10 to 15 failed there), so the controller's behaviour at the marginal delays is not repeatable: sometimes the command never completes.
The recovery was written to wait politely and reset afterwards; the vendor driver resets the host after every failed tuning command. The host model now has a failing delay that leaves the
command stuck until a controller reset; the old recovery times out on it as the board's did, and the recovery now forces the reset after 10 ms (at most 12 times in a sweep, then TIMEOUT).
No phase ran and no report exists. The card is at 1.8 V (the lifetime never closed), the bank was at 1.8 V until the watchdog reset, the board returned to FEL.

### The sweep against the vendor's tuning loop

Reference: `sunxi_mmc_execute_tuning` in the board kernel (`bsp/drivers/mmc/sunxi-mmc.c:2432-2640`), `sunxi_mmc_clk_gating_reset` (`:227`), `sunxi_mmc_reset_host` (`:640`), the v4p5x
`__sunxi_mmc_do_oclk_onoff` and `sunxi_mmc_save_spec_reg_v4p5x`. The vendor loop, for each of the 64 delays: reset the host (save the registers, gate the bus clock off and on through the
CCU, restore the saved registers), update the clock with `oclk_en(1)`, write the sample delay, then issue CMD19 up to 20 times, stopping at the first error or pattern mismatch; the delay
passes if all 20 matched. After the loop it picks the best delay, resets the host once more, writes it, and resets and updates the clock a last time. It never waits for the command or
data state machine, never sends CMD12, and does not reset the FIFO separately.

| Step | Vendor | Ours before | Ours now | Kept or changed, and why |
| --- | --- | --- | --- | --- |
| Repeats per delay, stop at first failure, pattern compare, window rule (first longest run, 12 or more, midpoint) | 20; yes; yes; yes | the same | the same | kept: identical |
| Reset before a delay | every delay, passing or failing | only after a failed command | only after a failed command | **kept**: a passing command leaves the controller idle; a reset before each of 64 delays adds 64 register restores, each with its own failure modes, and nothing shows a benefit. The failed-command case is the one that differs in behaviour. |
| Wait for the command or data state machine before the reset | none | up to 10 ms, then (first fix) a forced reset | none: reset at once | **changed**: this was the timing-dependent difference |
| Reset mechanism | CCU bus gate and reset toggle plus register save and restore | GCTRL soft, FIFO and DMA reset (`|7`), then rewrite of the SDR104 registers | the same GCTRL reset (the value of the vendor's `sunxi_mmc_reset_host`) and the same rewrite | **kept**: the CCU toggle belongs to the platform (the bus register has its own filter rules) and needs a new platform operation; if a stuck command survives the GCTRL reset the cap below trips and it becomes the next step |
| Registers restored | GCTRL, CLKCR, TMOUT, WIDTH, FTRGL, FUNS, DBGC, DMAC, DLBA, IMASK; DRV_DL, SAMP_DL, DS_DL, NTSR, EDSD, CSDC | drive phase (`0x140`), NTSR (`0x5c`), sample delay, TMOUT, WIDTH, CLKCR | the same | **kept**: the sweep is PIO with the interrupt masked, so GCTRL, FTRGL, DMAC, DLBA and IMASK do not take part in it; the SDR104 ones do and are rewritten and read back |
| Clock update after the reset | `oclk_en(1)`: CLKCR clock on, low power on (power-save mode), **DAT0 masked**; update command; wait; clear the interrupt flags; clear the mask | clock on only, update command, no mask | clock on with **DAT0 masked**, update command, clear the flags, clear the mask, read back | **changed**: a card that still holds DAT0 low stalls an update that does not mask it |
| Low-power clock gating | on (the board's device tree sets `sunxi-power-save-mode`) | off | off | **kept**: the vendor's own comment for this chip says it wants the clock always on to avoid a sample bug, and our delays were derived with it on |
| Delay change | write `SAMP_DL` after the clock update, clock running | same, without a clock stop | same | kept: identical in effect |
| Data timeout of CMD19 | the host's default | `TMOUT` 0x040000ff (about 1.7 ms at 150 MHz) | the same | **kept**: it bounds a command that gets no data |
| CMD12 or abort | none | none | none | identical |
| FIFO reset | none separate (the bus reset clears it) | within the GCTRL reset | within the GCTRL reset | kept: a superset |
| Reset after the sweep | reset, write the best delay, update the clock | write the best delay, restore the timeout | the same | **kept**: when the last delay failed the recovery has just reset; when it passed there is nothing to reset |
| Cap on trouble | none | none | consecutive recoveries that do not complete within 10 ms (reset bits still set, or the update still pending): the reset is repeated and after 12 in a row the sweep stops with TIMEOUT; a completed recovery clears the count | **added** (kept from the first fix, now consecutive) |

Host evidence. The card model has a failing delay that leaves the command stuck and DAT0 low: the old recovery (wait, then reset) timed out as the board did; the new one resets at once,
completes the masked update and finishes the sweep with the same window; a controller that stays stuck through reset and update makes the sweep stop with TIMEOUT after 12 recoveries;
removing the mask from the update makes the stuck-DAT0 case fail.

### Request-pattern experiment, second submission (entry `1793651292191500240-5493408d`): stopped in the sweep at delay 16 or 17

Stop word `0x8a073be8`, site 3: core error 7 (CARD), step `P_SD_TUNE`, delay 16 or 17, platform stage `IDENTIFY`. With the recovery matched to the vendor's (reset at once), the sweep got past
delays 12 and 13, where the previous submission had stopped, and stopped later, on a delay that the first successful trial had passed. The core faults when a command's R1 response carries a card
status error (`CARD_ERRORS`), at the response of any command with data and again in the `P_SD_TUNE` step. After a transfer aborted by a controller reset the card reports COM_CRC_ERROR once in its next
response (modelled; inferred, not observed). The board kernel's loop does not look at R1 at all: it judges a delay by the host's command and data errors and the pattern compare. The core now does the same
in the sweep, and the tolerated sampling errors are the vendor's whole error set (bits 1, 6, 7, 8, 9, 11, 12, 13, 15) plus data starvation (bit 10); only card insert and remove (bit 18, bit 31) stay terminal.
The card model's failing delays now include a hardware-locked and a starvation error, and a response with COM_CRC_ERROR after an aborted command: with the old core they fault, with the new one the sweep
completes with the same window.

This is the third stop in the sweep, each on a difference from the vendor loop that the host model did not have. Where the core and the vendor loop can still differ is listed in the table above; the sweep
has not been audited for any other check that fires on a marginal delay beyond those found.

### Audit of the tuning path against the vendor loop, and one rule for a failed attempt

The first three submissions of the experiment each stopped in the sweep, on a different check that fired on a marginal delay. Each fix added a tolerated bit or an exemption, and each time the host model lacked the board's failure kind.
The coordinator's rule is a full diff against the reference before a board run, so this section lists every path of `sunxi_mmc.c` that can end the sweep between its start (`sd_clock` step 2, which sets `tuning`) and the window decision
(`tune_advance`), and what the vendor does in the same situation. Vendor references are in the board kernel's `bsp/drivers/mmc/sunxi-mmc.c`.

**What the vendor does.** `sunxi_mmc_execute_tuning` (`:2415-2640`) runs each delay's CMD19 through `mmc_wait_for_req` up to 20 times (`:2561-2570`) and judges the delay by `cmd.error || data.error || memcmp(pattern)`. Nothing else:
it does not read the R1 status, check the FIFO or the controller state, or verify a register. `cmd.error` and `data.error` are set by `sunxi_mmc_finalize_request` (`:1060-1100`) when any bit of `SDXC_INTERRUPT_ERROR_BIT` (`sunxi-mmc.h:215-218`: bits 1, 6, 7, 8, 9, 11, 12, 13, 15)
is in the interrupt sum, and by the transfer watchdog (`sunxi_timerout_handle`, `:270-310`, `SUNXI_TRANS_TIMEOUT` 5 s, `:87`) for a request that neither completes nor errors. The loop has no other exit: a failed delay is recorded as failed (`:2572-2576`), and the loop goes on to the next.

**The rule.** While a CMD19 is in flight, any failure the attempt can produce fails that delay and never ends the sweep. It is one function, `fault()` in `sunxi_mmc.c`: while `tune_attempt` is set (set when the CMD19 is queued, cleared when the `P_SD_TUNE` step judges it),
a `CONTROLLER` or `CARD` error becomes `tune_failed`, which starts the recovery that resets the controller and moves to the next delay. The previous list of tolerated interrupt bits (`TUNE_TOLERATED`) is gone. One more thing ends an attempt without an error bit:
an attempt with no completion and no error within 10 ms (`TUNE_ATTEMPT_US`, the vendor's watchdog scaled to this driver) fails its delay too. The R1 status of the CMD19 is still not judged, as in the vendor loop (see the third stop above).

**What stays terminal, and why.** Each goes through `fatal()`, the old `fault()`.

| Path (function, check) | Vendor | Ours | Why |
| --- | --- | --- | --- |
| `rd`, `wr`, `fence`, FIFO read: a callback fails | not applicable (MMIO cannot fail) | terminal `IO` | the platform adapter refused the access (a filter or lease failure), not a sampling effect |
| `time_ok`: clock backwards, operation deadline | none | terminal `TIME`, `TIMEOUT` | the caller's own limits |
| `poll`: ownership lost (`retained`) | none | terminal `ADMISSION` | the bank and clock are no longer ours |
| `interrupts`: card remove (bit 31) or the controller's bit 18 | removal handled by the card-detect path | terminal `CONTROLLER` | the card is gone; nothing to tune |
| Attempts that hang, `TUNE_HUNG_CAP` (12) in a row | the 5 s watchdog, no cap | terminal `TIMEOUT` after 12 consecutive hung attempts | a card or controller that never answers; a completed attempt, passing or failing, clears the count |
| `tune_recover`: the reset bits stay set or the clock update stays pending | none (the reset is a clock gate toggle) | the reset repeats; after `TUNE_FORCED_RESETS` (12) consecutive incomplete recoveries, terminal `TIMEOUT` | kept from the second submission |
| `tune_recover`: GCTRL lost its CPU-FIFO bit after the reset, or the restored registers read back wrong | none (the vendor restores without reading back) | terminal `CONTROLLER` | these are the recovery's own checks, not the attempt's; a controller that cannot be restored cannot sample |
| `set_delay`: the delay register reads back wrong | none | terminal `CONTROLLER` | same |

Paths that are now attempt failures (all through `fault()`): the command or data start bit still set at issue (`command_step` step 0), the control register's FIFO or DMA state not as the PIO command needs (step 1), every error bit in the interrupt register
(`interrupts`, in steps 0, 2 and 3), the FIFO not empty after the block (step 3), and the R1 card-error faults of `command_step` and `card_response` (the R1 check at the data command is skipped in a sweep, as above). The DMA status faults and the
IDMAC steps 4 to 6 do not run for CMD19, which is PIO. A busy or start bit that never clears has no error, and the attempt timer ends it.

**Host fuzz.** `checks/sunxi-mmc-uhs.c` (function `fuzz`) injects one fault per sweep with the model's `inj_*` fields, armed before attempt 0 of each delay and before attempt 19 of four delays inside the window: every interrupt bit,
every R1 status bit, every bit of the status register (kept until a reset), every bit of the control register (one read, after the resets are done; bits 0 to 2 are excluded because flipping a reset-in-progress bit only makes software proceed early),
all bits at once, a command that never completes with the start bit clear and with it set. A run passes when the sweep ends READY with a window or the no-window fallback. The removal bits (18 and 31) must fault.

| Core | Runs | Failures | Interrupt bits | R1 bits | Hang | Stuck command | Control register | Status register |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| before (`e6dfa2a6`) | 8868 | 737 | 0 | 0 | 88 | 0 | 136 | 513 |
| after | 8868 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

The interrupt and R1 cases passed before because the third submission's fixes already covered them; what the audit found are the paths that were not covered: a sweep that hangs with no error bit (the sweep never ended), a status or control register that is not
as expected (a terminal fault, or a wait that never ended), none of which the model had.

### Request-pattern experiment (approved, one run)

One lifetime, re-entry then SDR104 as before (`--uhs-mode 3`), then eight phases over the slow zone and controls (`--phase FIRST:BLOCKS:READS[:quick]`; `quick` skips the per-request CRC and the progress call
except every 32nd request, which brings the gap between requests down from about 0.67 ms to what the platform's take and submit need):

| Phase | Region | Request | Count | Mode | Asks |
| --- | --- | --- | --- | --- | --- |
| A | 48 to 64 MiB | 128 KiB | 128 | normal | the zone again |
| B | the same | 128 KiB | 128 | quick | does the gap matter, or the second pass |
| C | the same | 128 KiB | 128 | normal | third pass: if slow again, B was the gap; if fast, the card remembers |
| D | the same | 64 KiB | 256 | normal | request size down |
| E | the same | 32 KiB | 512 | normal | request size down again |
| F | 4 to 12 MiB | 128 KiB | 64 | normal | the fast zone as a control |
| G | 100 to 116 MiB | 128 KiB | 128 | normal | an unseen region |
| H | 200 to 216 MiB | 128 KiB | 128 | quick | another unseen region, back to back |

Requests of 1 MiB and 4 MiB per CMD18 are not in it: the descriptor ring holds 33 descriptors, so a command reads at most 256 sectors (128 KiB), and the larger sizes need the ring, the buffers and the
platform's chain check extended; if the result points at the per-command cost they are the next step.

Prediction (written before the run). My best guess is a card behaviour that costs time per command, not per byte, and that our pattern does not cause:

- **A**: slow as before, mean 6 to 10 ms per request, 14 to 20 MB/s. **B and C**: the same as A within 20 percent (no gap or second-pass effect). If B is much faster than A and C slow again, the gap provokes it; if B and C are both fast, the card remembers.
- **D and E**: the per-command extra time stays about the same, so time per MiB rises: D about 1.5 to 1.8 times A per MiB, E about 2.5 to 3.5 times. If instead the time per MiB equals A's, the cost is per byte of media.
- **F**: fast, 2.4 to 2.8 ms per 128 KiB, 47 to 54 MB/s. **G and H**: slow like A (the zone beyond 48 MiB is where files lie), with H equal to G.
- **No error** in any phase; normal close; return normally.
- **What would change the plan:** a clear difference between A and B or C, or D and E not rising, points at something our pattern does (or a media-bound card). A rise in D and E with B equal to A points at the per-command cost.

## Board plan

Each stage has one hypothesis. Nothing runs without the coordinator's approval and none writes to the card. Stage 2 below is the
first that changes the card's state, and it is the user's decision (section 1b).

1. **Capability probe at 3.3 V (done).** S18A set. The CMD6 support field cannot show SDR104 at 3.3 V.
2. **Switch and in-session re-entry, no cycle (section 1b).** Resolution algorithm, CMD11, SDR25 reads, the SDR104 field and tuning
   if present, then CMD0 at 1.8 V and a full re-initialisation with a read, then a normal return. The card is left at 1.8 V.
   Failure branches by where the session ends: every row of the table in section 3 resolves in the next session by the algorithm.
   If the re-initialisation at 1.8 V fails, the card stays at 1.8 V until power removal, and only the SD is affected. Hypothesis:
   the card re-initialises at 1.8 V.
3. **Supply-cycle rehearsal at 3.3 V (rail change: AXP8191 register `0x11` bit 4, DC1SW2).** Not before the user has seen sections
   1 and 1b and the net is settled. It would hold DC1SW2 off for 3 s with PF pins and the clock low, enable it, and test the card
   with CMD13 to its old RCA.
4. **Adopt.** The OS runs SDR104 by default only after stage 2 proves re-entry; any intentional reset while the card is at 1.8 V
   needs the supply cycle (stage 3) if autonomous boot from the card is a requirement.

Marker for the card's level: a byte that outlives a watchdog reset and not a power removal would let a session skip step 2 of the
algorithm. The AXP8191's `BUFFER0` (`0x04`) is used by the vendor boot flow for the reboot mode, so it is not taken without a
decision. The algorithm does not depend on a marker.

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

### Request-pattern experiment, result (entry `1793652351260245353-286b63df`, 3-3, image `851a4859`, commit `1907eb70`)

The sweep completed (passing delays 0 to 9 and 17 to 63, window 17 to 63, delay 40, 150 MHz). All 1,472 requests ran, the lifetime closed with the bank back at 3.3 V, and the board returned normally. The entry's `result-mismatch` is the placeholder
expectation of the probe's input, as in the earlier successful run, not a run failure. Per phase (16 MiB for A to E, 8 MiB for F, G and H; mean request time, requests over 5 ms):

| Phase | Request | Region | Mean | Over 5 ms | Predicted |
| --- | --- | --- | --- | --- | --- |
| A | 128 KiB | 48 MiB, normal | 5.27 ms | 68 of 128 | 6 to 10 ms (missed: lower) |
| B | 128 KiB | same, quick | 5.42 ms | 70 of 128 | within 20 percent of A (met) |
| C | 128 KiB | same, normal again | 6.73 ms | 93 of 128 | within 20 percent of A (missed: 28 percent more) |
| D | 64 KiB | same | 4.82 ms (1.83 times A per MiB) | 88 of 256 | 1.5 to 1.8 times (just over) |
| E | 32 KiB | same | 1.93 ms (1.47 times A per MiB) | 9 of 512 | 2.5 to 3.5 times (missed) |
| F | 128 KiB | 4 MiB | 2.44 ms, every request | 0 | 2.4 to 2.8 ms (met) |
| G | 128 KiB | 100 MiB | 3.21 ms | 11 of 128 | slow like A (missed: mostly fast) |
| H | 128 KiB | 200 MiB, quick | 6.66 ms | 115 of 128 | slow like A (met) |

What it shows. First, our request pattern is not the cause: the slow requests are the same requests in A, B and C (67 of the 68 slow ones in A are slow in B and in C), and quick or normal makes no difference (A against B). Second, a per-command cost is refuted:
E issues four times as many commands as A over the same 16 MiB, and its extra time over the fast path is about the same in total (0.64 ms per request over 512 requests, 330 ms; A: 2.5 ms over 128 requests, 320 ms), where the per-command hypothesis predicted 2.5 to 3.5 times. Third, the slowness depends on
the card's position and history together, not on the data: in A the slow requests are numbers 1 to 16 and 71 to 127 of the 48 to 64 MiB region and requests 17 to 70 are fast, yet D reading the first 8 MiB of the same region is slow from its third request to its last, and C's slow start extends to request 42 where A's ended at 16. The region is all zeros (the slow and fast requests have the same CRC).
Fourth, regions differ: 4 MiB (F) is always fast, 100 MiB (G) mostly fast, 200 MiB (H) and 48 MiB (A to C) mostly slow. A fast request costs 2.75 ms for 128 KiB (47.6 MB/s), 1.81 ms for 64 KiB and 1.29 ms for 32 KiB, so about 0.8 ms per command plus 16 us per KiB.

Not explained: what in the card makes position 56 to 64 MiB slow in A to C but 48 to 56 MiB slow in D, and the changing length of the slow start. The candidates are the card's internal mapping lookups or read-ahead after a jump back (D, and each phase's start, follows a seek), which a descending or interleaved
order would separate from position. The sustained figure over all eight phases is 20.4 MB/s, so on this card at this position mix SDR104 delivers about what SDR25 did; the 47.6 MB/s per request is reached only on fast regions.
