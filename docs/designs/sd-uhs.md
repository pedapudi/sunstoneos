# Design: UHS-I SDR104 on the microSD slot

Status:
- **High speed (profile 2): qualified on the board** for returning reads
  ([the DMA read design](sd-dma.md)).
- **UHS-I SDR104 (profile 3): qualified on the board** for read-only
  lifetimes on port 3-3, reached by re-entering a card already at 1.8 V
  ([recovery design](sd-uhs-recovery.md)): 150 MHz, tuning window found, reads
  match the SDR25 CRCs. The first switch of a card from 3.3 V to 1.8 V
  (CMD11) ran on port 3-3 with SDR25 reads; the switch followed by SDR104
  tuning in one lifetime is host-tested only.
- **Writes at 1.8 V or 150 MHz and OS adoption: open** ([adoption design](sd-uhs-adoption.md)).

The user approved switching the microSD signalling to 1.8 V ([handoff](../handoff.md)).

The read-only SD service has a fourth bus profile, UHS-I SDR104 (profile 3).
It asks the card for 1.8 V signalling during identification, switches the
SoC's PF pin bank to 1.8 V, selects SDR104 and runs the card at 150 MHz after
sweeping the controller's sample delay with CMD19. Any failure before the card
accepts the switch, and any SDR104 or tuning failure after it, leaves a
working high-speed or SDR25 card. Failures in between are terminal, because the SD
specification then requires a card power cycle, which this board provides
only by removing board power.

## The PF pin-bank supply carries the signalling voltage

SDC0 uses PF0–PF5; PF6 is card detect. These are the only bonded PF pins, and
the datasheet lists their power domain as `VCC18-PF/VCC-IO`
(`A733-datasheet.txt` lines 2535–2563). The SoC has two PF supply balls:
`VCC-IO` (ball 1U5, 3.3 V) and `VCC18-PF` (ball 1U6, 1.8 V, maximum 2.1 V;
datasheet lines 5402, 5416 and 5648). On the board, DC1SW2 feeds `VCC-IO` and
`VCC-CARD`, and CLDO5 feeds `VCC18-PF` and `VCC-PC` (schematic revision 1.2,
sheet 9 for the balls and the power-tree sheet for the rails). The card's VDD
stays at 3.3 V through R51 (sheet 12).

The SoC chooses between the two supplies itself. Manual section 18.14.4.6
states that only the PF port has a configurable group voltage, set through
`GPIO_POW_VAL_SET_CTL`. The registers, all in the GPIO block at `0x02000000`:

| Register | Field | Values | Manual |
| --- | --- | --- | --- |
| `GPIO_POW_VAL_SET_CTL` `0x070` | bit 0 `VCC_PF_PWR_VOL_SEL` | 1 selects 3.3 V (reset value), 0 selects 1.8 V | 18.14.6.3 |
| `GPIO_POW_VAL` `0x048` | bits 11:10 `PF_PWR_VAL` | 2 reports 3.3 V, 3 reports 1.8 V | 18.14.6.2 |
| `GPIO_POW_MOD_SEL` `0x040` | bits 11:10 `PF_PWR_MOD_SEL` | 0 or 2 manual 3.3 V, 1 adaptive, 3 manual 1.8 V | 18.14.6.1 |

The board kernel reaches the same register. The board tree's SDC0 node has no
`vqmmc-supply` (`sun60i-a733-orangepi-zero3w.dts:1623–1652`); its
`vqmmc33sw-supply` (DC1SW2) and `vqmmc18sw-supply` (CLDO5) are both enabled
at probe and stay on (`bsp/drivers/mmc/sunxi-mmc.c:1942–1954`; both regulators
are `regulator-always-on`, `dts:2307` and `dts:2397`). So
`sunxi_mmc_signal_voltage_switch` skips the regulator and selects the
`mmc_1v8` pin state (`sunxi-mmc.c:2225–2246`), whose pins carry
`power-source = <1800>` (`sun60iw2p1.dtsi:1005–1029`, `4506–4508`). The pin
controller applies `PIN_CONFIG_POWER_SOURCE` to the PF bank only
(`pinctrl-sunxi.c:1252–1260`). Both A733 pin descriptors set
`auto_power_source_switch` (`pinctrl-sun60iw2.c:2585–2599`), so it calls
`sunxi_power_auto_switch_pf` (`pinctrl-sunxi.c:1600`). For a decrease, that
function writes 0 to `0x070`, polls `0x048` bit 10 every 100 µs for up to
10 ms, waits 10 µs and verifies the result (`pinctrl-sunxi.c:1410–1437`). It
writes nothing else; it relies on the adaptive withstand mode.

