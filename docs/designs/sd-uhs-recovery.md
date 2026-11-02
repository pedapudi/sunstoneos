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
