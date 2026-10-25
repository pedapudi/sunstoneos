# Clock audit: SunstoneOS rates against the vendor's top operating points and the A733 limits

Status: **audit of 2026-10-25, from sources and earlier board reads.** No board
was accessed for it. A read-only dump of every clock register is frozen for
both boards and has not run ([measurement](#a-read-only-dump-measures-both-boards)).

The project's performance policy ([AGENTS.md](../AGENTS.md), "Implementation")
sets every hardware block's default to the highest operating point that the
vendor uses for the chip's speed bin, with the vendor's voltage. Lower clocks
are optional modes. This audit compares, for each clock of the Orange Pi Zero
3W class boards on USB ports 3-1 and 3-3:

- **(a) current:** the rate that SunstoneOS programs or admits today, from code
  and board reads;
- **(b) vendor top:** the highest rate that the vendor's board software uses
  for this board and speed bin;
- **(c) hardware maximum:** the limit that the A733 datasheet or user manual
  states;
- **(d) gap:** the difference and its cause.

An *operating point* is a pair of clock rate and supply voltage from the
vendor's device-tree tables. The *speed bin* is the silicon grade that the
SID eFuse word `0x0300624c` records: bits 23:16, or bits 15:8 when those are
zero, select one voltage column of each table. Both boards are code 4, column
`vf0400` (port 3-3 reads `0x496`, port 3-1 `0x495`). The board oscillator
(DCXO) runs at 26 MHz, so the CPU tables' `26m` columns apply, and PLL_REF
divides it to the 24 MHz reference of every other PLL.

## The main blocks run below the vendor's top points

Rates are in MHz unless marked. "Inherited" means that SunstoneOS checks a
rate that boot firmware left and does not program it.

| Block | (a) Current | (b) Vendor top, bin vf0400 | (c) Hardware maximum | (d) Gap |
| --- | --- | --- | --- | --- |
| CPU-L (6 × A55) | 494 at 800 mV | 1794 at 1000 mV | 2000 ("up to 2.0 GHz") | 3.6× below; CPU clock driver off by default |
| CPU-B (2 × A76) | 1014 at 800 mV | 2002 at 1050 mV | 2000 | 2.0× below; same cause |
| DSU (L3) | 494 | 1274 at 1000 mV | Not stated | 2.6× below; no SunstoneOS DSU control |
| GPU | 1008 at 960 mV | 1008 at 960 mV | Not stated | None |
| NPU | 492 at 800 mV (852 profile exists) | 852 at 800 mV | Not stated; the table's highest row for any bin is 1188 | 1.7× below until the 852 default merges |
| Video engine, JPEG | Session-supplied, at most 720; PLL_VE inherited and unread | 624 at 800 mV | Not stated | Unknown until the dump; no default rate |
| Display engine | 600 | 600 | 696 for 4K output (manual note) | None |
| G2D | 100 | 300 | Not stated | 3× below |
| DRAM | 2040, P-state 0 | 2040 for this board's strap | 2400 (LPDDR5) | None against the vendor; 15% below the SoC limit |
| MBUS | 600 for the first image of a lifetime; 400 after any return to the ROM | 600 | 600 | 1.5× below for every later image |
| NSI | 600 | 600 | 700 | None against the vendor |
| AHB | 200 | 200 | 200 | None |
| microSD (SMHC0) card clock | 0.4 by default; 25 and 50 profiles; 150 SDR104 built and not yet run | 150 SDR104 | 200 SDR at 1.8 V | 375× below by default; 3× below at high speed |
| SDIO Wi-Fi (SMHC1) card clock | 25 | 208 (SDR104 allowed) | 200 SDR at 1.8 V | Up to 8× below; module support unrecorded |

The [ranked changes](#ranked-changes) order these gaps by expected gain over
risk.

## Sources

Line numbers refer to the pinned files below. The board kernel files are from
`orangepi-xunlong/linux-orangepi` at `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`,
cached in `~/.cache/sunstoneos/sources/board-kernel/` (the CPU table in
`~/.cache/sunstoneos/sources/npu-platform/`).

| Short name | File | SHA-256 |
| --- | --- | --- |
| vf.dtsi | `sun60iw2p1-cpu-vf.dtsi`, table version V0.9: CPU, DSU, NPU and video-engine operating points | `d5535630…` |
| dtsi | `sun60iw2p1.dtsi`: SoC nodes, GPU operating points, assigned rates | `1c7313f6…` |
| dts | `sun60i-a733-orangepi-zero3w.dts`: board overrides | `31145367…` |
| ccu.c | `ccu-sun60iw2.c`: PLL and module clock fields and parents | `1d32bb85…` |
| DS | A733 datasheet V0.93, cached text `A733-datasheet.txt` (line numbers) | PDF `ebbdf6a9…`, text `0c7c3c19…` |
| UM | A733 User Manual V1.00 (section numbers), `a733-user-manual-1.00` in `sources.lock.json` | `cdd77e57…` |
| boot0 | `boot0_sdcard_orangepizero3w.fex` and its 1800M variant ([vendor firmware](vendor-firmware/README.md)) | `829ea8e3…`, `d2a9bccc…` |
| sys_config | `sys_config.fex` of orangepi-build `a95129bf` | `eb598823…` |
| Schematic | Orange Pi Zero 3W schematic v1.2, cached text `schematic-v1.2.txt` | PDF `4e0e92af…` |

Earlier board reads come from `~/.cache/sunstoneos/evidence/`: the NPU state
read on both ports (`board-session-20261025-3/port-3-3/bb9-read`,
`board-session-20261025-4/port-3-1/bb9-read`), the CPU clock read on port 3-3
(`board-session-20261024-8/port-3-3/bb14-read`), the second board's SD pin and
clock state (`second-board-20261018-3-3/sd-pin-clock-state`), and the state
reads taken before and after each NPU start (`board-session-20261025-{7,9,12,15}/port-3-3/*-read-*`).

## Rates by block

### CPU clusters and the DSU

The CPU PLL block at `0x08870000` holds one PLL word and one mux word per
domain; the rate on the PLL source is `26 MHz × N ÷ 2^P`
([CPU clocks](cpu-clock.md)).

| Clock | (a) Current | (b) Vendor top | (c) Hardware maximum | (d) Gap and cause |
| --- | --- | --- | --- | --- |
| CPU-L | 494: PLL `0xf8802600` (N=38), mux `0x03010000` (P=1), DCDC5 800 mV, read on port 3-3 (bb14-read). The driver that changes it is built only with `SUNSTONE_A733_OS_CPU_CLOCK`, off by default (`cmake/a733-os.cmake:43`) | 1794 at 1000 mV (vf.dtsi:599, 615) | DS:1059 "up to 2.0 GHz"; UM 4.1.3.2 lists the CPU PLL outputs below 2.0 GHz | 3.6×. The user approved CPU clock and voltage changes on 2026-10-24 |
| CPU-B | 1014: PLL `0xf8802700` (N=39), mux `0x03000000` (P=0), DCDC3 800 mV (bb14-read) | 2002 at 1050 mV (vf.dtsi:1340, 1356) | DS:1059; UM 5.1.10 gives the CPU_B PLL range as 650 MHz to 2.26 GHz | 2.0×; same cause |
| DSU | 494: PLL `0xf8802600`, mux `0x03010105` (bb14-read). An earlier port 3-3 capture read the mux as `0x03000105`, which decodes to 988 MHz | 1274 at 1000 mV (vf.dtsi:1989, 2005), set by the vendor's DSU frequency driver. The shipped board kernel leaves that driver out and keeps 494 | Not stated | 2.6×. CPU-L and the DSU share DCDC5, so the DSU point needs the same 1000 mV as CPU-L's top point |

The branch `work/cpu-top-opp-1025` (`docs/designs/cpu-top-opp.md`, not
merged) derives these points and the vendor's change order, and freezes the
first board step: CPU-B to 1404 MHz at 800 mV with the A76 cores off.

### Accelerators

| Clock | (a) Current | (b) Vendor top | (c) Hardware maximum | (d) Gap and cause |
| --- | --- | --- | --- | --- |
| GPU | 1008: PLL_GPU0 N=84, P0=2 (`0xe9105300` locked), GPU clock `0x80000000`; DCDC4 960 mV (`include/a733_gpu_opp.h:12-18`, `drivers/gpu/a733_gpu_power.c:22-25`) | 1008 at 960 mV, the node's `vfdefault` (dtsi:4736-4739). The vf0400 column lists 900 mV (dtsi:4751), but the vendor kernel tree carries no speed-bin map for the GPU, so its driver takes `vfdefault` ([GPU power](gpu-power.md)) | Not stated; UM 4.1.3.2 gives PLL_GPU0 a 1.8 GHz typical VCO | None |
| NPU | 492: PLL_NPU N=82, P0=4 at 800 mV, the startup default (`drivers/npu/a733_npu_startup.c:14-28`). 852 (N=71, P0=2) is a second profile, which today only `apps/npu-convolution` selects | 852 at 800 mV (vf.dtsi:2069-2091, vf0400 at 2077 and 2087). The 1008 MHz row has no vf0400 entry (vf.dtsi:2093) | Not stated. Rows up to 1188 exist for other bins (vf.dtsi:2133) | 1.7×. `work/npu-job-overhead-1025`, in review, makes 852 the default |
| Video decoder, encoder and JPEG encoder | The bindings derive the rate from the inherited PLL_VE0 or PLL_VE1 and require it to equal the session's value, at most 720 (`drivers/video/a733_video_unit.c:134-151`, [video platform](video-platform.md)). They program no PLL. No board has read PLL_VE | 624 at 800 mV (vf.dtsi:2169-2172) | Not stated; UM 4.1.3.2 gives PLL_VE a 1.872 GHz typical VCO, which divides to 624 | Unknown until the dump reads PLL_VE. SunstoneOS has no default rate |
| ISP and CSI | 324 each, from PLL_VIDEO0 4X (`drivers/clock/a733_media_clock.c:120-122`) | 324 each on this board (dts:3626-3627); the SoC tree gives 600 and 540 (dtsi:2567-2568) | Not stated | None for this board |
| Display engine | 600 from PLL_DE 3X (`a733_media_clock.c:124-126`) | 600 from PLL_DE 3X (dtsi:3764-3766, dts:2715) | UM 4.1.4.3: 696 when DE drives 4K | None at the vendor rate |
| TCON and HDMI | Pixel clock 74.25 or 148.5 from the HDMI TOP PLL at a 24 MHz oscillator; 73.9375 or 147.875 at the board's 26 MHz ([HDMI](hdmi.md)) | Mode-dependent | 4K at 60 fps (DS:30) | Modes above 1080p are not implemented; the 26 MHz rows run 0.42% slow |
| G2D | 100: PERI0_400M ÷ 4, fixed by the session check (`apps/g2d/admission.c:25`) | 300 (dtsi:3383-3384) | Not stated | 3× |
| Deinterlacer | 300: PERI0_600M ÷ 2 (`apps/deinterlace/admission.c:24`) | 300 (dtsi:3364) | DS:1299: 500 for 1080p60 YUV420, 550 for YUV422 with every function | None against the vendor |

### Memory and buses

| Clock | (a) Current | (b) Vendor top | (c) Hardware maximum | (d) Gap and cause |
| --- | --- | --- | --- | --- |
| DRAM | 2040, boot0's trained P-state 0 (P-states 2040, 1165, 816 and 408), LPDDR5, 12,288 MiB, DRAM_VCC 560 mV (`ddr-20261015-223537/firmware.log`). DRAM0 clock `0x80000003`: PLL_DDR ÷ 4 | 2040: the only set boot0 holds for this board's strap ([DRAM](#dram-runs-the-only-set-that-boot0-holds-for-this-boards-strap)) | 2400 for LPDDR5 (DS:1091) | None against the vendor; 15% below the SoC limit |
| MBUS | 600 (`0x81000000`, PERI0_600M) right after the cold DDR start; 400 (`0x84000000`, PERI0_400M) after any return to the boot ROM. Every driver only checks it (`drivers/dma/a733_dma_platform.c:19-22`) | 600: the value after boot0; the board kernel sets no MBUS rate (ccu.c:517) | 600 (UM 4.1.4.3) | 1.5× for every image after the first in a lifetime ([MBUS](#mbus-falls-to-400-mhz-after-every-return-to-the-boot-rom)) |
| NSI | 600 (`0xc3000000`, PERI0_600M) after boot0; 24 in a lifetime without boot0 (port 3-1 bb9-read) | 600 (dtsi:1481) | 700 (UM 4.1.4.3, from PLL_DE 3X at 696) | None against the vendor |
| AHB | 200 (`0x03000002`, PERI0_600M ÷ 3), on both boards | 200 | 200 (UM 4.1.4.3) | None |
| APB0, APB1 | Inherited from boot0; unread. Drivers accept SYS24M or PERI0_600M ÷ M (`drivers/i2c/a733_twi_native.c:154-157`) | boot0's value; the kernel sets none | 100 each (UM 4.1.4.3) | Unknown until the dump |
| APB_UART | 24, checked (`drivers/uart/a733_uart_platform.c:35-39`) | boot0's value | 160 (UM 4.1.4.3) | Limits UARTs to 1.5 Mbaud (DS:1704-1707) |
| GIC | 100 (`0x84000003`, PERI0_400M ÷ 4), read on both boards | Same | Not stated | None |

### Storage

| Clock | (a) Current | (b) Vendor top | (c) Hardware maximum | (d) Gap and cause |
| --- | --- | --- | --- | --- |
| SMHC0, microSD | Profile 0 by default: SYS24M ÷ 30 = 800 kHz module, 400 kHz card, one bit (`tools/check-a733-storage-launch.py:84-89`). Profiles 1 and 2: PERI0_400M ÷ 8 or ÷ 4 for 25 or 50 MHz cards. Profile 3: PERI0_300M ÷ 1 for a 150 MHz SDR104 card at 1.8 V, or PERI0_400M ÷ 2 for 100 (`drivers/storage/a733_mmc_platform.c:249-257`) | 150 SDR104 (dts:1637, 1644) | 200 SDR at 1.8 V (DS:1108) | See [microSD](#microsd-defaults-to-400-khz-and-sdr104-at-150-mhz-has-not-run) |
| SMHC1, SDIO Wi-Fi | 400 kHz while enumerating, then PERI0_400M ÷ 8: 25 MHz, four bits (`drivers/net/a733_radio_platform.c:226-235`) | Up to 208 with SDR104 (dts:1663, 1670) | 200 SDR at 1.8 V (DS:1108) | Up to 8×. Which UHS-I modes the module accepts is unrecorded; the Wi-Fi data path's demand is unmeasured |
| SMHC2, eMMC | 400 kHz legacy profile (`a733_mmc_platform.c:248`) | HS400 at 200 (dts:1598-1614) | 200 DDR at 1.8 V (DS:1115-1116) | Not applicable: neither board has eMMC ([board wiring](board-wiring.md)) |

### Input and output

| Clock | (a) Current | (b) Vendor top | (c) Hardware maximum | (d) Gap and cause |
| --- | --- | --- | --- | --- |
| USB2 controller and PHY | Reference SYS24M, pipe PERI0_480M ÷ 4 = 120 (`drivers/usb/a733_usb2_platform.c:141-151`) | Same parents (ccu.c `usb2_*_parents`) | Protocol rates | None |
| USB3 PHY | Raw DCXO through RTC `0x0709016c` bit 4 (`drivers/usb/a733_usb3_phy.c:60`) | Same | 5 or 10 Gb/s | None in clock terms |
| PCIe | Gen1, 2.5 GT/s; refused on a 26 MHz oscillator because the PHY table is verified only for 24 MHz ([PCIe](pcie.md)) | Gen3 (dtsi:5079); enabled on the board (dts:1794-1801) | Gen3, 8 GT/s (DS:1661) | Refused on both boards; link speed 3.2× below Gen3 |
| SPI | Module from SYS24M, up to 24 (`drivers/spi/a733_spi.c:37-47`) | 100 module clock (dtsi:3585) | 100 (DS:1743) | Up to 4×; no board SPI device needs more today |
| TWI, S_TWI0 (PMIC) | S_TWI0 400 kHz, inherited and checked (`drivers/power/a733_pmic.c:113-145`); TWI9 and TWI11 at most 100 kHz | 400 kHz (dts:1946-1947) | 400 kHz standard fast mode | None for the PMIC; other TWI at the standard rate their devices need |
| UART | APB_UART 24; Bluetooth UART 1.5 Mbaud with divisor 1 ([UART](uart.md)) | Radio firmware default 1.5 Mbaud ([Bluetooth HCI](bluetooth-hci.md)) | 10 Mb/s with a 160 MHz APB clock (DS:1704) | None at the radio's rate |
| I2S | PLL_AUDIO1 3072 VCO, DIV5 614.4, I2S0 24.576 (`drivers/audio/a733_i2s_platform.c:7-15`) | Same parents and default (ccu.c, `pll_audio1_div5_clk`) | Audio rates | None |
| Crypto engine | 400, PERI0_400M (`drivers/crypto/a733_ce_clock.c:79-87`) | 400 (dtsi:2408) | Not stated | None |
| DMA | Bus gates only; runs on AHB and MBUS | Same | Bus limits | Follows the MBUS gap |
| GPADC, THS | 24, SYS24M on the 26 MHz board (`drivers/thermal/a733_thermal.c:96-121`) | 24 | 1 MHz sampling (DS:1792) | None |

### Timers and core PLLs

| Clock | (a) Current | (b) Vendor top | (c) Hardware maximum | (d) Gap and cause |
| --- | --- | --- | --- | --- |
| DCXO | 26, enabled and stable (`0x07090160` = `0x983fb0f7`, both boards) | 26 | 26 crystal (DS:6033) | None |
| Generic timer | `CNTFRQ_EL0` = 24 MHz (`platform/a733-smp-cached/continue.S:21-23`); FreeRTOS tick 1 kHz | 24 | Fixed | None |
| PLL_REF | 24 from 26 × 96 ÷ 104 (`0xf8675f00`, both boards) | Same (ccu.c:38-56) | VCO 1.9–2.8 GHz (UM 4.1.3.2) | None |
| PLL_CPU_L, PLL_CPU_B, PLL_DSU | See the CPU table | | VCO 650 MHz to 2.26 GHz (UM 4.1.3.2) | |
| PLL_DDR | Unread; boot0 sets it during training | boot0 | VCO 1.26–2.52 GHz | Unknown until the dump |
| PLL_PERI0 | VCO 2400: 2X 1200, outputs 800, 600, 480, 400, 300, 200, 150 (`0xff126310`, both boards) | Same; UM 4.1.3.2 typical VCO 2.4 GHz | VCO 1.26–2.52 GHz | None |
| PLL_PERI1 | Unread; no SunstoneOS consumer | boot0 | Same | Unknown until the dump |
| PLL_GPU0 | 1008 when the GPU is on | 1008 | VCO 1.26–2.52 GHz | None |
| PLL_NPU | 492 or 852 when the NPU is on; off at boot (`0x41104500`) | 852 | Same | See NPU |
| PLL_VIDEO0, 1, 2 | VIDEO0 4X checked at 324; VIDEO1 4X checked at 1188 for DisplayPort; VIDEO2 unused | Mode-dependent | VCO 1.26–2.52 GHz | Unknown until the dump |
| PLL_VE0, PLL_VE1 | Inherited, unread | Set for 624 | Same | Unknown until the dump |
| PLL_DE | 3X checked at 600 | 600 | Same | None |
| PLL_AUDIO0, PLL_AUDIO1 | AUDIO0 unused; AUDIO1 VCO 3072 | AUDIO0 90.3168 for 44.1 kHz rates; AUDIO1 3072 | AUDIO1 VCO 1.8–3.1 GHz | None |

## DRAM runs the only set that boot0 holds for this board's strap

**Parts and rating.** The schematic draws one 32-bit LPDDR5 device on sheet 7
("07_LPDDR5 32X1") and names no part number. No recorded marking, photograph
or part list identifies it, so its rated speed is unknown. Two records would
identify it:

- a photograph of the package marking on the board's top side, decoded with
  the maker's part-number guide;
- the LPDDR5 mode registers MR5 (manufacturer), MR6 and MR7 (revision) and
  MR8 (density, type and width), which boot0 reads during training but does
  not log. No pinned source documents the controller registers that would read
  them back.

**What the SoC supports.** The datasheet limits LPDDR5 to 2400 MHz and LPDDR4
or LPDDR4X to 2133 MHz (DS:1089-1091). If the MHz figures follow the same
convention as LPDDR4-4266 at 2133 MHz, 2040 MHz is 4080 MT/s and 2400 MHz is
4800 MT/s, 16.3 and 19.2 GB/s on 32 bits. This conversion is an inference.

**How boot0 selects a set.** boot0 reads a resistor divider on GPADC channel 1
and the level of PH16, as `sys_config` selects (`select_mode = 3`), and
compares the voltage with the thresholds 163, 382, 608, 811, 1050, 1315, 1569
and 1800 mV. The schematic's table maps those voltages to DDR parameter sets 1
to 8 for PH16 low. The board log reads `vaild para:4  select dram para5`: the
ADC index 4, about 1050 mV, set 5. The schematic labels sets 1, 2, 3, 6 and 8
with 4, 6, 8, 12 and 2 GB and leaves set 5 unlabelled, so its capacity labels
do not describe these 12 GiB boards.

**The parameter sets.** The table `DRAM.ext` at boot0 offset `0x374` holds 15
slots of 128 bytes; slot *k* is set *k*+1. Its words follow the `sys_config`
`dram_para` order: clock, type, ODT, drive strengths, `para0`–`para2`, mode
registers, then the timing words `tpr0`–`tpr14`.

| boot0 | Slot | Set | Clock (MHz) | `para1` | Differences from slot 4 of the default boot0 |
| --- | --- | --- | --- | --- | --- |
| Default | 0 | 1 | 2400 | `0xa10a` | `para1`; `mr17` `0x06`; `tpr0` `0x4040`; `tpr2` `0x170b0703`; `tpr10` bit 31 clear |
| Default | 1 | 2 | 2040 | `0xa10a` | `para1`; `tpr13` `0x65` |
| Default | 4 | 5, **selected** | 2040 | `0xa11a` | — (`tpr2` `0x13090603`, `tpr13` `0x10065`) |
| 1800M | 0 | 1 | 1800 | `0xa10a` | Clock, mode-register and timing words |
| 1800M | 1 | 2 | 1800 | `0xa10a` | Clock, mode-register and timing words |
| 1800M | 2 | 3 | 1800 | `0xa11a` | Clock, mode-register and timing words |

All six are LPDDR5 (type 9). boot0 first copies the `sys_config` default block
(`dram_clk = 1200`, `dram_type = 8`), and the selected set replaces it
([`boot0-functions.json`](vendor-firmware/data/boot0-functions.json), the set
selector). The 1800M variant
has no set in slot 4, so it holds nothing for this board's strap; what boot0
does with an empty slot is not traced.

**Training result.** On the board, set 5 trained four P-states at 2040, 1165,
816 and 408 MHz with result code 7, DRAM_VCC 560 mV, 12,288 MiB, `para2`
`0x30001001` and `dram_tpr13` `0x10065`
(`~/.cache/sunstoneos/evidence/ddr-20261015-223537/firmware.log` and
`dram-parameters.bin`).

**No faster vendor-validated set exists for this board.** The only 2400 MHz
set, slot 0, belongs to strap 1 and to the `para1` geometry `0xa10a`; this
board uses `0xa11a`, and slot 0 also differs in four timing and mode words.
Running this board at 2400 MHz would need a parameter set that the vendor has
not published for this strap and part, the part's rating, and training
qualification on both boards.

## MBUS falls to 400 MHz after every return to the boot ROM

MBUS clocks the memory-bus fabric between the masters (CPU, DMA, NPU, GPU,
video) and the DRAM controller. Its register is CCU `+0x588`: source in bits
26:24, divider in bits 4:0, and an update bit 27 that applies a new
configuration (UM 4.1.6.76).

| Board read | MBUS word | Rate |
| --- | --- | --- |
| Port 3-1 in a lifetime without boot0 (bb9-read) | `0x84000000` | 400 |
| Port 3-3, first state read after each cold DDR start (four lifetimes, `*-read-0`) | `0x81000000` | 600 |
| Port 3-3, every state read after an image returned to the ROM in the same lifetimes (`*-read-1` and later) | `0x84000000` | 400 |
| Port 3-3, a USB fault capture and a USB return capture of 2026-10-18 | `0x81000000` and `0x84000000` | 600 and 400 |

The boot ROM writes MBUS in its clock setup (function `0x2084` of the ROM
inventory, [`brom-mmio.json`](vendor-firmware/data/brom-mmio.json)), the cold
DDR start, which runs boot0's routines from SunstoneOS's RAM wrapper, leaves
600 MHz, and no SunstoneOS driver writes the register.
The reads therefore fit this account: each return to the ROM reruns the ROM's
clock setup, which leaves 400 MHz, and the next image inherits it. This is an
inference from the reads. The [dump](#a-read-only-dump-measures-both-boards)
run at the end of a lifetime, after its images returned, confirms or refutes it.

Two consequences follow:

- Every image after the first of a lifetime runs its memory traffic on a bus
  clocked at two thirds of boot0's rate, which is also the manual's maximum.
- The ShuffleNetV2 timing of 2026-10-25 (`board-session-20261025-12/port-3-3`)
  ran the 492 MHz start first, with MBUS at 600 MHz, and the 852 MHz start
  after a ROM return, with MBUS at 400 MHz unless something restored it. Its
  ratio of 0.79 against 0.72 predicted, and the 1.85 ms per inference that
  did not scale with the NPU clock, are therefore confounded with the MBUS
  rate.

## microSD defaults to 400 kHz, and SDR104 at 150 MHz has not run

- **Profiles.** The storage launcher (`tools/check-a733-storage-launch.py`)
  defaults to profile 0: one data line at
  400 kHz. Profiles 1 and 2 run four lines at up to 25 and 50 MHz at 3.3 V.
  Profile 3 switches the PF pin bank and the card to 1.8 V signalling, selects
  SDR104 and runs at 150 MHz from PERI0_300M, with 100 MHz from PERI0_400M ÷ 2
  and 50 MHz SDR25 as fallbacks ([UHS-I design](designs/sd-uhs.md)). The user
  approved the 1.8 V switch on 2026-10-25.
- **Board status.** Only the 400 kHz path has board evidence. The first
  profile 3 run (2026-10-25, port 3-3) launched and timed out with the result
  unobserved; the port needs a power removal ([handoff](handoff.md)).
- **Vendor limit.** The board device tree enables SDR104 on SDC0 and caps it at
  150 MHz (dts:1637, 1644), below the controller's 200 MHz at 1.8 V (DS:1108).
  The vendor's reason for the cap is not recorded; trace length and the
  board's PF wiring are the likely limit, so 150 MHz is the board limit this
  audit adopts.
- **Slot wiring.** SDC0 uses PF0–PF5 with PF6 as card detect. DC1SW2 supplies
  `VCC-IO` and the card's VDD at 3.3 V; CLDO5 supplies `VCC18-PF` for 1.8 V
  signalling (schematic sheets 9 and 12).
- **Card.** The cards' models and UHS-I support are not recorded. The card
  reports 1.8 V support (S18A) in its ACMD41 response and SDR104 support in
  CMD6 function group 1; the profile 3 run records both.
- **Throughput.** Programmed I/O bounds the gain: the design predicts 1.0–1.2
  times the high-speed throughput, against the bus's 3 times, until the
  controller's descriptor DMA replaces it ([SD DMA](designs/sd-dma.md)).

## A read-only dump measures both boards

The Read bundle `clock-audit-words` (bundle 19 in `tools/prepare-board-bundles.py`)
reads 380 words through the boot ROM's FEL `readl` command and writes no device
register:

- the SID speed-bin word;
- the 299 CCU registers that the manual's register list (UM 4.1.5) or the
  board kernel's `ccu-sun60iw2.c` names, `0x02002000`–`0x02003f50`;
- the 34 PRCM registers below `0x07010340` (UM 4.2.4 and `ccu-sun60iw2-r.c`);
- the 41 CPU PLL registers below `0x08873034` (UM 5.1.5.5);
- after R_CCU `0x0701020c` shows the RTC bus gate open and its reset released,
  the RTC clock words `0x07090000`, `+0x4`, `+0x60`, `+0x160` and `+0x16c`.

The reader admits words only inside its ungated blocks, so PRCM `0x360` and
above, which include the crypt key register, and `CPU_SLOW_CLK_SEL` at
`0x08873034` are not read. The reader is the fixed reader
`tools/board-bundles-next-read.py` (`c93e56ee…`); against the version that
bundles 9 and 14 ran (`6d7bae37…`) it adds only the SPC and S_TZMA blocks,
which this bundle does not use.

The frozen set is
`~/.cache/sunstoneos/evidence/board-bundles-20261025-clock-audit/`, prepared
from `e3d5bf48` with one bundle per port (`port-3-1/`, `port-3-3/`) and these
offline files:

- `runbook.md`: run point A right after the cold DDR start, before any image,
  and optional point B at the end of a lifetime;
- `predictions.json`: 49 checked words from earlier board reads (for example
  PLL_REF, PERI0, AHB, NSI, MBUS 600 MHz, the CPU PLL words) and 331 recorded
  words with their manual reset defaults;
- `check-predictions.py`: compares a session with the predictions;
- `decode-clocks.py`: decodes every PLL, the CPU domains and 25 module clocks
  into MHz.

The offline self-test of the checker and decoder passes on five synthetic
sessions, and both bundles re-validate with the preparation tool's own checks.
The coordinator runs the bundle; this audit made no board access.

## Ranked changes

Gain is the expected speed-up of the work the board runs; risk counts board
writes that have not run before and their recovery. The user has approved CPU,
GPU and NPU clock and voltage changes and the microSD 1.8 V switch.

1. **Restore MBUS to 600 MHz in every image.**
   - *Reference:* boot0's own value `0x81000000` and the manual's maximum (UM
     4.1.4.3). The manual's rule for raising a bus clock sets the divider before
     the source (UM 4.1.4.3), and the update bit 27 applies the change (UM
     4.1.6.76).
   - *Change:* the shared platform start writes `0x81000000` with the update
     bit when it finds `0x84000000`, and the fabric checks expect 600 MHz.
   - *Approval:* a bus clock change with no voltage change, covered by the
     performance policy but outside the recorded approvals; record it before
     the first run.
   - *Board run:* the dump at points A and B on one port, then the ShuffleNetV2
     timing at 492 and 852 MHz with MBUS at 600 MHz for both starts.
2. **CPU clusters and the DSU to 1794, 2002 and 1274 MHz.**
   - *Reference:* vf.dtsi rows above and the vendor change order in
     `docs/designs/cpu-top-opp.md` on `work/cpu-top-opp-1025`.
   - *Change:* merge that branch, then enable the CPU clock driver in the OS
     image with a grant for the top points.
   - *Approval:* approved on 2026-10-24 (CPU clock and DCDC3 and DCDC5
     voltage).
   - *Board run:* that branch's bundle 20 (CPU-B to 1404 MHz at 800 mV), then
     the raised steps, each with cycle-counter timing of the measured rate.
3. **NPU default to 852 MHz.**
   - *Reference:* vf.dtsi:2069-2091.
   - *Change:* merge `work/npu-job-overhead-1025`.
   - *Approval:* approved on 2026-10-25.
   - *Board run:* the ShuffleNetV2 timing with MBUS held at 600 MHz.
4. **G2D to 300 MHz.**
   - *Reference:* dtsi:3383-3384.
   - *Change:* PERI0_300M ÷ 1 in the G2D session check and module write.
   - *Approval:* a clock change outside the recorded approvals; the voltage is
     unchanged.
   - *Board run:* the G2D application with identical output digests at 100 and
     300 MHz.
5. **Video engine and JPEG at 624 MHz.**
   - *Reference:* vf.dtsi:2169-2172.
   - *Change:* after the dump reads PLL_VE, program PLL_VE0 for 624 MHz and make
     624 the binding's default.
   - *Approval:* a clock change outside the recorded approvals.
   - *Board run:* the H.264 and JPEG encoder applications with frame timing.
6. **microSD SDR104 at 150 MHz.**
   - *Reference:* dts:1637, 1644.
   - *Change:* none; it is built.
   - *Approval:* approved on 2026-10-25.
   - *Board run:* the rerun of `docs/board-runbooks/sd-uhs.md` after the power
     removal. Its gain stays small until descriptor DMA replaces programmed
     I/O.
7. **SDIO Wi-Fi above 25 MHz.**
   - *Reference:* dts:1663, 1670.
   - *Change:* SDR50 or SDR104 on SMHC1 with tuning, once the module's
     supported modes are read from its CCCR.
   - *Approval:* a clock change outside the recorded approvals.
   - *Board run:* the radio throughput test at 25 MHz and at the raised rate.
8. **DRAM at 2400 MHz.** Not recommended now. No vendor parameter set exists
   for this board's strap at 2400 MHz, the part's rating is unknown, and a
   failed training ends the lifetime. Identify the part first.

PCIe Gen3 and SPI at 100 MHz stay unranked: PCIe is refused on the 26 MHz
oscillator before its clock rate matters, and no board SPI device needs more
than 24 MHz.

## Open questions

- Whether the ROM return or an image lowers MBUS; the dump's points A and B
  separate them.
- The rates of APB0, APB1, APB_UART, PLL_DDR, PLL_PERI1, PLL_VIDEO0–2 and
  PLL_VE0/1 that boot0 leaves; the dump reads them.
- The DSU mux word read as `0x03000105` (988 MHz) in one earlier port 3-3
  capture and `0x03010105` (494 MHz) in another.
- The LPDDR5 part number and rating.
- The microSD cards' UHS-I support and the Wi-Fi module's SDIO modes.
