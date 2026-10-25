# Design: UHS-I SDR104 on the microSD slot

Status: **implemented and host-checked**. No board has run it, and no SD
command has yet run on either board, so the existing default-speed and
high-speed profiles are unqualified as well. The user approved switching the
microSD signalling to 1.8 V on 2026-10-25 ([handoff](../handoff.md)).

The read-only SD service gains a fourth bus profile, UHS-I SDR104 (profile 3).
It asks the card for 1.8 V signalling during identification, switches the
SoC's PF pin bank to 1.8 V, selects SDR104 and runs the card at 150 MHz after
sweeping the controller's sample delay with CMD19. Any failure before the card
accepts the switch, and any SDR104 or tuning failure after it, leaves a
working high-speed card. Failures in between are terminal, because the SD
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
   `SMHC_CMD` bit 28 (`sunxi-mmc.c:2656–2659`; manual 11.3.8.7). An R1 error
   aborts (`core.c:1228`). No response requires a power cycle (specification
   4.2.4.4; `core.c:1224–1225`, `1262–1265`).
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

## Differences from the previous SunstoneOS path

| Step | Before this change | After |
| --- | --- | --- |
| ACMD41 | No S18R; a response with bit 24 faulted | Profile 3 requests S18R when the platform reports the switch available; bit 24 then means S18A |
| CMD11 | Not issued; the native filter refused it | Issued with bit 28, exact form admitted only for profile 3 |
| RINTSTS bit 10 | Always terminal | Expected during the switch, terminal elsewhere |
| PF bank | Read-only checks of 3.3 V | One write of 0 to `0x070` with the card clock stopped; live checks then require 1.8 V |
| CMD6 | Function 1 (high speed) only | Function 3 (SDR104), then function 1 as the fallback |
| Module clock | PERI0_400M / 4..32 | Adds PERI0_300M / 1 and PERI0_400M / 2 for profile 3 |
| Sampling | New mode, fixed phases | Old mode, CMD19 sweep of `SAMP_DL_SW` |
| Read threshold | Reset value | SDR104 reads: the block size (64 bytes for CMD6 and CMD19, 512 for sectors); disabled in the profile's other timings, including the `NO_WINDOW` fallback; untouched in profiles 0–2 |
| `DRV_DL` bit 7 | Not written | Not written: the vendor sets it only in FPGA builds, and the manual marks it reserved |
| Data timeout | `0xffffffff` | 1.7 ms during tuning, then `0xffffffff` |

SunstoneOS also performs checks that the references do not: it requires
adaptive PF mode, sensed 3.3 V and CLDO5 programmed to 1.8 V before offering
the switch; it reads back the selector and every SMHC timing register; it
verifies the PERI0_300M gates and PLL ratio on every platform poll while
running from that parent; and it refuses a tuning sweep without a window of 12
delays instead of continuing with an unset delay. It uses PERI0_300M where the
vendor selects PERI1_300M (`sun60iw2p1.dtsi:4496–4497`), because PERI0 is
already the reserved and checked parent of the negotiated profiles; both give
300 MHz. It requires the switch-done bit and DAT3 high but not command done,
because clearing the clock update's flags could remove a command-done bit that
an early completion raised; the record keeps `RINTSTS` for comparison.

## Change

- **MMC core** (`drivers/storage/sunxi_mmc.c`). Profile 3 queries the
  platform once after CMD8, fixes S18R for every ACMD41, and on S18A runs the
  switch in steps 2–8 above as a non-blocking state machine with absolute
  deadlines. After ACMD6 it checks and sets SDR104, changes the clock, sweeps
  the sample delay with CMD19 and resets the controller FIFO and state
  machines after each failed block. The card record gains the outcome, the
  capability bits, the chosen delay and the passing window.
- **Storage platform** (`drivers/storage/a733_mmc_platform.c`). A new
  callback reports the capability, writes the selector once and polls the
  bank. The live voltage check follows the bank through 3.3 V, switching and
  1.8 V. The module callback admits the two new rates.
- **Native filters** (`a733_mmc_platform_native.c` and its header). The
  default aperture filter still refuses the selector; the adapter admits one
  write of 0 only for profile 3. CMD11 and CMD19 are admitted in their exact
  forms with zero arguments, and CMD6 with `0x00fffff3` and `0x80fffff3`.
  `THLD` at `0x100` becomes a selected SMHC register for SD only, and the
  adapter writes it only in profile 3; eMMC still refuses it.
- **Stage-2 loader.** It builds the core with `SUNXI_MMC_NO_UHS`, which puts
  every UHS-I path behind a constant false condition so that the compiler
  drops the code.