The answer is therefore the pin-bank supply selector. No PMIC rail changes,
and no VQMMC regulator exists on this board. The second board reports adaptive
mode 1, sensed 3.3 V and selector 1
(`second-board-20261018-3-3/single-core-admission/remaining-facts.md`).

The two A733 pin descriptors disagree on the polarity of `GPIO_POW_VAL` bit
10. The first silicon version uses hardware type 4, which reads bit 10 set as
3.3 V (`power_mode_reverse = false`, `pinctrl-sunxi.c:184–186`); version B
uses type 10, which reads it set as 1.8 V (`auto_power_detect_mode_reverse =
true`, `pinctrl-sunxi.c:342–345`), and `pinctrl-sun60iw2.c:2605–2614` chooses
between them by `sunxi_get_soc_ver()`. The manual's two-bit field matches
type 10: value 2 (bit 10 clear) is 3.3 V and value 3 is 1.8 V. The second
board read value 2 at 3.3 V, which is the type-10 polarity. Under type-4
polarity a 3.3 V bank would read 3, and the existing admission, which requires
2 before any card command, refuses that board, so a wrong polarity cannot lead
to a switch.

The schematic shows no external pull-up on CMD or DAT; the SoC pulls them up
internally from the bank supply (15 kΩ on PF1, 100 kΩ on the others,
datasheet lines 5880–5900). PF6 reaches the socket's switch through 1 kΩ and
an open-drain MOSFET (sheet 12). No PF pin therefore sees 3.3 V while the bank
is at 1.8 V.

## The reference sequence

The SD Physical Layer Simplified Specification 9.10 gives the command order
(section 3.9.4, Figure 3-16; section 4.2.4.1, Figure 4-6), and the pinned
Linux MMC core gives the host timing. The core is the same in Linux v6.12
and in the board kernel at `2ac08e8c` (6.6.98). Its comments cite the full
specification; the simplified edition leaves section 4.2.4.2, the switch
timing, blank.

1. **ACMD41 with S18R.** The host sets HCS and S18R (bit 24) and repeats
   ACMD41 with the same argument until the card is ready (specification
   4.2.3.1; `drivers/mmc/core/sd.c:868–869`). A ready response with S18A
   (bit 24) admits CMD11 before CMD2 (`sd.c:890–891`). Linux does not also
   require CCS (`sd.c:884–889`).
2. **CMD11.** The board kernel sets the controller's voltage-switch flag,
   `SMHC_CMD` bit 28 (`sunxi-mmc.c:2656–2659`; manual 11.3.8.7). The command
   completes on the voltage-switch interrupt (bit 10), not on command done:
   the interrupt mask leaves command done off for CMD11, and command done
   arrives only at the end of the switch in step 8 (`sunxi-mmc.c:1333–1345`,
   `sunxi-mmc.h:221–227`). An R1 error aborts (`core.c:1228`). No response
   requires a power cycle (specification 4.2.4.4; `core.c:1224–1225`,
   `1262–1265`).
3. **Card holds DAT low.** After at least 1 ms, the host checks that the card
   drives DAT low (`core.c:1235–1239`). The board kernel reads `SMHC_STATUS`
   bit 8, the DAT3 level (`sunxi-mmc.c:2291–2311`; manual 11.3.8.16).
4. **Clock stops.** The clock-off update carries the voltage-switch flag and
   masks DAT0 (`core.c:1191–1192`; `sunxi-mmc-v5p3x.c:364–379`, `400`;
   `CLKCR` bit 31, manual 11.3.8.2).
5. **The host switches.** The PF bank goes to 1.8 V, as above.
6. **10 ms gate.** The clock stays stopped at least 10 ms (`core.c:1198`; the
   comment there cites 5 ms in the specification).
7. **Clock restarts** at the identification rate with the same update
   (`core.c:1199–1200`).
8. **Card releases DAT.** After at least 1 ms (`core.c:1251`), the board
   kernel waits up to 10 ms for DAT3 high and for both `RINTSTS` bit 10
   (`DSTO_VSD`, switch done; manual 11.3.8.15) and command done
   (`sunxi-mmc.c:2295–2306`). Failure requires a power cycle (`core.c:1257–1265`;
   specification Figure 4-6).
