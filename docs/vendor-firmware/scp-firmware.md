# The A733 SCP firmware runs system suspend, resume and power-off on the E902

The vendor's system control processor (SCP) firmware, `scp.fex`, runs on the
A733's E902 RISC-V core. In the vendor boot chain, U-Boot has BL31 start it
just before booting Linux, and it then waits for messages from the application
processors. It
performs four jobs: system suspend and resume, system power-off and reset
through the PMIC, wakeup-source configuration, and DRAM self-refresh with PHY
state retention. It has no frequency scaling, thermal or watchdog service.

SunstoneOS never starts this firmware. This document describes it so that
SunstoneOS can reimplement the parts it needs, avoid its register side effects,
and resolve questions about vendor power sequences without disassembling the
blob again. The [vendor firmware overview](README.md) records where the blob
comes from and how to fetch it.

## Citations and evidence labels

Citations and labels follow the
[overview's conventions](README.md#evidence-labels-and-citations). A citation
such as `07e6b976@0x5e9a` names the blob by the first eight hexadecimal digits
of its SHA-256 and a byte offset into the file; a bare `@0x5e9a` cites the same
blob, and `8af16b86@…` cites BL31. The offset is the E902 address minus
`0x40004000`. Each fact is labelled observed, inferred or confirmed on a board.
Here, inferred facts come from strings, register names in the A733 User Manual
V1.00 or vendor kernel sources, or data flow.

No SCP behaviour is confirmed on a board, because no SunstoneOS image starts
the E902. The section on SunstoneOS names the board readings that are
consistent with the firmware never having run.

The register names below come from the A733 User Manual V1.00 (cached as
`~/.cache/sunstoneos/sources/camera-research/A733_User_Manual_V1.00.txt`).
Names in quotation marks are the firmware's own log strings.

## Definitions

- **CPUS domain.** The always-on subsystem at `0x07000000`: E902, PRCM (S_PRCM
  in the manual's memory map, R_CCU in the vendor clock driver), RTC, S_TIMER, S_UART, S_TWI, S_MBOX and the PCK-600 power
  policy units (PPUs).
- **PPU row.** One PCK-600 power domain at `0x07060000 + 0x1000·n`. Offset 0
  (`PPU_PWPR`) requests a power policy; offset 8 (`PPU_PWSR`) reports the
  reached state, 8 meaning ON. Domain 4 is the NPU, 5 the GPU top (TOP5) and
  6 the GPU core (CORE6).
- **Message box.** The A733 MSGBOX hardware: a word FIFO per channel in each
  direction, 8 words deep (inferred from the firmware's full test). The CPUX side is `0x03004000`, the CPUS side (S_MBOX) is
  `0x07094000`.
- **Progress code.** A value the firmware writes to the RTC general-purpose
  register `GP_DATA_REG3` (`0x0709010c`) at each step, so that a crash leaves
  its last step readable from the application processors.

## Machine-readable inventories

Four JSON files hold the full inventories. Each starts with the blob name,
SHA-256 and source reference.

| File | Content |
| --- | --- |
| [scp-functions.json](data/scp-functions.json) | All 235 functions: address, offset, size, role, label, callers, callees, MMIO addresses |
| [scp-mmio.json](data/scp-mmio.json) | 390 statically resolved registers grouped by peripheral, with read/write, written values, functions and SunstoneOS source files that name the address; plus 17 table- or loop-computed access patterns |
| [scp-messages.json](data/scp-messages.json) | Message transport, header layout, every message type with payload, handler and BL31 sender |
| [scp-tables.json](data/scp-tables.json) | PLL, bus-clock and PPU tables, PMIC rail-switch table, wakeup interrupt map, operation tables, DRAM PHY lists, progress codes, error codes, device-tree properties |

The `sunstoneos_files` field is a text match of the register address in the
SunstoneOS sources at the time of writing; review a match before relying on it.
The unabridged versions, the annotated listing and the analysis scripts are
outside Git under `~/.cache/sunstoneos/evidence/vendor-firmware-20261025/scp/`
(`manifest.json` lists the commands).

## Identity and provenance

| Property | Value | Label and evidence |
| --- | --- | --- |
| Source | `https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/scp.fex` (orangepi-build revision `a95129bf30ea11886cc35f08ebb3e69695a26656`) | observed; fetching is described in the [README](README.md) |
| File | `scp.fex`, 105,912 bytes, SHA-256 `07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749` | observed |
| Core | T-Head E902 (A733 User Manual section 5.2, "Single E902, up to 200 MHz") | inferred from the manual and the start address that BL31 writes |
| Instruction set | RV32EMC with Zicsr: only registers x0–x15 occur, the trap entry saves the ten RV32E caller-saved registers, frames are 4-byte aligned, `mul`/`divu`/`remu` and compressed instructions occur | observed, `07e6b976@0x0`–`0x40`, `@0xc2`–`0xfe` |
| Custom CSRs | Reads of `0x7c0` and `0x7e1` (T-Head machine CSRs); `0x307` written as `mtvt` | observed, `07e6b976@0x1d4`–`0x1f0`, `@0x4c` |
| Link and load address | `0x40004000` in the E902 view | observed: the device-tree error-string pointers at `07e6b976@0xbac0` hold `0x4000fb84`…, which are the string offsets `0xbb84`… plus `0x40004000` |
| Physical location | SRAM A2 offset `0x4000` (CPUX address `0x00044000`); the E902 maps SRAM A2 at `0x40000000` | inferred from the manual's E902 memory map; BL31 copies `0x28000` bytes from DRAM `0x48100000` to `0x44000` (`8af16b86@0xaeb0`–`0xaebc`) |
| Version string | Empty: the 51 bytes copied into the start-up message and printed as "ar100 firmware version" begin with zero | observed, `07e6b976@0xc67c` |
| Origin | Derived from Allwinner's "arisc"/AR100 firmware and built for the E902; the log calls the application processors "AC327" | inferred from strings |

Some function listings from the CPU backsolve
(`~/.cache/sunstoneos/evidence/reference-backsolve-20261025/cpu/vendor-power/scp/f_*.dis`)
are named after table entries minus `0x40000000`. They start inside unrelated
code; the PMIC operations those entries point to are at offsets `0x3706`,
`0x36ce`, `0x366a` and `0x356e`.

### Memory map

| E902 address | Blob offset | Content | Label |
| --- | --- | --- | --- |
| `0x40004000`–`0x400040bf` | `0x0`–`0xbf` | Reset entry and an unused register-preset sequence | observed |
| `0x400040c0`–`0x4000f26b` | `0xc0`–`0xb26b` | Code | observed |
| `0x4000f26c`–`0x4000f2eb` | `0xb26c`–`0xb2eb` | Switch tables (relative offsets) | observed |
| `0x4000f2ec`–`0x4001085f` | `0xb2ec`–`0xc85f` | Strings, the wakeup interrupt map (`0xbc74`), the PMIC rail-switch table (`0xc774`) | observed |
| `0x40010860`–`0x400108bb` | `0xc860`–`0xc8bb` | Operation tables and initialised variables | observed |
| `0x400108bc`–`0x4001dc2b` | `0xc8bc`–`0x19c2b` | Three lists of 13,532 DRAM PHY register addresses | observed |
| `0x4001dc2c`–`0x4001ddb7` | `0x19c2c`–`0x19db7` | Initialised data ("CPUs" tag) | observed |
| `0x4001ddb8`–`0x4002af3b` | none | BSS (53 KiB), mostly the DRAM PHY save area at `0x4001e00c` | observed, cleared at `07e6b976@0x15c` |
| `0x4002ec00`–`0x4002efff` | none | Stack (1 KiB, top `0x4002f000`), zeroed at reset | observed, `07e6b976@0x26`–`0x3c` |
| `0x40000000`–`0x40003fff` | none | Unused CPUS-only part of SRAM A2 | inferred: no reference found |
| `0x40034000` and up | none | DRAM, identity-mapped; the firmware reads the device tree there | inferred from the manual |

The global pointer is `0x4001e430` (`07e6b976@0x1e`).

### Vector table and traps

The reset code writes the same address, `0x400040c0`, to `mtvec` and to the
CLIC vector-table register `mtvt`, then sets `mstatus.MIE`
(`07e6b976@0x40`–`0x52`, observed). The word at that address is a jump to the
register-saving trap handler, so every interrupt and exception enters one
routine (observed, `@0xc0`–`0xfe`). Hardware vectoring through `mtvt` would
need a table of addresses there; there is none (inferred).

The C dispatcher (`07e6b976@0x1330`, observed) routes interrupts to a 35-entry
handler table at `0x4002aca0`. Exception cause 24 calls the NMI handler; any
other exception returns to the faulting instruction. Interrupt enables are the
CLIC `CLICINTIE` bytes at `0xe0801001 + 4·irq` (`07e6b976@0x1f54`, observed).
The E902 interrupt numbers come from the manual's Table 12-2.

## Startup

BL31 starts the E902 when U-Boot issues SMC `0x8000ff10`, as described under
[the BL31 interface](#bl31-starts-the-firmware-and-sends-every-request). The
firmware's `main` (`07e6b976@0x7080`) then runs these steps in order. Each step
writes its progress code `0xb000`–`0xb00f` to `GP_DATA_REG3` (observed).

| Step | Registers and values | Label, citation |
| --- | --- | --- |
| 1. Read the device-tree address | Reads `GP_DATA_REG3` (`0x0709010c`), where BL31 stored it | observed `@0x7088`; BL31 write at `8af16b86@0xaeac` |
| 2. Open an address window over the device tree | CPUIDLE `0x07050010` = tree address, `0x07050014` = address + 1 MiB, bit 0 of `0x0705000c` set | observed `@0x14f4`; the log calls it "little-endian config"; purpose unknown |
| 3. Set the CPUS bus clocks | `AHBS_CLK` (`0x07010000`): divider 1, source PERIPLL_DIV (200 MHz). `APBS0_CLK` (`0x0701000c`): divider 2, source PERIPLL_DIV (100 MHz). `APBS1_CLK` (`0x07010010`): divider 1, source SYS_CLK24M. Each divider is written before its source | observed `@0x17f4`–`0x1840` |
| 4. Interrupt table | 35 entries set to a default handler | observed `@0x206c` |
| 5. Debug UART | PL2/PL3 function 2; `S_UART_BGR` (`0x0701018c`) gate and reset; 57,600 baud, 8N1, FIFOs on | observed `@0x27a0`–`0x2832` |
| 6. S_TWI0 for the PMIC | PL0/PL1 function 2 with pull-up; `S_TWI_BGR` (`0x0701019c`) gate and reset; clock divider; `TWI_CNTR` = `0x44`; checks idle status `0x3a` | observed `@0x2b96`–`0x2c3e` |
| 7. PMIC and charger | Marks the AXP8191 driver ready; probes an AXP515 charger at TWI address `0x34` by reading its type register 3 (no PMIC write) | observed `@0x3ae0`, `@0x3bc6`, `@0x391a` |
| 8. Message boxes | `MSGBOX0_BGR` (`0x02002744`) and `S_MBOX_BGR` (`0x0701017c`) gate and reset | observed `@0x317a` |
| 9. System counter frequency | `CNT_FREQID` (`0x08020020`) = 24,000,000 | observed `@0x14ce` |
| 10. Timers | `S_TIMER_BGR` gate and reset; S_TIMER0 and S_TIMER1 from 24 MHz, 24,000 counts per ms | observed `@0x2438` |
| 11. Standby service | Clears the DRAM checksum results; creates the wakeup software timer | observed `@0x5096` |
| 12. System tick | S_TIMER0 periodic, 10 ms | observed `@0x148e` |
| 13. Watchdog | Read-modify-write of `WDT_MODE` (`0x07021014`, bit 0 set) and `WDT_OCFG` (`0x07021018`) without the `0x16aa` key | observed `@0x3c18`; no effect inferred, see [watchdog](#the-watchdog-setup-has-no-effect) |
| 14. Start-up notification | Message type `0x90`, synchronous, 13 payload words (the empty version string) | observed `@0x71d2`–`0x7202` |
| 15. DRAM parameters | Reads `dram_para00`… from the `/dram` node into the DRAM parameter block | observed `@0x72e6`, `@0x34bc` |
| 16. Main loop | Polls S_MBOX for a message and dispatches it; every 500 ticks runs the daemon callbacks (stack usage report) | observed `@0x726a`–`0x72dc` |

At cold start the firmware writes none of the following (observed: no
instruction outside the suspend, resume, power-off and DRAM paths reaches them):

- the PPU rows, so no GPU, NPU or other power domain changes, and no PPU
  `PWCR` or delay register at any time;
- the GPU register window, the GPU clock words `0xb20`/`0xb24` and every CCU PLL;
- the CPU PLLs, CPU core power controls and GIC;
- any AXP8191 voltage register (`0x12`–`0x1a`) at any time;
- PRCM `0x250`, the DRAM controller and the DRAM PHY.

The main loop never executes `wfi`; the E902 spins while idle (observed). The
only `wfi` is in the suspend wait loop.

## Messages

### The firmware polls message-box channel 3

| Property | Value | Label, citation |
| --- | --- | --- |
| Application processor to SCP | CPUS message box channel 3: the sender writes `MSGBOX_MSG_REG` `0x0709407c`; the firmware polls `MSGBOX_MSG_STATUS` `0x0709406c` and clears read-interrupt bit 6 of `0x07094024` | observed `@0x33e0`–`0x3430` |
| SCP to application processor | CPUX message box channel 3: the firmware waits until `0x0300406c` is below 8, then writes `0x0300407c` | observed `@0x31b2`, `@0x321e` |
| Framing | Word 0 header, word 1 payload count (low byte), then the payload words | observed |
| Header | Byte 0 state, byte 1 attribute, byte 2 type, byte 3 result | observed `@0x6bde`–`0x6c30` |
| Synchronous messages | Attribute bit 1 set: the receiver sends the message back with byte 3 = result ("feedback"); a synchronous sender waits for it | observed `@0x6cd8`–`0x6cec`, `@0x333e` |
| Reception | Polling from the main loop; the message-box interrupt is never enabled | observed |
| Timeout | 100,000 tries of 1 ms per word | observed `@0x31b2`, `@0x31ea` |

### Message types

| Type | Name | Payload | Handler | Label |
| --- | --- | --- | --- | --- |
| `0x11` | CPU0 restore finished | none; the reply carries one word, the wakeup event | accepted only inside the resume wait, `@0x6930` | observed |
| `0x19` | Fake power-off | none | no action, result 0, `@0x6ac4` | observed |
| `0x22` | CPU operation | MPIDR, entry point, CPU state, cluster state, system state | `@0x55a0`; acts only when the entry point is non-zero and the system state is 3 | observed |
| `0x24` | System operation | state: 0 power-off after a VBUS check, 1 or 2 reset, 3 power-off | `@0x6a54` | observed |
| `0x25` | Clear wakeup source | wakeup word | `@0x5346` | observed |
| `0x26` | Set wakeup source | wakeup word (see [wakeup sources](#wakeup-sources)) | `@0x5248` | observed |
| `0x60` | Set debug mask | mask | `@0x7044` | observed |
| `0x61` | Loopback | up to 11 words | result 0 | observed |
| `0x62` | Set UART baud rate | baud | `@0x290e` | observed |
| `0x64` | Set DRAM checksum parameters | enable, start address, length | `@0x50e4` | observed |
| `0x96` | Not named | one word | result −1 without a log line | observed |
| other | none | none | logs "ERR:imt", dumps the message, result −3 | observed |

The firmware sends two kinds of message: the start-up notification `0x90` and
the feedback for synchronous requests. BL31 contains a sender for every type in
the table except `0x19` (`8af16b86@0x1904`–`0x21c8`, observed). Shared
memory is not used for messages. The device tree is the only shared structure,
and the firmware only reads it.

## Power flows

### System suspend

BL31 sends type `0x22` from its system-suspend path (`8af16b86@0x1948`;
[CPU backsolve](../reference-backsolve-cpu.md#idle-is-dsb-followed-by-wfi)).
The handler at
`07e6b976@0x55a0` then runs the steps below. Progress codes run from `0xf3f31000` to `0xf3f3300b`.

| Step | Registers and values | Label, citation |
| --- | --- | --- |
| 1. First suspend only | Reads `standby_param` from the device tree; builds the PLL and bus-clock tables | observed `@0x5604`, `@0x5662`–`0x5714` |
| 2. Oscillator measurement | Measures the RC16M oscillator against the timestamp counter by switching `AHBS_CLK` between DCXO and RC16M (once) | observed `@0x1710` |
| 3. USB wake test | If E902 interrupt 16 (USB standby wake) is enabled in the CLIC, standby type bit 13 is set | observed `@0x562e`–`0x5642` |
| 4. Core 0 and DSU | Clears core 0 power-mode bit 0 (`0x07050220`); waits without a bound until the cluster PPU status `0x07051008` leaves ON | observed `@0x16d0`; "cluster PPU" inferred |
| 5. State saves | USB port words `0x04101800`/`0x04200800` bit 31; system counter `0x08010000`/`4` and `CNT_FREQID`; RTC day and time; eight SMC0 secure-region words `0x0a000100`–`0x0a00017c` | observed `@0x5734`–`0x581c` |
| 6. Wake preparation | `NMI_INT_EN` (`0x07010324`) bit 1 set; S_TWI0 re-initialised; message boxes closed | observed `@0x5820`–`0x5828` |
| 7. CPU clocks off | `CPU_L/B/DSU_PLL_CLK` (`0x0887n01c`): source field 2, factor P cleared; then `0x08870000`–`0x08873000` bits 27, 29, 31, 30 cleared | observed `@0x5844`–`0x5900` |
| 8. DRAM checksum | If message `0x64` enabled it, an additive 32-bit sum over the range | observed `@0x590e`–`0x592c` |
| 9. DRAM | Self-refresh and retention, see [DRAM](#dram-self-refresh-and-phy-retention) | observed `@0x5938` |
| 10. Bus clocks | Saves `CPU_DA_DDR_CTRL` (`0x08000200`); `RISCV_24M_CLK` = `0x82000000`; CCU `AHB/APB0/APB1/APB_UART_CLK` to source 2 with divider 0; `NSI_CLK`, `MBUS_CLK`, `GIC_CLK`, `TRACE_CLK` gated; UART and TWI dividers recomputed; PRCM `AHBS/APBS0/APBS1` to RC16M | observed `@0x5948`–`0x59c2` |
| 11. PLLs off | Each of the 14 PLL words in the table saves bits 24:0 except bit 7, then clears bits 31, 30 and 29 | observed `@0x59ec`–`0x5a2c` |
| 12. Auto-gating | `BUS_ACG` (`0x0701033c`) bits 24 and 0 cleared; `DEV_BUS_AUTOG_CTRL` (`0x07010338`) \|= `0xf01`; `MSRAMOC_CTRL` (`0x07010360`) bit 0 set; CPUIDLE `0x07050000` bit 0 cleared. Resume does not restore these | observed `@0x5a30`–`0x5a68` |
| 13. 24 MHz oscillator | Only if `osc24m-on` is 0 and `XO_CTRL` bit 31 is set: PRCM `0x244` bit 0 cleared under key `0xa7`; `XO_CTRL` bit 1 cleared under key `0x16aa`; low-speed clock moved to the internal source; timers moved to 32 kHz | observed `@0x5a76`–`0x5a82`, `@0x1ba8`, `@0x236e` |
| 14. Isolation and gates | PRCM `0x254` \|= `0xf`; PRCM `0x250` \|= `0x108` (bits 3 and 8); `0x260` bit 0 cleared; `0x250` \|= `0x4` (bit 2) | observed `@0x5aac`–`0x5b70` |
| 15. RTC power gate | `VDD_OFF_GATING_CTRL` (`0x070901f4`) bit 0 set, bit 7 cleared | observed `@0x5b98`–`0x5ba4` |
| 16. Rails off | For each bit in the masks `vcc-io`, `vcc-efuse`, `vdd-cpub`, `vdd-cpu`, `vdd-sys`, `vcc-pll`, in that order, clears the AXP8191 enable bit from the rail-switch table | observed `@0x5bac`–`0x5c5a`, table `@0xc774` |
| 17. Wait | `wfi` until an interrupt or the software timer records a wakeup event | observed `@0x5c98`–`0x5ca2`, `@0x6410` |

With standby type bit 13 set (USB wake), step 14 leaves bit 8 clear. It
instead gates the USB0 and USB1 clocks (`0x02003300`–`0x0200330c`,
`0x02003340`) and asserts their resets, for each port that reported bit 31 in
step 5 (observed `@0x615a`–`0x640c`). The vendor clock driver names PRCM `0x250`
bit 0 `vdd-ddr`, bit 2 `vdd-sys2cpus`, bit 3 `vdd-sys2usb` and bit 8
`vdd-usb2cpus` (`ccu-sun60iw2-r.c`). That they isolate the VDD-SYS domain from
the always-on and USB domains is inferred.

### Resume

| Step | Registers and values | Label, citation |
| --- | --- | --- |
| 1. Rails on | Sets the enable bits for `vcc-pll`, `vdd-sys`, `vdd-cpu`, `vdd-cpub`, `vcc-efuse`, `vcc-io`, in that order | observed `@0x5cd2`–`0x5d88` |
| 2. RTC power gate | `VDD_OFF_GATING_CTRL` bit 0 cleared, bit 7 set | observed `@0x5da6`–`0x5db2` |
| 3. 24 MHz oscillator | If it was stopped: restores the low-speed clock, sets `XO_CTRL` bit 1 and PRCM `0x244` bit 0 | observed `@0x5dce`, `@0x1c44` |
| 4. Isolation and gates | `0x250` bit 2 cleared; `0x260` bit 0 set; `0x254` low nibble cleared; then `0x250` bits 3 and 8 cleared (mask `~0x108`); bit 0 is not changed here | observed `@0x5dee`–`0x5e04`, `@0x5e9a` |
| 5. All power domains on | For rows 0 to 10: `PPU_PWPR` \|= 8; then polls `PPU_PWSR` bit 3 without a bound for every row except row 6 (CORE6) | observed `@0x5ec6`–`0x5ee8` |
| 6. PLLs | Each PLL: restore the saved factors with bit 27 clear, set bits 31 and 30, set bit 29, poll lock bit 28 without a bound; after all, wait 20 µs and set bit 27 on each | observed `@0x5f0a`–`0x5f80` |
| 7. Bus clocks | TWI and UART dividers for 24 MHz; PRCM `AHBS/APBS0/APBS1` restored; `GIC_CLK`, `TRACE_CLK`, `NSI_CLK`, `MBUS_CLK` bit 31 set; CCU bus clocks restored | observed `@0x5f84`–`0x5fdc` |
| 8. DRAM | Restores `CPU_DA_DDR_CTRL`; re-initialises DRAM; checks the checksum and spins on a mismatch (code `0xf1f1900f`) | observed `@0x5ff6`–`0x6052` |
| 9. Interconnect | NSI words `0x0202446c` and `0x0202466c` = `0x40a05`; `0x02023a14` = `0x3f`; `0x02023c14` = `0x7f`; `0x02023e14` = `0xf`; `0x02024014`, `0x02024214` = `0xf1`; `0x0202200c`, `0x0202220c` = 1 | observed `@0x66d4`–`0x6714`; meaning unknown |
| 10. CPU clocks | Each CPU PLL: bits 30, 27, 31, 29, 26 set, poll lock; then restore factors, re-lock, wait 20 µs, set bit 27; then restore each mux source and factor P | observed `@0x672a`–`0x6892` |
| 11. Message boxes and NMI | Message boxes re-opened; NMI disabled | observed `@0x689a`–`0x68a2` |
| 12. Memory security | If SID word `0x030060a0` bit 0 is set: refuses to continue when SMC0 `+4` bit 6 is set; otherwise writes SMC0 and SMC1 `+0x70`/`+0x74` = 0, `+0x80`/`+0x84` = `0xffffffff`, `+0x18` = `0x90`, and restores the eight saved regions to both | observed `@0x68a6`, `@0x69a2`–`0x6a44` |
| 13. System counter | `TSTAMP_CTRL` = 0; `CNT_CTRL_LOW/HI` = saved count + elapsed RTC seconds × frequency; `TSTAMP_CTRL` = 1 | observed `@0x68c2`–`0x6900`; the count advancing by the sleep time is inferred |
| 14. Core 0 | `CLU0_RVBARADDR0_L` = entry point, `_H` = 0, `CLU0_CPU0_CTRL` bit 0 set; hotplug-enable and power-mode bit 0 set, poll core 0 PPU for ON without a bound, hotplug-enable cleared | observed `@0x6914`, `@0x16ec`, `@0x1678` |
| 15. Handshake | Waits for message `0x11`, ignoring others; replies with the wakeup event | observed `@0x6930`–`0x696e` |

Step 5 powers on every PCK-600 domain, including the NPU (row 4, polled) and
both GPU domains, whatever their state before suspend. It writes only the
policy word: never `PPU_PWCR` or a delay register. The skipped poll is the only
treatment specific to CORE6 (observed). Linux turns unused domains off again
afterwards (inferred). The same loop is item 4 under "Boot firmware" in the
[NPU backsolve](../reference-backsolve-npu.md#boot-firmware), which leaves its
caller unidentified; the caller is this resume path.

The resume path never writes the GPU register window, the GPU clock words or a
DCDC voltage (observed). The
[GPU backsolve](../reference-backsolve-gpu.md#3-the-scp-firmware-touches-gpu-state-only-on-resume-from-suspend)
draws the GPU consequences.

### PRCM 0x250 across the flows

| Bit | Vendor driver name | Cold start | Suspend | Resume | DRAM path |
| --- | --- | --- | --- | --- | --- |
| 0 | `vdd-ddr` | unchanged | cleared at DRAM suspend `@0x994e` | unchanged at `@0x5e9a` | set during DRAM re-initialisation `@0x7644`, `@0x909c`, `@0x9176` |
| 2 | `vdd-sys2cpus` | unchanged | set `@0x5b70` | cleared `@0x5dee` | none |
| 3 | `vdd-sys2usb` | unchanged | set `@0x5b2c` (also in the USB branch) | cleared `@0x5e9a` | none |
| 8 | `vdd-usb2cpus` | unchanged | set `@0x5b2c` unless USB wake is enabled | cleared `@0x5e9a` | none |

All entries are observed.
[gpu-power.md](../gpu-power.md#what-the-vendor-code-does-around-the-core6-write)
says the resume instruction clears bits 0, 3 and 8. It clears bits 3 and 8; bit 0 is cleared and
set again only by the DRAM code.

### Power-off and reset

Message `0x24` re-initialises S_TWI0, waits for the bus to become idle, and
calls one of four PMIC operations from the table at `07e6b976@0xc86c`
(observed):

- **Power-off.** AXP8191 at TWI address `0x36`, register `0x55` bit 7 set; the
  firmware then spins (`@0x35ee`–`0x3658`, observed). Register `0x55` is
  `AXP8191_POWER_DISABLE_POWER_DOWN_SEQUENCE` in the vendor `axp2101.h`; bit 7
  meaning power-off is inferred from the log string.
- **Reset.** The same register, bit 6 (observed; meaning inferred).
- **VBUS check.** State 0 first asks the AXP515 charger whether VBUS is present;
  if so, it writes `0x08` to AXP8191 register `0x04`, resets the charger and
  restarts instead of powering off (`@0x366a`, observed; purpose inferred).
- **Rail switch.** Sets or clears one AXP8191 enable bit by index: registers
  `0x10` bits 0–7, `0x11` bits 0, 3, 4, and `0x20`–`0x23` (39 entries,
  `@0xc774`, observed). These are `DCDC_CTL1/2` and `LDO_POWER_ON_OFF_CTL1-4`
  in `axp2101.h`.

### Frequency scaling and thermal management are absent

The firmware has no frequency-scaling or thermal code (observed): no message
changes a clock rate or voltage, no access reaches the thermal sensor at
`0x02522000`, and no PMIC voltage register is written. The CPU, DSU and PLL
changes occur only inside suspend and resume. The DRAM driver has a frequency-scaling table ("DRAM DFS VERSION INFO: V0.5").
It uses the table only while it re-initialises DRAM.

## Wakeup sources

Message `0x26` takes one word (observed, `07e6b976@0x5248`):

- **Bits 31:30 = 3.** Bits 29:0 are a timeout in ms. The 10 ms tick decrements
  it and records wakeup event `0xc0000000`.
- **Otherwise.** Bits 9:0 are an application-processor interrupt number minus
  32. The firmware looks it up in the table below, installs its wakeup handler
  on the E902 interrupt, enables it in the CLIC and sets its bit in
  `E902_WAKEUP_MASK0` or `MASK1` (`0x07032064`/`68`). When it fires, the
  handler disables it and records the interrupt number minus 32 as the event.

| E902 interrupt | Source (manual Table 12-2) | GIC INTIDs it represents |
| --- | --- | --- |
| 16 | `USB0_USB1_STB` | 189, 190, 191, 192 |
| 24 | `RTC_ALARM` | 228 |
| 25 | `GPIOL_S` | 229 |
| 26 | `GPIOL_NS` | 230 |
| 27 | `GPIOM_S` | 231 |
| 28 | `GPIOM_NS` | 232 |
| 34 | `S_IRRX` | 238 |

The table is at `07e6b976@0xbc74` (observed). An NMI also ends the wait with
event `0xbc` (observed, `@0x3afe`); its source pin is not identified. The
firmware does not program the RTC alarm itself: the application processors
set it, and the firmware only routes the alarm interrupt (observed: no write to
the alarm registers `0x07090020`–`0x07090050`).

## Timers, watchdog, debug output and interrupts

### Timers

S_TIMER0 provides the 10 ms tick; S_TIMER1 provides millisecond delays by
polling its pending bit (observed, `07e6b976@0x148e`, `@0x2676`). Microsecond
delays use the system timestamp counter `0x08010000`, or a CPU loop scaled by
the bus clock while the 24 MHz oscillator is off (observed, `@0x274a`).

### The watchdog setup has no effect

The S_WDG initialisation writes `WDT_MODE` and `WDT_OCFG` without the `0x16aa`
key that the manual (section 8.3.6) requires for these registers (observed,
`07e6b976@0x3c18`–`0x3c50`). The feed and disable routines that follow it have
no caller. The writes are therefore ignored and the CPUS watchdog stays
disabled (inferred). The write pattern matches an older Allwinner layout in
which these offsets needed no key (inferred).

### Debug output

- **UART.** S_UART0 on PL2/PL3 at 57,600 baud; message `0x62` changes the rate
  (observed).
- **Log levels.** Each line carries a level bit; it prints when the bit is
  within the debug mask, default 3, set by message `0x60` (observed,
  `@0x6e0a`, mask at `@0xc8a4`). Lines start with a `[seconds.ms]` tick stamp.
- **Progress codes.** `GP_DATA_REG3` holds the last step:
  `0xb000`–`0xb00f` start-up, `0xf3f3xxxx` suspend and resume,
  `0xa1xx`–`0xa5xx` power operations. The list is in
  [scp-tables.json](data/scp-tables.json).

### Interrupts in use

The firmware enables only the S_TIMER interrupts and the wakeup interrupts it
is asked for (observed). It never enables the message-box, UART or TWI
interrupts. Its S_INTC write routine (`0x070240c0`) sits behind a test that
compares a 10-bit field with −32 and therefore never runs (inferred).

## DRAM self-refresh and PHY retention

The firmware contains a complete DRAM controller driver, which reports itself
as version V0.5 (observed, `07e6b976@0x945c`):

- **Suspend** (`@0x9522`). Puts both DRAM channels (`0x0a100000`,
  `0x0a500000`) into self-refresh. On the first suspend only, it saves PHY state to SRAM.
  The state comprises one of two register lists (4,441 or 5,717 entries,
  selected by DRAM parameter bit 2), a third list of 3,374 entries, and the
  32 KiB PHY memory at `0x0aa40000`. It then clears PRCM `0x250` bit 0 and
  turns off `PLL_DDR`, `DRAM0_CLK`, `MBUS_CLK` and `NSI_CLK` and their resets
  (observed; the list roles are inferred from the save/restore pairs).
- **Resume.** Re-initialises the controllers from the device-tree parameters
  and restores the saved PHY state (observed, `@0x945c`, `@0xab7c`).

These lists are why the blob is large: 13,532 register addresses fill
54 KiB, and the save area takes most of the BSS.

## BL31 starts the firmware and sends every request

| Interaction | Mechanism | Label, citation |
| --- | --- | --- |
| Start | SMC `0x8000ff10` from U-Boot: BL31 writes the device-tree address to `GP_DATA_REG3`, copies `0x28000` bytes from `0x48100000` to SRAM A2 `0x44000`, writes `RISCV_BGR` = `0x10003`, `E902_RST_START_ADDR` = `0x40004000`, sets `0x07050008` bit 0 and `RISCV_24M_CLK` bit 31 | observed, `8af16b86@0xae88`–`0xaf9c`; recorded in [cold-entry.md](../cold-entry.md#remote-processor-exclusion-needs-the-effective-a733-register-map) |
| Requests | BL31 builds every message type of the table above and polls the same channel-3 registers | observed, `8af16b86@0x1904`–`0x21c8`, `@0x19f0` |
| System suspend | BL31 sends `0x22` asynchronously, then the firmware powers core 0 back on at the entry point BL31 gave | observed |
| Resume handshake | BL31 sends `0x11` synchronously and stores the returned wakeup word | observed, `8af16b86@0x1994` |
| Shared registers | The firmware overwrites `GP_DATA_REG3` with progress codes after reading the device-tree address | observed |

## SunstoneOS's use

### SunstoneOS relies on none of it

SunstoneOS boots through FEL with its own EL3 monitor and never issues the SMC
that starts the E902 ([boot.md](../boot.md), [cold-entry.md](../cold-entry.md)).
The planned microSD boot package omits `scp.fex`
([microsd-boot-package.md](../microsd-boot-package.md)). No SunstoneOS code
sends a message to the firmware.

### SunstoneOS reimplements these functions

| Firmware function | SunstoneOS implementation |
| --- | --- |
| Core power-on through CPUIDLE and the core PPUs (resume step 14) | [cpu-start.md](../cpu-start.md), [cpu_continue.c](../../platform/a733/cpu_continue.c) |
| PCK-600 domain control (resume step 5) | [power-domains.md](../power-domains.md), [a733_power_domains_native.c](../../drivers/power/a733_power_domains_native.c) |
| AXP8191 access over S_TWI0 | [pmic-runtime.md](../pmic-runtime.md), [a733_pmic.c](../../drivers/power/a733_pmic.c) |
| CPU PLL changes (the firmware changes them only for suspend) | [cpu-clock.md](../cpu-clock.md), [a733_cpu_clock.c](../../drivers/clock/a733_cpu_clock.c) |
| Idle | [cpu-power.md](../cpu-power.md): cores wait in WFI; SunstoneOS has no system suspend |
| RTC alarm | [rtc-application.md](../rtc-application.md): read and a non-waking alarm |
| Watchdog | [watchdog.md](../watchdog.md): SunstoneOS uses `WDT0` at `0x02050000`; the firmware's S_WDG setup has no effect |

### SunstoneOS must avoid these conflicts if the firmware ever runs

If a future boot chain keeps the vendor BL31 and starts the E902, the firmware
owns resources that SunstoneOS also uses:

- **CPUS bus clocks.** Its cold start sets `APBS0_CLK` to source 3
  (PERIPLL_DIV). The PMIC provider admits only source 0 or 4 and would refuse
  ([pmic-runtime.md](../pmic-runtime.md), "Controller ownership precedes
  transactions").
- **S_TWI0, PL0/PL1, S_UART0 and PL2/PL3.** The firmware initialises them at
  start and re-initialises S_TWI0 before power-off.
- **S_TIMER0/1 and the message boxes.** It runs its tick and delays on the
  timers and polls S_MBOX channel 3.
- **RTC `GP_DATA_REG3`.** BL31 and the firmware use it; SunstoneOS's FEL flag
  is `GP_DATA_REG2` (`0x07090108`), so the two do not overlap.
- **Remote-writer exclusion.** The E902 can write DRAM. The
  [remote-writer check](../remote-writers.md) and
  [remote_writers.c](../../platform/a733/remote_writers.c) accept the E902 only
  with its start bits clear.

## New insights

Each item changes or extends a SunstoneOS record:

- **The 0x250 claim in [gpu-power.md](../gpu-power.md) needs correcting.** The
  resume instruction clears bits 3 and 8; bit 0 belongs to the DRAM code (see
  the table above).
- **The NPU domain loop has an identified caller.** It is the system-resume
  path for message `0x22` with system state 3, so it never runs at cold boot
  (extends item 4 of the NPU backsolve's boot-firmware list).
- **The vendor firmware never writes CORE6's `PWCR` or delay row.** It
  requests ON for every row and does not wait for CORE6. This agrees with
  [gpu-power.md](../gpu-power.md), which finds no vendor path that writes
  CORE6's delay row.
- **Board readings are consistent with the firmware never running.** On a
  FEL-booted board PRCM `0x250` reads 1 (the GPU delay-row probe run, image
  `ade71834…`, in [gpu-power.md](../gpu-power.md)). The PMIC
  provider's board run admitted the inherited APBS0/APBS1 routes
  (`pmic-provider-probe-20261018`). The firmware's cold start would have set
  APBS0 to source 3 (inferred).
- **A board run confirmed the vendor GPU state that the GPU backsolve
  derived.** With that state applied, CORE6 reached ON and the GPU
  core identity read `36.56.104.183` (confirmed on a board,
  `~/.cache/sunstoneos/evidence/board-session-20261025-4/port-3-1/s10-gvs-run/`).
  The firmware's cold start touches no GPU state, which is consistent with
  that result: the state that produced the identity read needs no SCP action.
- **The firmware gives the system counter continuity across suspend.** It
  rewrites `CNT_CTRL` from the RTC. SunstoneOS images rely on a continuous
  24 MHz counter ([a733-smp.md](../a733-smp.md)); a SunstoneOS suspend design
  that removes VDD-SYS must restore it the same way.
- **System suspend needs DRAM PHY retention code.** Self-refresh alone is not
  enough when VDD-SYS is removed: the vendor saves 13,532 PHY registers and
  32 KiB of PHY memory. The [hardware matrix](../hardware.md) entry for always-on
  processors does not list this dependency.
- **The vendor E902 firmware idles in a polling loop.** It never executes
  `wfi` outside suspend, so it draws E902 power continuously while the system
  runs (inferred). This matters if SunstoneOS ever runs its own E902 service.
- **The CPUS watchdog is not armed by the vendor stack.** A SunstoneOS recovery
  design cannot assume an armed S_WDG from the vendor boot chain
  ([watchdog.md](../watchdog.md)).

## Open questions

- The purpose of the CPUIDLE window at `0x07050010`–`0x07050014`, and the
  reason the firmware opens one over the device tree, are unknown.
- PRCM `0x244`, `0x254` and `0x260`, and CPUIDLE `0x07050000` bit 0, are
  absent from the manual; their functions are inferred only from the vendor
  clock driver or not at all.
- The configuration set by the NSI writes in resume step 9 is unknown.
- The pin or device that drives the NMI ending a suspend with event `0xbc` is
  unidentified.
- Whether the Orange Pi Zero 3W carries the AXP515 charger that the power-off
  path probes is unverified.
- AXP8191 register `0x55` bit 7 as power-off and bit 6 as restart rests on log
  strings only.
- Message type `0x96`, which BL31 sends and the firmware rejects, is unnamed.
- The receiver of the start-up notification `0x90`, and whether the boot chain
  waits for it, are unknown.
- That the hardware ignores the keyless `WDT_MODE` write follows from the
  manual only. A read of `0x07021014` after a vendor boot would confirm it.