- **Read-only SD service** (`apps/storage-read`). Sessions may select profile
  3. Negotiated profiles poll an active read for up to 1 ms in one wake,
  as USB0 does within its 200 µs budget, so that each read's time measures the
  controller path rather than tick pacing. This also changes profiles 1 and
  2, which must use the same polling for the high-speed reads to be a fair
  comparison; profile 0 and the eMMC build keep one poll per tick. Records become version 4: fact
  words 6 and 7, which hold write diagnostics that this read-only service
  never sets, carry the read's elapsed microseconds and the UHS-I state, and
  the divisor byte's bit 7 marks the 300 MHz parent. The supervised launcher
  gains a read-only throughput plan.

## Fallback without a power cycle, unless the specification requires one

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
boot, the stage-2 loader's cold SD admission and any later SD lifetime would
meet a 1.8 V card with a 3.3 V bank. The runbook therefore removes board power
after every profile-3 lifetime.

## Measured goal and budgets

The goal is sequential and random read throughput, in MB/s (10^6 bytes per
second), from the native per-read time. The same image and card run first in
profile 2 (high speed, 50 MHz, 3.3 V) and then in profile 3. The
launcher's plan reads 128 consecutive 4 KiB requests, 96 4 KiB requests at
fixed pseudo-random 4 KiB-aligned offsets and 32 single sectors. The
single-sector reads give the per-request cost; the difference from the 4 KiB
mean gives the per-block cost.

Each 4 KiB request is one CMD18 with automatic CMD12, then CMD13. The model
below uses two unmeasured costs: a guarded register access through the
platform (about 0.6–1.0 µs, extrapolated from the PMIC adapter's measured 0.42–0.5 µs plus
the time and task checks), and one FIFO word load (assumed 0.10–0.25 µs). A
request costs about 50–110 µs of commands and platform polls, the card's
first-block latency (assumed 100 µs sequential, 300 µs random), and per block
the larger of the bus time and the PIO time.

| Per 512-byte block | High speed, 50 MHz | SDR104, 150 MHz |
| --- | --- | --- |
| Bus time | 20.8 µs | 6.9 µs |
| PIO time, 128 loads and two accesses | 14–34 µs | 14–34 µs |

| Prediction | High speed | SDR104 |
| --- | --- | --- |
| Sequential 4 KiB reads | 8.5–13.0 MB/s | 8.5–15.6 MB/s |
| Random 4 KiB reads | 6.0–7.9 MB/s | 6.0–8.9 MB/s |
| Per-block cost | 20.8–34 µs | 14–34 µs |

The programmed I/O path limits the gain to 1.0–1.2 times for sequential and
1.0–1.12 times for random reads, against the bus's 3 times. If the SDR104 per-block cost equals the high-speed one, PIO bounds
both, and the next change is the controller's descriptor DMA with larger
requests; [the follow-up note](sd-dma.md) plans both. Acceptance: SDR104 throughput is at least the
high-speed throughput in both patterns, with identical sector digests. An
unexplained shortfall blocks the merge.

Other budgets:

- **Identification.** The switch takes 13–23 ms. Tuning issues about 520
  CMD19 (20 for each passing delay, one for each failing delay) at about 1.6
  per tick, plus up to 2 ms per failing delay: 0.4–0.6 s. Sessions allow up to
  10 s; the runbook uses 5 s.
- **CPU.** The 1 ms read budget keeps the MMC task busy for each read's
  duration in profiles 1–3, at most about 0.7 ms per 4 KiB read; profile 0
  keeps one poll per tick.
- **Memory.** `struct sunxi_mmc` grows by 136 bytes, 16 of them in the card
  record, and `struct a733_mmc_platform` by 160 bytes; each read-service client
  slot gains 12 bytes of fields. `sunxi_mmc_init`'s frame grows from 864 to
  992 bytes (AArch64, `-O2`), and `command_step` from 560 to 576 bytes.

## Board qualification reads every card area and writes none

The qualification writes nothing to the card, including its protected areas
(boot0, the boot package and the stage-2 slots). Only CMD17, CMD18 and CMD13
touch media, and only for reads; CMD6, CMD11 and CMD19 change volatile card
state. Goal section 4 therefore needs no host backup and no data-partition
confinement for this test; the existing host backup remains the preservation
record. [The board runbook](../board-runbooks/sd-uhs.md) gives the commands
and predictions.

## Sources

The cache `~/.cache/sunstoneos/sources/sd-uhs/` holds the new downloads, and
its `sources.json` records each file's URL, revision, SHA-256 and license.
`checks/sunxi-mmc-uhs.py` pins every reference it reads by SHA-256.