9. **Identification and mode.** CMD2, CMD3, CMD9, CMD7, ACMD6 for four data
   lines (`sd.c:660`), then CMD6 function group 1 value 3, SDR104
   (`sd.c:487–529`; specification 4.3.10). The driver strength stays type B,
   because the sunxi host has no `select_drive_strength`
   (`sd.c:423–452`). The current limit stays at its default, because the host
   reports no maximum current (`sd.c:557–606`).
10. **Clock and timing.** The board tree limits SDC0 to 150 MHz (`dts:1644`).
    The vendor reaches it from a 300 MHz module clock with the internal divider
    at zero (`sunxi-mmc-v5p3x.c:466`, `504`, `640`). The SMHC0 mux offers
    PERI0_400M (1) and PERI0_300M (2) (`ccu-sun60iw2.c:1083–1090`); PERI0_300M
    is PERI0_2X / 4, gated at `0x1908` bits 11, 9, 5 and 4
    (`ccu-sun60iw2.c:118–124`, `1893–1923`). Tuning moves the controller to
    the old sample mode (`sunxi-mmc.c:2475–2480`; `sunxi-mmc-v4p5x.c:813`)
    with 180-degree command and data drive (`sunxi-mmc-v4p5x.c:303–305`).
    In SDR50 and SDR104 reads the read threshold equals the block size, so a
    block starts only with room for all of it; other timings disable it
    (`sunxi-mmc-v5p3x.c:647–700`; manual 11.3.8.29).
    The vendor's `SMHC_DRV_DL` bit 7 write for clocks above 400 kHz
    (`sunxi-mmc-v5p3x.c:565–575`, `sunxi-mmc-v4p5x.c:773–783`) sits inside
    `#ifdef MMC_FPGA` (`v5p3x.c:556–600`, `v4p5x.c:763–809`); the production
    branch only clears the clock divider, and manual 11.3.8.36 marks
    `DRV_DL` bits 15:0 reserved. SunstoneOS therefore leaves bit 7 alone.
11. **Tuning.** The board kernel sweeps `SAMP_DL_SW` over 64 delays (manual
    11.3.8.37; `sunxi-mmc-v4p5x.c:1000–1010`). A delay passes only after 20
    matching 64-byte CMD19 blocks (`sunxi-mmc.c:2505–2580`;
    `MMC_TUNING_RETRY_TIMES`, `sunxi-mmc.h:611`). It chooses the midpoint of
    the first longest passing run, and requires at least 12 delays
    (`sunxi-mmc.c:2332–2413`, `2448`). A sweep without such a run returns
    success with no delay chosen (`sunxi-mmc.c:2583–2635`). The tuning block
    is the specification's pattern (4.2.4.5) and Linux's
    `tuning_blk_pattern_4bit` (`mmc_ops.c:27–36`). The card must complete
    40 CMD19 in 150 ms.

Upstream Linux `sunxi-mmc` supports no A733 controller. It switches only
through a VQMMC regulator (`drivers/mmc/host/sunxi-mmc.c:957–972`) and has no
tuning; it confirms the core sequence only.

## What SunstoneOS checks beyond the references

SunstoneOS requires adaptive PF mode, sensed 3.3 V and CLDO5 programmed to
1.8 V before offering the switch; reads back the selector and every SMHC timing
register; verifies the PERI0_300M gates and PLL ratio on every platform poll
while running from that parent; and refuses a tuning sweep without a window of
12 delays instead of continuing with an unset delay. It uses PERI0_300M where
the vendor selects PERI1_300M (`sun60iw2p1.dtsi:4496–4497`), because PERI0 is
already the reserved and checked parent of the negotiated profiles; both give
300 MHz. It requires the switch-done bit and DAT3 high but not command done,
because clearing the clock update's flags could remove a command-done bit that
an early completion raised; the record keeps `RINTSTS` for comparison. The
vendor's `SMHC_DRV_DL` bit 7 write is left out (see step 10). The data timeout
is 1.7 ms during tuning and `0xffffffff` otherwise.

## Implementation

