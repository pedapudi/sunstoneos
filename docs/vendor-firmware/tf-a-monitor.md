# Vendor TF-A BL31 (`monitor.fex`) reference

The Orange Pi board package for the A733 ships one EL3 firmware image,
`monitor.fex`. It is Allwinner's build of Trusted Firmware-A (TF-A) BL31, the
resident EL3 runtime that U-Boot and Linux call through SMC instructions. This
document records the image's structure and behaviour, so that SunstoneOS work
can use it without disassembling the image again. SunstoneOS does not run this
image: it boots through FEL with its own EL3 monitor. The vendor image is a
reference for the work that monitor must do.

The fetch reference and provenance record for every vendor binary is
[the vendor firmware index](README.md). Repository rules are in
[`AGENTS.md`](../../AGENTS.md), and recurring references are listed in
[the reference index](../reference-index.md). Architectural requirements on
EL3 and GIC setup are in
[the multicore reference audit](../a733-multicore-reference-audit.md). The
step-by-step comparison of CPU bring-up with this image is in
[the CPU backsolve](../reference-backsolve-cpu.md). The SunstoneOS thermal and
GPADC service described in [`monitor-application.md`](../monitor-application.md)
is unrelated; it shares only the word "monitor".

## How facts are cited and labelled

Every citation gives the first eight hexadecimal digits of the image's
SHA-256 and a file offset, for example `8af16b86@0xc3b8`. The image loads at
`0x48000000`, so an offset is the address minus `0x48000000`. Table columns
headed "Offset" hold offsets in the same image.

Each fact carries one of four labels:

- **Observed**: read directly from the instructions or data of the image.
- **Upstream**: the code has the structure of the named function in TF-A
  v2.5 (commit `c1588782`). The image's assertion messages quote file names
  and line numbers identical to that release. The newer TF-A pin
  `0892ad4d` and the experimental A733 port `5bc9605c` are cited where they
  differ.
- **Board**: confirmed on an A733 board; the evidence record is named.
- **Inferred**: a conclusion from observed facts that the image alone does
  not prove.

Machine-readable inventories accompany this document:

| File | Contents |
| --- | --- |
| [`data/tf-a-monitor-functions.json`](data/tf-a-monitor-functions.json) | All 536 functions: offset, size, name, role, label, upstream match, callers, callees, system registers written and MMIO addresses |
| [`data/tf-a-monitor-mmio.json`](data/tf-a-monitor-mmio.json) | Every constant MMIO access, grouped by peripheral with the manual's memory map |
| [`data/tf-a-monitor-smc.json`](data/tf-a-monitor-smc.json) | Runtime-service descriptors and every SMC function ID with its handler |
| [`data/tf-a-monitor-tables.json`](data/tf-a-monitor-tables.json) | Boot0 header fields, memory sections, EL3 mappings, PSCI ops, power-domain tree, CPU ops, errata, GIC data, switch tables, SCP interface |

The full inventories, the analysis scripts and the listing they were built
from are outside Git, in
`~/.cache/sunstoneos/evidence/vendor-firmware-20261025/monitor/`. Rebuild
the committed files with `curate.py` in that directory.

## Identity and provenance