| Source | Location and revision | SHA-256 |
| --- | --- | --- |
| SD Physical Layer Simplified Specification 9.10 (SD Association terms: personal, internal use; not redistributed) | [sdcard.org download](https://www.sdcard.org/downloads/pls/), file `Part1PhysicalLayerSimplifiedSpecificationVer9.10Fin_20231201.pdf` | `ee86b20e…5380e3` |
| Linux MMC core and upstream `sunxi-mmc` (GPL-2.0) | `torvalds/linux` tag v6.12, commit `adc218676eef25575469234709c2d87185ca223a`: `drivers/mmc/core/{core,sd,sd_ops,mmc_ops}.c`, `drivers/mmc/host/sunxi-mmc.c`, `include/linux/mmc/{sd,host}.h` | per file in `sources.json` |
| Board kernel MMC core and vendor `sunxi-mmc` (GPL-2.0) | `orangepi-xunlong/linux-orangepi` at `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` (Linux 6.6.98): `drivers/mmc/core/{core,sd,sd_ops,mmc_ops}.c`, `bsp/drivers/mmc/sunxi-mmc{.c,.h,-v5p3x.c}` | per file in `sources.json` |

The board kernel's pin control, clock and device-tree files, the v4p5x timing
file, the A733 manual, datasheet and schematic are the existing pinned copies
under `sources/pins/`, `sources/board-kernel/`, `sources/mmc-sdc3/`,
`sources/camera-research/` and `sources/board-documents/`.

## Checks

- `checks/sunxi-mmc-uhs.py` pins the references above and asserts the cited
  facts, then runs two models under AddressSanitizer and
  UndefinedBehaviorSanitizer. `checks/sunxi-mmc-uhs.c` drives the MMC core
  against a modeled UHS-I card: the switch order and its 1 ms and 10 ms
  waits, SDR104 at 150 and 100 MHz, the window rules, eight declines or
  fallbacks and eight terminal failures. `checks/a733-mmc-uhs.c` runs the
  actual storage platform and shared providers: one selector write in adaptive
  mode, both rates, no write in manual mode, a bank timeout and three losses
  after the switch, each retained.
- `checks/storage-read-uhs-host.py` decodes version-4 records with the
  unchanged observer, accepts eight consistent records and refuses ten
  inconsistent ones, and checks the throughput plan and summary.
- The existing core, write, SD-mode, idle-timing, platform, adaptive-voltage,
  ROM-pin, SDC3 and storage-update checks pass unchanged.

The native SD-read emulation checks fail on the current image before reaching
this change (a repeated structure name with two layouts in the DWARF); the
same check fails identically on the base image, and the
[handoff](../handoff.md) already lists these checks as failing on current
images.

The integration gate `sd-uhs-1025-b4bc04d0` passed on the clean tree at
`b4bc04d0`: the storage-maintenance, board, mimus-native, GPU render, HDMI,
camera and storage-probe images, the supervised OS image and the check index.
Evidence is under `~/.cache/sunstoneos/build/gate-sd-uhs-1025-b4bc04d0/`.

Images built from clean archives of `3c17aeca` and `b4bc04d0` at one shared
path differ as follows:

| Image | Change |
| --- | --- |
| Read-only SD service (`sunstone-a733-storage-read`) | `.text` +7,380 B, `.rodata` +296 B, `.bss` +192 B: the UHS-I core and platform functions, the filters and the record |
| Storage maintenance | `.text` +6,144 B, `.rodata` +296 B, `.bss` +128 B: the same core and platform code |
| Stage-2 slot loader (`a733-stage2`) | `.text` +152 B and `.bss` +128 B with `SUNXI_MMC_NO_UHS`: larger `protocol`, `command_step` and `sunxi_mmc_init` from the shared structure and the high-speed refactor; no UHS-I function remains |
| Board, mimus-native, GPU render, HDMI, camera, supervised and resident OS | Only the 32-byte build identity in `.rodata`; `.text` identical, because their links discard the MMC code |

The standalone single-core SD and eMMC read projects fail to link on both
revisions in the board application's TinyUSB sources, as the handoff records.
Their storage objects compile on both; against the base, the eMMC build's
`native.c` grows from 7,324 to 7,436 B and its `sunxi_mmc.c` from 11,758 to
17,248 B, and the single-core SD build's `native.c` from 7,348 to 7,704 B.

Evidence: `~/.cache/sunstoneos/evidence/sd-uhs-20261025/`, with the host
checks, the image comparison and the standalone objects of the reviewed
revision in `review-fixes/`.