- **MMC core** (`drivers/storage/sunxi_mmc.c`). Profile 3 queries the
  platform once after CMD8, fixes S18R for every ACMD41, and on S18A runs the
  switch in steps 2–8 above as a non-blocking state machine with absolute
  deadlines. CMD11 completes on bit 10 and reads its R1 then; after the clock
  restart the core clears every flag the update set, and the switch ends on
  DAT3 high with bit 10 and command done arriving afterwards. After ACMD6 it
  checks and sets SDR104, changes the clock, sweeps the sample delay with CMD19
  and resets the controller FIFO and state machines after each failed block
  ([sweep rules](sd-uhs-recovery.md#the-tuning-sweep-follows-the-vendor-loop)).
  The card record holds the outcome, the capability bits, the chosen delay and
  the passing window. The read threshold is the block size in SDR104 reads
  (64 bytes for CMD6 and CMD19, 512 for sectors), disabled in the profile's other timings including the
  `NO_WINDOW` fallback, and untouched in profiles 0–2. Re-entry (`sunxi_mmc_reenter`) and SDR25-only
  (`sunxi_mmc_sdr25_only`) options serve the recovery lifetimes.
- **Storage platform** (`drivers/storage/a733_mmc_platform.c`). A callback
  reports the capability, writes the selector once and polls the bank. The live
  voltage check follows the bank through 3.3 V, switching and 1.8 V. The module
  callback admits PERI0_300M / 1 and PERI0_400M / 2 for profile 3 in addition
  to PERI0_400M / 4..32. The close puts the bank back at 3.3 V.
- **Native filters** (`a733_mmc_platform_native.c` and its header). The
  default aperture filter refuses the selector; the adapter admits one write of
  0 only for profile 3 (and the single write back, see the recovery design).
  CMD11 and CMD19 are admitted in their exact forms with zero arguments, and
  CMD6 with `0x00fffff3` and `0x80fffff3`. `THLD` at `0x100` is a selected SMHC
  register for SD only, written only in profile 3; eMMC refuses it.
- **Stage-2 loader.** It builds the core with `SUNXI_MMC_NO_UHS`, which puts
  every UHS-I path behind a constant false condition so that the compiler
  drops the code.
- **Read-only SD service** (`apps/storage-read`). Sessions may select profile
  3. Negotiated profiles poll an active read for up to 1 ms in one wake, so
  each read's time measures the controller path rather than tick pacing;
  profile 0 and the eMMC build keep one poll per tick. Records are version 4:
  fact words 6 and 7 carry the read's elapsed microseconds and the UHS-I state,
  and bit 7 of the divisor byte marks the 300 MHz parent. The supervised
  launcher has a read-only throughput plan.

## Failure handling

| Failure | Card state | Action |
| --- | --- | --- |
| PF not in adaptive mode, CLDO5 not 1.8 V, or switch otherwise unavailable | 3.3 V, S18R never sent | High speed at 3.3 V, outcome `HOST` |
| Card answers without S18A | 3.3 V | High speed at 3.3 V, outcome `DECLINED` |
| CMD11 without response or with an R1 error | Unknown; specification requires a power cycle | Retained fault |
| DAT not low 1 ms after the CMD11 response | Unknown | Retained `SIGNAL` fault before any host change |
| Selector write fails, or PF not at 1.8 V within 10 ms | Card in the switch sequence | Retained fault |
| DAT3 not high or no switch-done bit within 11 ms of the restart | Unknown | Retained `SIGNAL` fault |
| SDR104 unsupported, busy or not selected by CMD6 | 1.8 V, SDR12 | SDR25 at 50 MHz, outcome `NO_SDR104` |
| No passing run of at least 12 delays | 1.8 V, SDR104 | Clock to 50 MHz, CMD6 SDR25, outcome `NO_WINDOW` |
| Sampling errors on a CMD19 | 1.8 V, SDR104 | Mark the delay failed, reset FIFO and state machines, continue |
| Any other controller error during tuning | Unknown | Retained fault |

A retained fault keeps every lease and stops card commands, as in the other
profiles. Recovery is whole-board power removal, the documented recovery of
every supervised SD lifetime.

After a successful switch the card keeps 1.8 V signalling through CMD0 until
it loses power (specification 3.9.4). DC1SW2 also supplies `VCC-IO`, so only
whole-board power removal restores 3.3 V. Until then, the boot ROM's microSD
boot, the stage-2 loader's cold SD admission and any later SD lifetime meet a
1.8 V card with a 3.3 V bank. The runbook therefore removes board power after
every profile-3 lifetime. A session that finds a card still at 1.8 V re-enters
it ([recovery design](sd-uhs-recovery.md)).

## Qualified figures

| Result | Value | Record |
| --- | --- | --- |
| High-speed returning reads, port 3-3 | 256 reads, every CRC recorded, card clock stopped at exit | `sd-dma.jsonl` `sd-dma-lifetime-a` |
| SDR104 at 150 MHz, port 3-3, 64 MiB read | window 16 to 63, delay 40, 47.6 MB/s median per request, 27.4 MB/s sustained | `storage.jsonl` `1793650532228649180-1fb49397` |
| Request-pattern experiment, port 3-3 | 20.4 MB/s over eight phases; card-dependent stalls | `storage.jsonl` `1793652351260245353-286b63df` |
| CMD11 completion defect (fixed in the core) | the core waited for command done after CMD11 | `sd-uhs.jsonl` `sd-uhs-lifetime-b` |

The programmed-I/O throughput model (about 1.0 to 1.2 times gain from SDR104)
is in `sd-uhs.jsonl` `sd-uhs-throughput-model`; descriptor DMA ([sd-dma](sd-dma.md))
removes that limit.

## Board qualification reads every card area and writes none

The qualification writes nothing to the card, including its protected areas
(boot0, the boot package and the stage-2 slots). Only CMD17, CMD18 and CMD13
touch media, and only for reads; CMD6, CMD11 and CMD19 change volatile card
state. Goal section 4 therefore needs no host backup and no data-partition
confinement for this test; the existing host backup remains the preservation
record. [The board runbook](../board-runbooks/sd-uhs.md) gives the commands.

## Limitations and open work

1. The card at 1.8 V needs board power removal before the ROM, boot0 or the stage-2 loader read it.
2. Writes at 1.8 V or at 150 MHz are not qualified; the write service is qualified at 3.3 V, 50 MHz only.
3. SDR50 and DDR50 are not implemented.
4. Port 3-1's card (Phison SD64G) has only been seen at 3.3 V.
5. The OS adopting SDR104 is open ([adoption design](sd-uhs-adoption.md)).

## Sources

The cache `~/.cache/sunstoneos/sources/sd-uhs/` holds the downloads, and
its `sources.json` records each file's URL, revision, SHA-256 and license.
`checks/sunxi-mmc-uhs.py` pins every reference it reads by SHA-256.

| Source | Location and revision | SHA-256 |
| --- | --- | --- |
| SD Physical Layer Simplified Specification 9.10 (SD Association terms: personal, internal use; not redistributed) | [sdcard.org download](https://www.sdcard.org/downloads/pls/), file `Part1PhysicalLayerSimplifiedSpecificationVer9.10Fin_20231201.pdf` | `ee86b20e…5380e3` |
| Linux MMC core and upstream `sunxi-mmc` (GPL-2.0) | `torvalds/linux` tag v6.12, commit `adc218676eef25575469234709c2d87185ca223a`: `drivers/mmc/core/{core,sd,sd_ops,mmc_ops}.c`, `drivers/mmc/host/sunxi-mmc.c`, `include/linux/mmc/{sd,host}.h` | per file in `sources.json` |
| Board kernel MMC core and vendor `sunxi-mmc` (GPL-2.0) | `orangepi-xunlong/linux-orangepi` at `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` (Linux 6.6.98): `drivers/mmc/core/{core,sd,sd_ops,mmc_ops}.c`, `bsp/drivers/mmc/sunxi-mmc{.c,.h,-v5p3x.c}` | per file in `sources.json` |

The board kernel's pin control, clock and device-tree files, the v4p5x timing
file, the A733 manual, datasheet and schematic are the pinned copies
under `sources/pins/`, `sources/board-kernel/`, `sources/mmc-sdc3/`,
`sources/camera-research/` and `sources/board-documents/`.

## Checks

- `checks/sunxi-mmc-uhs.py` pins the references and asserts the cited facts,
  then runs the core against a modeled UHS-I card (`checks/sunxi-mmc-uhs.c`)
  and the storage platform with the shared providers (`checks/a733-mmc-uhs.c`).
- `checks/storage-read-uhs-host.py` decodes version-4 records and checks the
  throughput plan and summary.
- Results, image-size comparison and the integration gate are in the record
  `sd-uhs-host-checks`.