| Property | Value | Basis |
| --- | --- | --- |
| File | `monitor.fex`, 78,609 bytes | Observed |
| SHA-256 | `8af16b86ae8e2eff634a3ebca76a8cdf3c10b3540387573d46c3d785545b2632` | Observed |
| Source | [orangepi-build `a95129bf`](https://github.com/orangepi-xunlong/orangepi-build/blob/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/monitor.fex), `external/packages/pack-uboot/sun60iw2/bin/` | [Vendor firmware index](README.md) |
| Version | `v2.5(debug):7dddb263a-dirty`, built 21:40:11, Dec 30 2024 | Observed, `8af16b86@0xc8`, `@0xf3aa`, `@0xf3c8` |
| Base | TF-A v2.5, debug build, with Allwinner's platform `plat/allwinner/sun60iw2p1` | Upstream (assertion paths and lines) |
| Load address | `0x48000000`; the header word at offset `0x2c` holds it | Observed |
| Build options | `RESET_TO_BL31`: the cold entry discards x0 to x3 (`@0x1cc`); log level 20 (NOTICE, `@0x132fc`); OP-TEE dispatcher built in; no coherent memory section | Observed, upstream `bl31_entrypoint` |

### License terms

No term was found that forbids reverse engineering or disassembly of this
image. Four sources were checked:

- **The image.** It contains no license, copyright, "proprietary" or
  "confidential" notice. A search of its bytes for those words, and for
  "reverse" and "disassembl", finds nothing.
- **The containing repository.** orangepi-build `a95129bf` has one root
  `LICENSE`, the GNU General Public License version 2 (SHA-256
  `39db8f9acf036595a2566ea3fe560bc7bd65d8749f088e0f4a4ef2f8a6cb4b34`, fetched from
  `raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf…/LICENSE`).
  The repository's README states no other licensing.
- **The binary's directory.** The repository tree lists no license, notice or
  README file under `external/packages/pack-uboot/` or
  `external/packages/pack-uboot/sun60iw2/`. The cached tree listing is
  `~/.cache/sunstoneos/sources/orangepi-build-tree.txt`.
- **The source the image is built from.** Upstream TF-A v2.5 is distributed
  under the BSD-3-Clause license (`docs/license.rst` at `c1588782`). That
  license sets conditions on redistribution and places no restriction on
  analysis. Allwinner's platform additions carry no separate terms in the
  image or in the repository.

The fetched license and README copies are in
`~/.cache/sunstoneos/sources/orangepi-build-license/`.

The first 0x108 bytes are a boot0 header: a branch to `0x108`, the magic
`monitor`, the tag `CHKv1.0` at `0x1c`, the load word at `0x2c` and the
version string at `0xc8`. Boot0 fills further header words before it jumps to
BL31; their meaning is in [the boot0 header fields](#boot0-header-fields-select-the-next-images).
SunstoneOS's [cold-entry recorder](../cold-entry.md) captures the same
264-byte header for its own entry.

## Memory map

BL31 occupies `0x48000000`–`0x48048000`. Its layout follows the upstream BL31
linker script. The C functions appear in name order because the script sorts
`.text.<function>` sections (observed). This order bounds the name of every
unknown function by its neighbours.

| Region | Start | End | Contents | Basis |
| --- | --- | --- | --- | --- |
| Header | `0x48000000` | `0x48000108` | Boot0 header | Observed |
| Code | `0x48000108` | `0x4800e0d4` | Entry point first, then sorted functions | Observed |
| Vectors | `0x4800e800` | `0x4800f000` | `runtime_exceptions`; VBAR_EL3 is written at `@0x12c` and `@0x23c` | Observed |
| Read-only data | `0x4800f000` | `0x48013000` | PSCI arguments `@0xf000`, GIC driver data `@0xf010`, platform PSCI ops `@0xf040`, static EL3 mappings `@0xf158`, switch tables, strings, service descriptors `@0x127a0`, CPU ops `@0x12820` | Observed |
| Data | `0x48013000` | `0x48013311` | USB device operations, translation context `@0x13040`, USB0 base word, log level, console state | Observed |
| Stacks | `0x48013340` | `0x4801b340` | 4 KiB per core for 8 cores; the core index is MPIDR bits 15:8 (`@0x36a8`) | Observed |
| BSS | `0x4801b380` | `0x48041380` | Per-CPU data first (`0x80` bytes per core, `@0x26d8`), then entry-point records, GIC save areas, SCP buffers | Observed |
| Translation tables | `0x48041300` | `0x48048000` | One 16-entry level-1 table at `0x48041300`, the last bytes of BSS, and six 4 KiB tables at `0x48042000` | Observed |

The EL3 translation regime uses a 4 KiB granule, a 34-bit virtual address
space and level-1 base tables; MAIR_EL3 is `0x4404ff` (observed,
`8af16b86@0xaab0`, `@0x33f8`). `sunxi_configure_mmu_el3` (`@0xb20c`) maps
the static list at `@0xf158` and then the code and read-only data:

| Physical and virtual address | Size | Attributes | Basis |
| --- | --- | --- | --- |
| `0x00000000` | 1 GiB | Device, read-write, Secure, execute-never | Observed |
| `0x40000000` | 8 GiB | Normal memory, read-write, Nonsecure, execute-never | Observed |
| `0x48000000` | 2 MiB | Normal memory, read-write, Secure, execute-never | Observed |
| `0x48200000` | 16 KiB | Device, read-write, Secure | Observed; purpose unknown |
| `0x48000000` | `0xf000` | Code, read-only, executable | Observed |
| `0x4800f000` | `0x4000` | Read-only data | Observed |
| `0x48200000` | 14 MiB | Secure read-write; mapped only when the header holds an OP-TEE entry | Observed |

All mappings are flat. BL31 programs no DRAM security region. It reads the
regions that boot0 left in the SMC0 DRAM security controller and prints them
(`8af16b86@0xd53c`, observed).

## The image contains five groups of code

The 536 functions are listed with their roles in
[`data/tf-a-monitor-functions.json`](data/tf-a-monitor-functions.json). The
labels are 339 upstream, 83 vendor, 107 inferred and 7 unknown.

| Group | Offsets | Contents | Basis |
| --- | --- | --- | --- |
| Generic TF-A | entry `0x108`, assembly `0x26d8`–`0x3c18`, C throughout | Entry, context management, PSCI library, GICv3 driver, translation tables, runtime services, libfdt, libc, console, crash reporting, OP-TEE dispatcher | Upstream v2.5 |
| CPU support | `0x2b8`, `0x2aa0`–`0x2dec` | Cortex-A55 and Cortex-A76 reset, power-down and errata report | Upstream v2.5 |
| Allwinner platform | `0x3c1c`–`0x3e40`, `0x4228`, `0x80bc`–`0x8920`, `0xc224`–`0xc4c4`, `0xd53c` | Platform setup, power ops, CPU power helpers, security setup, console setup, SMC0 report | Vendor |
| SCP and SMC services | `0x1830`–`0x2304`, `0xae88`, `0xbd58`–`0xc224` | Vendor SMC IDs, SCP message box, SCP loader, eFuse keys, activity monitors | Vendor |
| USB device stack | `0x334`–`0x1830`, `0xb2c8`–`0xbd58`, `0xc598`–`0xd2d8`, `0xd660`–`0xd814` | A copy of Allwinner's U-Boot USB device and "efex" flashing protocol | Inferred from structure and strings |

The exception vectors at `0x4800e800` follow upstream `runtime_exceptions`
(observed). An SMC from a lower exception level goes through the
descriptor index at `0x48040cdc` to the runtime services listed in
[the SMC section](#four-runtime-service-descriptors-dispatch-every-smc). A lower-level
IRQ or FIQ goes to the handler registered for its type, and returns when none
is registered. An external abort prints its syndrome, a backtrace and the
registers on UART0, then halts (`8af16b86@0x82a4`, `@0x31d8`, upstream
`plat_ea_handler` and `el3_panic`). The crash console is UART0 at
`0x02500000`, 24 MHz, 115200 baud (`@0x3664`).

## Cold boot runs eight platform steps

`bl31_entrypoint` (`8af16b86@0x108`, upstream) sets SCTLR_EL3 to
`0x30c50830`, installs the vectors, runs the CPU reset function, then sets
SCTLR_EL3.I, A and SA, SCR_EL3 = `0x238`, MDCR_EL3 = `0x818000`, PMCR_EL0 =
`0xe0` and CPTR_EL3 = 0 (observed). It invalidates the read-write region,
clears BSS, selects the core's stack and calls `bl31_setup` and `bl31_main`.
The register values match upstream `el3_entrypoint_common` (upstream).

The platform steps run in this order:

1. **Console** (`bl31_early_platform_setup2`, `@0x3f78`; console setup
   `@0xc4c4`, vendor). Bits 15:0 of `UART0_BGR_REG` select the first enabled
   UART. BL31 asserts and releases that UART's reset, re-opens its gate and
   registers a 16550 console at `(0x9400 + n) << 10`. For UART0 this is
   `0x02500000`. For any other UART the computed base does not match the
   manual's 4 KiB spacing (inferred). On the A733 each UART has its own BGR
   register, so only UART0 is handled correctly (inferred).
2. **Next images** (`@0x3f78`, upstream with vendor sources). BL31 reads the
   OP-TEE and U-Boot entry addresses from the boot0 header. From the first
   instruction at each address it detects AArch64 or AArch32 and fills the
   entry-point records.
3. **EL3 translation** (`@0x41c8` calling `@0xb20c`): the mappings above.
4. **Chip version** (`@0xc390`, vendor): sets bit 15 of SYSCTRL `0x03000024`,
   reads bits 31:16, then clears bit 15.
5. **Chip check** (`@0x4418`, vendor): compares SID-derived identity words
   with fields of the image. A failure stops platform setup with a panic.
6. **Device tree search** (`@0x4228`, upstream `sunxi_find_dtb` with a vendor
   base): scans `0x04000000`–`0x040007f8` for the U-Boot base `0x4a000000`. On
   the A733 that window is device space below the NAND controller, so the
   search is not expected to find the U-Boot device tree (inferred).
7. **GIC-600** (`@0x65ec`, `@0x602c`, `@0x683c`, `@0x5f38`, upstream): driver,
   distributor, the boot core's redistributor and its CPU interface. The
   result is in [GIC-600 setup](#gic-600-setup-leaves-every-interrupt-nonsecure).
8. **Security setup and SMC0 report** (`@0xc3b8`, `@0xd53c`, vendor
   values): the register writes in the table below.

PSCI setup then clears the CPUIDLE policy words and programs core 0's
entry words (`@0x88cc`, `@0x4e88`). After the runtime services are
initialised, `bl31_main` (`@0x40f0`) can start one extra core for an
asymmetric-multiprocessing image, then prepares the U-Boot entry.

### Cold-boot register writes

Order is program order within BL31. The basis is observed unless stated.

| Order | Address | Register | Value or operation | Purpose | Offset |
| --- | --- | --- | --- | --- | --- |
| 1 | `0x02002e00` | CCU `UART0_BGR_REG` | Clear bit 16, set bit 16, clear bit 0, set bit 0 | Reset and re-gate the console UART | `0xc4c4` |
| 2 | `0x02500000` | UART0 | 16550 initialisation for 115200 baud | Console | `0x2938` |
| 3 | `0x03000024` | SYSCTRL version | Set bit 15, read, clear bit 15 | Read the chip version | `0xc390` |
| 4 | `0x03006364`, `0x03006200` | SID | Read | Chip check | `0x59f0`, `0x5974` |
| 5 | `0x04000000`–`0x040007f8` | Unlisted window | Read in 8-byte steps | Device tree search | `0x4280` |
| 6 | `0x03400000` | GICD_CTLR | Groups off, ARE_S and ARE_NS on; RWP polled without timeout | Distributor | `0x602c` |
| 7 | `0x03400080`+ | GICD SPI registers | Group 1 Nonsecure, priority `0x80`, level | SPI defaults | `0x602c` |
| 8 | `0x03460000`+ | GICR_PWRR, GICR_WAKER, SGI frame | Power on, wake, SGI and PPI defaults | Boot-core redistributor | `0x683c` |
| 9 | `0x02054004`–`0x020540d4` | SPC ports 0–13 set registers | `0xffffffff` each, stride `0x10` | Peripherals Nonsecure | `0xc3cc` |
| 10 | `0x02054104`–`0x02054174` | SPC ports 16–23 set registers | `0xffffffff` each | Peripherals Nonsecure | `0xc3e0` |
| 11 | `0x07003000`, `+0x4`, `+0x8` | S_TZMA | 0 | SRAM areas Nonsecure | `0xc408` |
| 12 | `0x07002004`, `+0x14`, `+0x24` | S_SPC | `0xffffffff` | Always-on peripherals Nonsecure | `0xc41c` |
| 13 | `0x07010290` | `PRCM_SEC_SWITCH_REG` | `\|= 7` | CPUS bus clock, PLL and power registers Nonsecure | `0xc438` |
| 14 | `0x02003f00` | `CCMU_SEC_SWITCH_REG` | `7` | PLL, bus and MBUS clock registers Nonsecure | `0xc448` |
| 15 | `0x04601020` | DMAC0 `DMA_SEC_REG` | `0xffff` | All DMA0 channels Nonsecure | `0xc458` |
| 16 | `0x04024020` | DMAC1 `DMA_SEC_REG` | `0xffff` | All DMA1 channels Nonsecure | `0xc460` |
| 17 | `0x020540e0` | SPC port 14 set register | `0xfffffbff` | Port 14 Nonsecure except bit 10; port 15 is never written | `0xc470` |
| 18 | `0x02002800` | CCU `TIMER0_CLK0_CLK_REG` | `0x80000002` | Gate on, 24 MHz source, divide by 4 | `0xc484` |
| 19 | `0x02002850` | CCU `TIMER0_BGR_REG` | `0x10001` | TIMER0 out of reset and gated | `0xc48c` |
| 20 | `0x03009008` | TIMER0 `+0x08` | 0 | Not described in the manual | `0xc498` |
| 21 | `0x0a000110`–`0x0a0001f8` | SMC0 secure regions | Read | Print the boot0 regions | `0xd53c` |
| 22 | `0x030060a0`, `0x0a000060`, `0x0a000064` | SID, SMC0 DRM region | Read when SID bit 0 is set | Print the DRM region | `0xd5f4`, `0xf54` |
| 23 | `0x02002054`, `0x0200205c` | CCU | `0x3180`, `1` | Upstream A64 and H5 bus-clock writes, made only if the version reads `0x1689` or `0x1718` | `0x4344` |
| 24 | `0x07051000`, `0x07052000`–`0x07059000` | CPUIDLE words | Clear bits 3:0 of each of nine words | Policy field, meaning inferred | `0x4e88` |
| 25 | `0x08001000` | CPU_SUBSYS_CTRL core 0 | `\|= 1` | Core 0 resets in AArch64 | `0xc298` |
| 26 | `0x08001004`, `0x08001008` | CPU_SUBSYS_CTRL core 0 | `0x48000228`, 0 | Core 0 warm-reset address | `0xc4ac` |

Register names come from the A733 user manual V1.00 (manual sections
4.1.6.278 for `CCMU_SEC_SWITCH_REG` and 4.2.5.26 for `PRCM_SEC_SWITCH_REG`).
The manual describes the SPC and TZMA blocks only as "always secure" and
gives no register layout; their port meaning comes from this image and the
A733 port's `sunxi_spc.h`.

### Boot0 header fields select the next images

| Header offset | Meaning | Reader | Basis |
| --- | --- | --- | --- |
| `0x10` | U-Boot entry address | `0x3f78` | Observed |
| `0x14` | Nonzero: pass the address at `0xb0` to U-Boot in x0 | `0x3f78` | Observed |
| `0x18` | OP-TEE entry address; 0 when no OP-TEE is packaged | `0x3f78`, `0xb20c` | Observed |
| `0x24` | Name field; bytes 1 to 7 feed the chip check | `0x4418` | Observed |
| `0xb0` | Device tree address for U-Boot | `0x3f78` | Observed |
| `0xb8` | Entry address of the extra core | `0x40f0` | Observed |
| `0xc0` | Index of the extra core | `0x40f0` | Observed |
| `0xc4` | Extra-core flags: any bit of `0x616d70` starts it; bit 31 also sets SCR_EL3.RW | `0x40f0` | Observed |

## PSCI implements version 1.1 with a CPUIDLE power sequence

The PSCI library is unmodified v2.5 (upstream). `psci_smc_handler`
(`8af16b86@0xa0b0`) reports version `0x10001` and supports CPU_SUSPEND,
CPU_OFF, CPU_ON, AFFINITY_INFO, SYSTEM_OFF, SYSTEM_RESET, PSCI_FEATURES and
SYSTEM_SUSPEND (observed). NODE_HW_STATE, MEM_PROTECT,
MEM_PROTECT_CHECK_RANGE and SYSTEM_RESET2 have dispatch code, but their
platform hooks are absent, so they return NOT_SUPPORTED (observed). The
platform supplies no `validate_ns_entrypoint`, so BL31 accepts any
Nonsecure entry address (observed, ops table `@0xf040`).

The power-domain tree has one system, one cluster and eight cores
(`@0x8308`, observed). `validate_power_state` (`@0x8920`) accepts only the
original format with StateID 0: `0x0` (core standby), `0x10000` (core
power-down), `0x1000000` (cluster retention), `0x1010000` (cluster
power-down) and `0x2010000` (system power-down). The board device tree's
two idle states, `0x0010000` and `0x1010000`, are in that set (inferred from
[the CPU backsolve](../reference-backsolve-cpu.md)).

### CPU_ON

The caller runs `plat_pwr_domain_on` (`@0x854c`); the target runs
`plat_pwr_domain_on_finish` (`@0x85c8`). Polls have no timeout. Core `n` is
MPIDR Aff1.

| Order | Side | Address or register | Operation | Purpose | Offset | Basis |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Caller | `0x08001000 + 0x1000n` | `\|= 1` | AArch64 at reset | `0xc2ac` | Observed |
| 2 | Caller | `0x08001004 + 0x1000n` | `0x48000228` | Warm entry, low word | `0xc4ac` | Observed |
| 3 | Caller | `0x08001008 + 0x1000n` | 0 | Warm entry, high word | `0xc4bc` | Observed |
| 4 | Caller | `0x07052008 + 0x1000n` | Poll bits 3:0 for 0 | Wait for OFF | `0xc320` | Observed |
| 5 | Caller | `0x07050220 + 4n` | `\|= 1` | Power-mode bit | `0xc350` | Observed |
| 6 | Caller | `0x07050200 + 4n` | `\|= 1` | Power-on request | `0xc35c` | Observed |
| 7 | Caller | `0x07052008 + 0x1000n` | Poll bits 3:0 for 8 | Wait for ON | `0xc360` | Observed |
| 8 | Caller | `0x07050200 + 4n`, then `0x07050220 + 4n` | Clear bit 0 of each | Release the request | `0xc378` | Observed |
| 9 | Target | CPU reset function | A76 only: erratum 1946160 | Errata | `0x2db4` | Upstream |
| 10 | Target | `0x07050200 + 4n` | Clear bit 1 | Allow GIC wake-up | `0xc23c` | Observed |
| 11 | Target | `CLUSTERPWRDN_EL1` (`S3_0_C15_C3_6`) | 1 | Meaning not established | `0x8240` | Observed |
| 12 | Target | GICR_PWRR, GICR_WAKER, ICC_SRE_EL3 | Power on, wake, enable the CPU interface | GIC | `0x6a1c`, `0x5f38` | Upstream |
| 13 | Target | GICR SGI frame | All Group 1 Nonsecure, priority `0x80`, then the shared snapshot | SGI and PPI state | `0x3c54`, `0x3cb8` | Observed |
| 14 | Target | CNTFRQ_EL0 | 24,000,000 | Timer frequency | `0x8bd4` | Observed |

Steps 1 to 8 are the sequence that SunstoneOS's
[`cpu_start.c`](../../platform/a733/cpu_start.c) reproduces with bounded
polls. Its first power-on of every secondary passes on the board
(board: [CPU start evidence](../cpu-start.md)). The SunstoneOS instruction
harness also executes the image's helpers at `0xc4a0`, `0xc298` and `0xc314`
and compares their writes with the native code (observed in emulation,
`checks/a733-cpu-start.py`).

### CPU_OFF

`psci_do_cpu_off` (`@0x9370`, upstream) calls `plat_pwr_domain_off`
(`@0x84bc`, vendor):

1. CPUPWRCTLR_EL1 bit 0 set, then ISB, with no DSB (`@0x2aa0`, `@0x2bd8`).
2. CLUSTERPWRDN_EL1 = 0 (`@0x81e0`).
3. The core's SGI and PPI configuration is saved into one snapshot that all
   cores share (`@0x3db4`): IGROUPR0, ISENABLER0, ICFGR0, ICFGR1, IGRPMODR0.
4. The CPU interface is disabled and ProcessorSleep is set; ChildrenAsleep is
   polled without timeout (`@0x5e68`, upstream `gicv3_cpuif_disable`).
5. GICR_PWRR.RDPD = 1 (`@0x69d8`, upstream).
6. CLUSTERPWRDN_EL1 = 0 again (`@0x8500`).
7. `0x07050220 + 4n` bit 0 cleared (`@0xc308`), then `0x07050200 + 4n` bit 1
   set to block GIC wake-up (`@0xb2c0`).
8. DSB SY and WFI (`@0x3790`); a returning WFI panics.

No software cache maintenance runs (observed). Upstream TF-A relies on
hardware cache cleaning for these cores.

### CPU_SUSPEND and SYSTEM_SUSPEND

- **Core standby `0x0`** (`@0x8284`): sets SCR_EL3.IRQ and FIQ, then DSB SY
  and WFI, then restores SCR_EL3. No MMIO (observed).
- **Core or cluster power-down** (`@0x864c`, `@0x81a4`): the CPU_OFF GIC
  steps only. BL31 writes no CPUIDLE word and no cluster register, so the
  core does not power off through the power controller (inferred). On the
  Cortex-A76 the activity counters are saved and restored around the
  suspend (`@0x1898`, `@0x1830`, observed).
- **Cluster retention `0x1000000`**: runs the full GIC power-down as well,
  although the core only retains state (observed; inferred to be
  unintended).
- **System suspend** (`@0x864c`, system branch, observed):
  1. GIC power-down and wake-disable, as for CPU_OFF.
  2. Distributor state and the pending SGIs of all eight redistributors are
     saved.
  3. When SID `0x030060a0` bit 0 is set, the SMC0 DRM words `0x0a000060` and
     `0x0a000064` are saved.
  4. GPIO `PB_CFG0` (`0x02000100`) and `PF_CFG0` (`0x02000300`) are saved.
  5. SCP message type `0x22` carries the MPIDR, the warm entry and states 3,
     3, 3 (`@0x1948`). The SCP then switches the CPU clocks and rails off
     ([CPU backsolve](../reference-backsolve-cpu.md)).
- **System resume** (`@0x8790`, system branch, observed):
  1. The GIC is re-initialised and its saved state restored.
  2. SCP message `0x11` is sent; its reply is stored as the wakeup source at
     `0x48020798`.
  3. The two GPIO words are restored and the console UART is reset again
     (`@0xc4c4`).
  4. SYSCTRL `0x03000008` is written 0.
  5. The DRM words are written to SMC0 and to SMC1 (`0x0a010060`,
     `0x0a010064`).
  6. The security setup runs again (`@0xc3b8`), and wake-disable is cleared.

### SYSTEM_OFF and SYSTEM_RESET

Both send SCP message type `0x24`, with argument 0 for off and 1 for reset,
then WFI; a returning WFI panics (`@0x80bc`, `@0x8110`, `@0x2158`,
observed). BL31 writes no watchdog or reset register itself. The "SCP error"
message is unreachable because the send function always returns 0
(observed). Off and reset therefore require a running SCP (inferred).

## GIC-600 setup leaves every interrupt Nonsecure

The driver data (`8af16b86@0xf010`) gives the distributor at `0x03400000`,
redistributors from `0x03460000` and eight frames, with no interrupt
properties (observed). After setup GICD_CTLR is `0x30`: ARE_S and ARE_NS,
with no group enabled. Every SPI is Group 1 Nonsecure, priority `0x80`,
level-triggered. The CPU interface enables Group 0 and Secure Group 1;
Nonsecure Group 1 is left to the operating system. No Secure interrupt is
configured (observed, upstream `gicv3_distif_init`, `gicv3_cpuif_enable`).

- The cold-boot SGI and PPI defaults reproduce the upstream v2.5 indexing
  defect: only INTIDs 0 to 7 receive the default priority (`@0x6744`,
  observed). The vendor warm path (`@0x3c54`) writes all eight priority words.
- The shared snapshot (`0x48040c20`) gives a powering-up core the
  configuration that the last core to power down saved, including its enable
  bits (observed).
- Every RWP, WAKER and PWRR poll is unbounded (observed).
- No GIC-600 erratum workaround is present. GICR_CTLR.DPG is never written,
  so erratum 2384374 is not worked around (observed); TF-A v2.5 predates that
  workaround.

The distributor reports `GICD_IIDR = 0x0201643b`, GIC-600 r1p6, and each
redistributor frame `n` reports `GICR_PWRR = n << 8` (board:
[ROM interrupt-frame reads](../a733-smp-supervised.md#rom-reads-identify-all-interrupt-frames-without-changing-their-state)).

## CPU errata and implementation-defined registers

| Core | Revision | Compiled in | Applies | Absent compared with `0892ad4d` | Basis |
| --- | --- | --- | --- | --- | --- |
| Cortex-A55 | r2p0 | 1530923 | 1530923 | DSU 798953 and 936184 are reported but not compiled | Observed (`@0x2ac0`) |
| Cortex-A76 | r4p1 | 1946160 (reset slots 3 to 5, `@0x2b8`); CVE-2018-3639 by SSBS | Both | 2743102 (no DSB before ISB at power-down), 2356586, 3888013, CVE-2022-23960, CVE-2025-10263 | Observed (`@0x2c0c`, `@0x2db4`) |

The A733 port `5bc9605c` selects no errata and disables the CVE workarounds.

| Register | Encoding | Use | Basis |
| --- | --- | --- | --- |
| CPUPWRCTLR_EL1 | `S3_0_C15_C2_7` | Bit 0 set before power-down | Upstream |
| CLUSTERPWRDN_EL1 | `S3_0_C15_C3_6` | 1 at power-up, 0 at power-down | Observed; meaning needs the DSU manual |
| CPUACTLR2_EL1 | `S3_0_C15_C1_1` | Bit 16 cleared by the A76 CVE-2018-3639 hook `@0x2bf8` | Upstream |
| IMP-DEF patch slots | `S3_6_C15_C8_0`–`3` | Erratum 1946160 | Upstream |
| A76 activity monitors | `S3_3_C15_C9_0`–`7` | Saved and restored around core power-down; SMC `0xc000ff20` enables them | Observed |
| ACTLR_EL3 | — | SMC `0x8000ff50` writes `0xc00` | Observed |

## Four runtime-service descriptors dispatch every SMC

The runtime-service descriptors (`8af16b86@0x127a0`) register three services
(observed):

| Owning entity | Call type | Name | Handler |
| --- | --- | --- | --- |
| 0 (Arm architecture) | Fast | `arm_arch_svc` | `0x21e4` |
| 4 (Standard: PSCI) | Fast | `std_svc` | `0xacf0` |
| 50–63 (Trusted OS) | Fast and yielding | `opteed_fast`, `opteed_std` | `0x7c14` |

There is no SiP (owning entity 2) service. Allwinner's calls use owning
entity 0 and are decoded inside the architecture handler (observed). An
unknown ID returns -1. The OP-TEE dispatcher is built in, but this package
contains no OP-TEE image, so Trusted OS calls have no target (inferred from
header word `0x18`). The full list, with arguments, is in
[`data/tf-a-monitor-smc.json`](data/tf-a-monitor-smc.json).

| ID (32-bit / 64-bit) | Handler | Function | Hardware |
| --- | --- | --- | --- |
| `0x80000000` | `0x21e4` | SMCCC_VERSION, returns 1.2 | — |
| `0x80000001` | `0x21e4` | SMCCC_ARCH_FEATURES | — |
| `0x80000002` | `0x21e4` | SMCCC_ARCH_SOC_ID | — |
| `0x80008000`, `0x80007fff` | `0x21e4` | Workarounds 1 and 2; return with no action | — |
| `0x80000023` / `0xc0000023` | `0x1f7c` | Returns the wakeup source recorded at resume | — |
| `0x80000025`, `0x26`, `0x60`, `0x61`, `0x62`, `0x64`, `0x96` (and `0xc0…` forms) | `0x1904`, `0x2114`, `0x1ff8`, `0x1f34`, `0x20d0`, `0x203c`, `0x1fb4` | Forward the arguments to the SCP as the message type of the same number | Message boxes |
| `0x80000095` / `0xc0000095` | `0x1f90` | Runs the USB "efex" device loop at EL3 | USB0, CCU USB gates, GIC INTID 188 |
| `0x8000fe00` / `0xc000fe00` | `0xbd58` | Reads a named eFuse key | SID |
| `0x8000fe01` / `0xc000fe01` | `0xbf9c` | Programs a named eFuse key | SID, RTC `0x07090204` |
| `0x8000ff04`, `0x8000ff40` | `0x89c4` | Enter a new Nonsecure image | — |
| `0x8000ff10` | `0xae88` | Load and start the SCP | S_PRCM, E902 configuration, CPUIDLE, RTC |
| `0x8000ff15` | `0x219c` | SCP message `0x24` with argument 3 | Message boxes |
| `0x8000ff41` | `0x8c1c` | Start a core at a given address | CPU_ON registers |
| `0x8000ff42` | `0xc274` | Report whether a core's CPUIDLE status is ON | `0x07052008 + 0x1000n` |
| `0x8000ff50` | `0x21e4` | ACTLR_EL3 = `0xc00` | — |
| `0xc000ff05` | `0xb15c` | Read one register from a fixed list | Listed registers |
| `0xc000ff06` | `0xb1d8` | Write the DMA security words only | DMAC security registers |
| `0xc000ff07` | `0xbeec` | eFuse key information | SID `0x03006200` |
| `0xc000ff20` | `0xadd8` | Enable the A76 activity monitors; clear the AMU traps in CPTR_EL2 and CPTR_EL3 | — |
| `0x80000027`, `0xc0000029`, `0x8000ff14`, `0xc00000a0` | stubs | Return -1 | — |
| `0x84000000`–`0x8400000e`, `0xc4000001`–`0xc400000e` | `0xa0b0` | PSCI 1.1 | See PSCI |

No function in the image accesses the GPU window (`0x01800000`–`0x0188ffff`),
the NPU, the PCK-600 at `0x07060000` or SMC0 `0x0a0000b0` (observed: none of
these addresses appears in the access map).

## The SCP is started on request and reached through two message boxes

BL31 never starts the SCP during its own boot. The board U-Boot issues SMC
`0x8000ff10` with the kernel device tree address just before it starts Linux,
when built with `CONFIG_ARISC_DEASSERT_BEFORE_KERNEL` (Orange Pi U-Boot
`b791be84`, `arch/arm/lib/bootm.c` lines 408–412). Then `sunxi_arisc_load`
(`8af16b86@0xae88`) runs these steps (observed):

| Order | Address | Operation | Purpose |
| --- | --- | --- | --- |
| 1 | `0x0709010c` | Write the SMC argument | RTC general-purpose word read by the SCP (inferred) |
| 2 | `0x00044000` | Copy `0x28000` bytes from `0x48100000`, then clean the cache | SCP image into SRAM A2; the image at `0x48100000` is placed by the boot chain (inferred) |
| 3 | `0x48100000` | Copy `0x8000` bytes from `0x4812c000`, then clean the cache | SCP parameters in DRAM (inferred) |
| 4 | `0x0701021c` | `0x10003` | `RISCV_BGR_REG`: E902 gates and reset release |
| 5 | `0x07032204` | `0x40004000` | E902 start address |
| 6 | `0x07050008` | `\|= 1` | CPUIDLE word for the E902 |
| 7 | `0x07010210` | `\|= 0x80000000` | `RISCV_24M_CLK_REG` gate |
| 8 | `0x50005120` | Clear bit 0 | DRAM word, purpose unknown |
| 9 | Message boxes | Wait for message `0x90`, print the SCP version, reply | Startup handshake |

Register names for steps 4 and 7 are from manual sections 4.2.5.23 and
4.2.5.24, as cited in [the remote-writer exclusion](../remote-writers.md).

BL31 sends through S_MBOX: it checks `0x0709406c` and writes `0x0709407c`.
It receives through the CPUX message box: it polls `0x0300406c`, reads
`0x0300407c` and acknowledges with `0x03004024 = 0x40` (`@0x1bf4`, `@0x1b18`,
`@0x19f0`, observed). Each request carries an attribute word, a type,
arguments and a timeout of 4000. The types BL31 uses are `0x11` (resume),
`0x22` (system suspend), `0x24` (off or reset) and the SMC-forwarded types in
the table above.

## eFuse, SID and the USB loop run only on request

- **eFuse keys.** A table of key names at `8af16b86@0x130b8` includes
  `chipid`, `customer_reserved`, `npu_status` and `hdcp_edp_l128`. Reads go
  through the SID read control at `0x03006040`/`0x03006060` (`@0xac5c`); the
  programming path uses `0x03006040`/`0x03006050` and RTC `0x07090204`
  (`@0xabe0`). Only the SMCs in the table above reach them (observed).
- **USB loop.** SMC `0x80000095` registers a USB device and runs Allwinner's
  efex protocol in EL3, polling GIC INTID 188 rather than taking interrupts.
  It opens the USB0 clock and reset through CCU `0x020025a4`, `0x020025c0`,
  `0x02003300` and `0x02003304` and configures the PHY word `0x04100410`
  (`@0x1f90`, `@0xd274`, `@0xc8cc`, observed). No boot or PSCI path calls it.

## Hand-off to U-Boot and Linux

`bl31_prepare_next_image_entry` (`8af16b86@0x4380`, upstream) and
`cm_prepare_el3_exit` (`@0x47c4`, upstream) leave this state for U-Boot:

| Item | Value | Basis |
| --- | --- | --- |
| Entry | Header word `0x10` (`0x4a000000` in the board package) | Observed; address inferred from the device-tree search constant |
| Exception level | EL1h with DAIF masked (SPSR `0x3c5`); AArch32 SVC (`0x1d3`) for a 32-bit image | Observed (`@0x4040`, `@0x40d8`) |
| x0 | Header word `0xb0` when header word `0x14` is nonzero, else 0 | Observed |
| SCR_EL3 | `0x30631`: NS, SIF, RW, APK, API and the RES1 bits; FIQ only with a Secure OS; HCE only for EL2 entry | Observed ([CPU backsolve](../reference-backsolve-cpu.md)) |
| EL2 | HCR_EL2 `0x30080000000`, CPTR_EL2 `0x33ff`, CNTHCTL_EL2 3, CNTVOFF_EL2 0, VPIDR/VMPIDR from MIDR/MPIDR, VTTBR_EL2 0, MDCR_EL2 with HPMN = PMCR.N, HSTR_EL2 0, CNTHP_CTL_EL2 0 | Upstream `cm_prepare_el3_exit` (`@0x4854`–`0x4898`) |
| EL1 | SCTLR_EL1 `0x30d00800`, CPACR_EL1 0 | Observed (`@0x31b0`) |
| Timer | CNTFRQ_EL0 = 24 MHz on every core | Observed (`@0x8bd4`) |
| GIC | Distributor ARE on, no group enabled; every SPI Group 1 Nonsecure | Observed |
| Security | The cold-boot writes 9 to 20 above | Observed |
| Console | UART0 stays registered for boot output; runtime messages keep printing | Observed (`@0x4220` sets the boot state) |

Linux therefore starts at EL1, with EL2 unused, and uses PSCI through `std_svc`
for every core. U-Boot is responsible for starting the SCP and for routing
the shared SRAM; BL31 does neither at cold boot.

## Hardware access map

[`data/tf-a-monitor-mmio.json`](data/tf-a-monitor-mmio.json) lists every
access with its value, function and site. Base addresses held in data (the
GIC, USB0 and UART0 bases) are listed separately in that file.

| Peripheral | Base | Accesses | SunstoneOS status |
| --- | --- | --- | --- |
| CCU | `0x02002000` | UART0 BGR, TIMER0 clock and BGR, security switch, USB0 clocks, A64/H5 bus clocks | Security switch, TIMER0 clock and the USB gates through this path not written |
| GPIO | `0x02000000` | `PB_CFG0`, `PF_CFG0` saved and restored at system suspend | Not applicable |
| SPC | `0x02054000` | Ports 0–14 and 16–23 | Not written; no layout documented |
| SYSCTRL | `0x03000000` | Version `+0x24`; shared SRAM routing `+0x08` = 0 at resume | `+0x08` read only ([handoff](../handoff.md)) |
| CPUX message box | `0x03004000` | SCP receive | Not used |
| SID | `0x03006000` | Chip check, DRM flag `+0xa0`, eFuse | Not documented |
| TIMER0 | `0x03009000` | `+0x08` = 0 | Not used; register undocumented |
| GIC-600 | `0x03400000` | Full driver | Implemented ([multicore audit](../a733-multicore-reference-audit.md)) |
| DMAC0, DMAC1 | `0x04601000`, `0x04024000` | Security register `+0x20` = `0xffff` | Not written ([DMA](../dma.md#dma1-request-routing-remains-unresolved)) |
| USB0 | `0x04100000` | PHY word `+0x410`, controller through pointers | USB loop only |
| S_SPC, S_TZMA | `0x07002000`, `0x07003000` | Security setup | Not written |
| S_PRCM | `0x07010000` | Security switch `+0x290`, E902 clock and reset | Read by the remote-writer check only |
| CPUS E902 configuration | `0x07032000` | Start address `+0x204` | Read by the remote-writer check only |
| CPUIDLE | `0x07050000` | Core power requests, status, policy words, E902 word | Power requests and status written by `cpu_start.c`; policy words not written |
| RTC | `0x07090000` | `+0x10c` SCP argument, `+0x204` eFuse | Not written |
| S_MBOX | `0x07094000` | SCP send | Not used |
| CPU_SUBSYS_CTRL | `0x08000000` | Per-core AArch64 bit and reset address | Written by `cpu_start.c` |
| SMC0, SMC1 | `0x0a000000`, `0x0a010000` | Secure-region read; DRM region read and resume restore | Not documented |
| Unlisted window | `0x04000000` | Device-tree search reads | Not used |

## Comparison with upstream TF-A and the A733 port

- **Unchanged from upstream v2.5:** entry, context management, the PSCI
  library, the GICv3 driver, translation tables, runtime-service dispatch,
  the OP-TEE dispatcher, the console and crash code (upstream, every
  assertion line matches).
- **Changed upstream platform code:**
  - `bl31_platform_setup` adds the chip check and the SMC0 report;
  - the device-tree search base is device space;
  - the soc-ID bus-clock writes of the A64 and H5 remain;
  - `bl31_plat_runtime_setup` keeps the console in its boot state.
- **Vendor platform code with no upstream counterpart:**
  - power ops for CPU_SUSPEND and SYSTEM_SUSPEND;
  - the shared SGI/PPI snapshot;
  - CLUSTERPWRDN writes;
  - the SCP loader and message box;
  - the vendor SMC IDs, eFuse keys, the USB loop, the extra-core start and the
    activity-monitor helpers.
- **Compared with the A733 port `5bc9605c`:**
  - Supported calls: the port implements only CPU_ON, CPU_OFF and system off,
    and its idle-state table is empty.
  - Entry words: the port programs every core's entry words once; the vendor
    writes them on each CPU_ON.
  - Security setup: both make the same S_TZMA and S_SPC writes and the same
    CCU switch write. The port writes all 24 SPC ports with
    `0xffffffff`, `PRCM_SEC_SWITCH_REG = 1` and one DMA security word. The
    vendor leaves SPC port 14 bit 10 secure, never writes port 15, sets
    PRCM bits 0 to 2 and writes both DMA controllers. The TIMER0 writes are
    vendor only.
- **Compared with upstream `0892ad4d`:** the newer release adds the GIC-600
  2384374 workaround and A76 errata 2743102, 2356586 and 3888013. It also
  adds the CVE-2022-23960 and CVE-2025-10263 mitigations. The vendor image
  has none of these.

## What the SunstoneOS EL3 monitor must reproduce

SunstoneOS's EL3 code is [`monitor.c`](../../platform/a733/monitor.c) and
[`monitor.S`](../../platform/a733/monitor.S) for the boot core, and
[`continue.S`](../../platform/a733-smp-cached/continue.S) with
[`cpu_continue.c`](../../platform/a733/cpu_continue.c) for secondaries.
Secondary power-on is [`cpu_start.c`](../../platform/a733/cpu_start.c).

| Vendor behaviour | SunstoneOS today | Required |
| --- | --- | --- |
| CPU_ON register sequence (steps 1 to 8) | Reproduced with deadlines | Done (board) |
| Wake-disable bit cleared on the target, set at power-down | Reproduced | Done (board) |
| Security setup writes 9 to 17 | Absent | Before any Nonsecure driver that needs a secure-default clock, bus or DMA register; the manual makes `PLL_GPU0_CTRL_REG` secure by default (section 4.1.6.16) |
| CNTFRQ, CNTHCTL, CNTVOFF, CPTR at EL3 and EL2 | Written | Done |
| CNTHP_CTL_EL2 = 0, VPIDR_EL2 and VMPIDR_EL2 from MIDR and MPIDR | Absent | Add beside CNTHCTL_EL2 ([CPU backsolve](../reference-backsolve-cpu.md)) |
| GIC distributor RWP wait instead of refusal | Refusal in several places | Poll until RWP clears, as upstream `gicd_wait_for_pending_write` does |
| CPUIDLE policy clear at `0x07051000`–`0x07059000` | Not written | Record the inherited values first; write only on a difference |
| CLUSTERPWRDN_EL1 | Not written | Only after its meaning is established |
| SYSCTRL `0x03000008` routing | FEL boot leaves 2 | The NPU path requires the U-Boot value; BL31 itself writes 0 only at resume |
| SCP start (SMC `0x8000ff10` sequence) | Not started | Only for system suspend, off and reset through the SCP |
| Cortex-A76 erratum 1946160; A55 1530923 | Applied with more errata | Done |
| GIC-600 2384374, A76 2743102 | 2743102 applied, 2384374 absent | Add 2384374 before any image routes SPIs |

## New insights for SunstoneOS

1. **The nine policy words are in the CPUIDLE block.** `0x07051000` and
   `0x07052000`–`0x07059000` lie inside CPUIDLE (`0x07050000`–`0x0705ffff`) in
   the manual's memory map, while the PCK-600 is at `0x07060000`
   (observed, `8af16b86@0x4e88`). [The CPU power-state design](../designs/cpu-power-states.md)
   and [the CPU backsolve](../reference-backsolve-cpu.md) treat them as PPU
   policy registers; that layout is an inference.
2. **DMA security registers are set to Nonsecure for both controllers.**
   BL31 writes `0xffff` to `0x04601020` and `0x04024020` (`@0xc458`,
   `@0xc460`). A FEL-booted SunstoneOS image inherits their reset value. This
   affects the DMA0 consumers in
   [the DMA pool session](../dma-pool-session.md) and the DMA1 analysis in
   [the DMA document](../dma.md#dma1-request-routing-remains-unresolved).
3. **The CCU and PRCM security switches gate Nonsecure clock writes.**
   `CCMU_SEC_SWITCH_REG = 7` makes PLL, bus and MBUS registers Nonsecure;
   `PRCM_SEC_SWITCH_REG |= 7` does the same for CPUS bus clocks, PRCM PLLs and
   power registers (`@0xc448`, `@0xc438`). SunstoneOS writes neither, which
   bears on [the DRAM bus priority design](../designs/dram-bus-priority.md)
   and on every Nonsecure clock driver.
4. **SPC port 14 bit 10 stays Secure and port 15 is never written**
   (`@0xc470`). A peripheral behind either stays Secure under the vendor
   stack as well.
5. **TIMER0 is clocked and its `+0x08` word cleared at cold boot**
   (`@0xc484`–`0xc498`). No SunstoneOS code uses TIMER0.
6. **BL31 writes SYSCTRL `0x03000008` = 0 on resume from system suspend**
   (`@0x8834`). This matches the NPU finding in [the handoff](../handoff.md)
   that U-Boot, not BL31, routes the shared SRAM at cold boot.
7. **The vendor stack enters U-Boot and Linux at EL1.** EL2 is unused, and
   SunstoneOS's EL2 settings in `continue.S` follow the same unused-EL2 model.
8. **System off and reset depend on the SCP.** BL31 has no reset register
   path of its own. A SunstoneOS reset must use the watchdog path in
   [the watchdog document](../watchdog.md) or a started SCP.
9. **No GIC-600 or A76 power-down workaround is present in the vendor
   image.** The vendor image is therefore no evidence that 2384374 or 2743102
   are unnecessary on the A733.

## Open questions

- What bits 3:0 of the nine CPUIDLE words at `0x07051000`–`0x07059000`
  control, and which unit the ninth word (`0x07051000`) belongs to.
- What bit 0 of CLUSTERPWRDN_EL1 does on this DSU revision; the DSU
  technical reference manual is not cached.
- Which peripherals sit behind SPC port 14 bit 10 and port 15.
- What TIMER0 `+0x08` controls, and why BL31 clocks TIMER0.
- What SYSCTRL `0x03000024` returns while bit 15 is set; the board's
  captured value `0x101` was read with bit 15 clear
  ([board wiring](../board-wiring.md)).
- Whether the device-tree search reads at `0x04000000` decode to any slave on
  the A733.
- The purpose of the 16 KiB Secure device mapping at `0x48200000` and of
  the DRAM word `0x50005120` cleared during SCP start.
- The meaning of SCP message types `0x25`, `0x26`, `0x60`–`0x64` and `0x96`;
  the SCP source is not published.
- Whether the chip check at `0x4418` accepts both SunstoneOS boards. No board
  has run this image under SunstoneOS control.
