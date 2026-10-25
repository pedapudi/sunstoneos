# The A733 boot ROM and boot0 vendor binaries

The Allwinner A733 starts from an on-chip boot ROM (BROM) that SunstoneOS
cannot replace, and the board's microSD boot package begins with a vendor
first-stage loader, boot0, that SunstoneOS reuses for DRAM and clock bring-up
but does not otherwise run. This document records what those binaries do, so
that a future developer can rely on them, reimplement a part, or decide an
ownership question without disassembling them again. It describes present
behaviour; the acquisition history and running record stay in the evidence
directories cited here and in the [vendor-firmware provenance
README](README.md).

Every fact below is cited as a blob SHA-256 prefix and an offset, and labelled:
*observed in the binary* means read directly from the cited blob; *confirmed on
a board* cites the evidence directory where a physical read matched; *inferred*
means derived from the instruction sequence and the manual without a direct
constant or a board read. The full machine-readable inventories live outside
Git under `~/.cache/sunstoneos/evidence/vendor-firmware-20261025/boot/`; the
curated extracts are the JSON files under [`data/`](data/) and are linked from
each section. Register tables give the address, the value, the order, any
delay, and the purpose. No disassembly listing or blob byte sequence is
reproduced here.

This document does not duplicate the boot records it builds on. The cold-start
sequence and the physical bring-up probes are in [boot.md](../boot.md); the
SunstoneOS monitor payload and its handoff capture are in
[cold-entry.md](../cold-entry.md); the two-slot card layout is in
[boot-slots.md](../boot-slots.md); the host packaging of a card image is in
[microsd-boot-package.md](../microsd-boot-package.md); the forced-FEL reset
path and the captured board ROM are in [watchdog.md](../watchdog.md).

## Identity and provenance

| Binary | Role | SHA-256 | Size | Load address |
| --- | --- | --- | --- | --- |
| FIXS-BROM | Reset, media boot and FEL dispatch | `3a2a7d28…` | 69,632 B | `0x00000000` |
| NS-BROM | USB FEL device-side protocol | `95be69c9…` | 45,056 B | `0x00017000` |
| boot0 | First-stage loader | `829ea8e3…` | 245,760 B | `0x00047000` |
| boot0 (1800 MHz) | Same code, 1800 MHz DRAM set | `d2a9bccc…` | 245,760 B | `0x00047000` |
| Board BROM capture | FIXS-BROM read from the original board | `a8e31821…` | 69,632 B | `0x00000000` |

The two BROM images come from the published `smaeul/sunxi-blobs` dump at
revision `42aabf4f2c93158a0cd88a000f9af56ce024882a`, path `sun60iw2p1/`, decoded
from hexadecimal to binary. The manual's memory map places FIXS-BROM at
`0x00000000`–`0x00010FFF` (68 KiB), a high-security HS-BROM at
`0x00011000`–`0x00016FFF`, and NS-BROM at `0x00017000`–`0x00021FFF` (44 KiB)
(A733 User Manual V1.00, section 2). The published FIXS dump and the board
capture (`a8e31821…`) are the same 68 KiB region; thirteen words in their first
1 KiB differ, and the reset and redirect code through offset `0x228` is
identical, so the board runs a close revision of the published ROM
(*confirmed on a board*, `~/.cache/sunstoneos/evidence/rom-copy-20261016-084055/`).
The HS-BROM and the secure BROM Monitor are not in either dump.

boot0 comes from the Orange Pi build repository
`orangepi-xunlong/orangepi-build` at revision
`a95129bf30ea11886cc35f08ebb3e69695a26656`, path
`external/packages/pack-uboot/sun60iw2/bin/`.

No term found in the applicable sources forbids reverse engineering or
disassembly of these four binaries. The licenses and terms that apply, with
their sources, are:

- **The Orange Pi build repository (boot0 and both images).** The repository's
  only repository-level license is its root `LICENSE`, the GNU General Public
  License version 2 (cached copy
  `~/.cache/sunstoneos/evidence/microsd-boot-package-20261020/references/orangepi-build/LICENSE`).
  There is no `LICENSE` or terms file inside
  `external/packages/pack-uboot/sun60iw2/bin/` (observed in the repository tree
  listing `~/.cache/sunstoneos/sources/orangepi-build-tree.txt`). GPL-2.0
  permits studying and disassembling the covered work; it imposes no
  reverse-engineering prohibition.
- **Notices inside the binaries.** The BROM dumps and both boot0 images carry no
  embedded copyright, license, or reverse-engineering notice. NS-BROM contains
  the device-descriptor strings `Allwinner USB GADGET` and `Allwinner`, which are
  USB identifiers, not license terms (observed in the binaries `3a2a7d28…`,
  `95be69c9…`, `829ea8e3…`, `d2a9bccc…`; string scan finds no
  copyright/license/reverse-engineering text).
- **The BROM dump source.** The two BROM images are redistributed by the
  community `smaeul/sunxi-blobs` repository as extracted on-chip ROM for
  interoperability; the dump files themselves carry no license text. The code is
  Allwinner's on-chip boot ROM.
- **Allwinner's published terms.** Allwinner publishes no public license grant
  for these boot binaries, and none was found that forbids reverse engineering
  or disassembly. The A733 User Manual used for register names is marked
  Confidential, which governs that document, not these binaries, and is not a
  disassembly prohibition on the firmware.

This assessment covers only the BROM dumps and the two boot0 images. It does not
extend to the separate TOC packer (`dragonsecboot` and the
`allwinner-bsp-tools` source), whose files carry All-Rights-Reserved notices and
whose individual licensing is unresolved
([microsd-boot-package.md](../microsd-boot-package.md)), nor to the GPU firmware,
whose license forbids disassembly and whose analysis is withdrawn.

boot0's header is an eGON.BT0 block:
a branch at offset 0, the magic `eGON.BT0` at `0x4`, an additive 32-bit
checksum at `0xc` (stock value `0x2fce7bb3`), the length `0x3c000` at `0x10`,
the run address `0x47000` at `0x18`, and a 128-byte DRAM parameter block at
`0x38` that the loader overwrites at runtime (observed in the binary, boot0
`829ea8e3…` header `0x47000`–`0x47040`). The `1800M` variant shares every
instruction; only its DRAM parameter tables and checksum differ (observed in
the binary, diff of `829ea8e3…` against `d2a9bccc…`: all 113 changed byte runs
lie in the DRAM tables `0x47374`+ and the training code, none in the clock,
PMIC or package-parser code). The source for boot0 is not published; only these
binaries exist. The full source references and fetch commands are in the
[provenance README](README.md).

## The boot ROM (BROM)

The BROM runs first on Cortex-A55 core 0 at reset. It splits into two tasks:
media boot, which loads and verifies boot0 from non-volatile storage, and FEL,
the USB recovery loader through which SunstoneOS uploads every image today
(A733 User Manual V1.00, section 3.1). Curated function and register inventories
are in [`brom-functions.json`](data/brom-functions.json),
[`brom-ns-functions.json`](data/brom-ns-functions.json),
[`brom-mmio.json`](data/brom-mmio.json) and [`brom-tables.json`](data/brom-tables.json).

### Reset path and core selection

At the reset vector (offset `0x44`), core 0 continues into common
initialisation while secondary cores read their MPIDR affinity and jump through
an eight-entry vector table based at `0x070501c4` in the CPUIDLE block (observed
in the binary, `a8e31821…` `0x44`–`0x178`). Before that dispatch, two
software-redirect checks run on core 0: if `0x070501c0` holds `0xfa50392f`, the
ROM loads the program counter from `0x070501c4`; and if the low 16 bits of
`0x070501d4` equal `0xefe8`, it loads the counter from `0x070501d8` (observed in
the binary, `a8e31821…` `0x164`, `0x1f0`; literals at `0x2c0`, `0x2d4`). These
are the recovery routes that [watchdog.md](../watchdog.md) evaluates; a board
read found all four words zero (*confirmed on a board*,
`rom-copy-20261016-084055/recovery-registers.json`).

### Boot-media order and selection

The ROM reads a BOOT_MODE bit to choose between a GPADC-pin selection and an
eFuse selection of the boot medium (A733 User Manual V1.00, section 3.1.3). The
media walker tries controllers in a fixed order and, on the first that returns a
valid boot0 header, copies boot0 to `0x47000` (observed in the binary,
`a8e31821…` `0xa00`–`0xad8`). The try order and the GPADC fallback order are in
[`brom-tables.json`](data/brom-tables.json). The board under test boots from the
microSD controller SMHC0; neither board has eMMC or UFS populated
([handoff.md](../handoff.md)).

### FEL entry and protocol

The media-boot top level samples the FEL pad before trying storage: it reads bit
8 of SYSCTRL `0x03000024` four times, and four low samples select FEL (observed
in the binary, `a8e31821…` `0x10134`, reached from `0xb30`). A board read of
`0x03000024` returned `0x00000101`, so the sampled bit was high and the board
would continue to storage; the user nonetheless observed automatic FEL entry
after a USB reconnect with a card inserted (*confirmed on a board*,
`rom-copy-20261016-084055/`, and [watchdog.md](../watchdog.md)). The forced-FEL
entry at offset `0x20` zeroes the shared SRAM from `0x44000` to `0xf3fff`, sets a
selector and reaches the FEL dispatcher at `0x2514`, bypassing the pad sample
(observed in the binary, `a8e31821…` `0x20`–`0x40`, `0x2514`).

The USB device side is NS-BROM. It presents a DesignWare USB gadget and answers
the Allwinner FEL protocol: a 16-byte request whose first halfword selects the
command. The dispatcher serves version, control read and write, legacy execute,
and the windowed upload, download and run commands; it checks every memory
address against a four-entry whitelist before access (observed in the binary,
`95be69c9…` dispatcher `0x19f40`, whitelist table `0x21370`). The command list
and the whitelist extents are in [`brom-tables.json`](data/brom-tables.json).
The identity response reports SoC `0x00001903`, protocol version 1 and
scratchpad `0x000ee800` (observed in the binary, `95be69c9…` `0x1a2c8`;
*confirmed on a board*, `~/.cache/sunstoneos/evidence/ddr-20261015-223537/`
version line).

### SRAM, stack and the shared-SRAM routing

The FEL stack and scratch live inside the shared SRAM: the whitelist names
`0x000f0f00`–`0x000f3400` as the FEL stack region and `0x000f3500`–`0x000f3ffc`
as upper scratch, and the scratchpad is `0x000ee800` (observed in the binary,
`95be69c9…` whitelist `0x21370`). The ROM's own stack sits in this SRAM as well.
Before its boot dispatch the ROM sets bit 1 of SYSCTRL `0x03000008`, which routes
the 512 KiB shared SRAM at `0x00074000`–`0x000f3fff` to the CPU rather than the
NPU (inferred from the instruction sequence, reported in
[reference-backsolve-npu.md](../reference-backsolve-npu.md); the manual does not
document this bit). A board read of `0x03000008` returned `2`, so bit 1 is set
and the shared SRAM is routed to the CPU on a freshly powered board entered
through FEL (*confirmed on a board*,
`~/.cache/sunstoneos/evidence/board-session-20261025-4/port-3-1/bb9-read/` and
`board-session-20261025-3/port-3-3/`, 21 of 21 predicted words matched on both
boards). This routing is the one BROM behaviour whose omission changes an
accelerator result: U-Boot clears the bit to give the SRAM back to the NPU, and
SunstoneOS images loaded through FEL never do, which
[reference-backsolve-npu.md](../reference-backsolve-npu.md) ties to the
input-independent NPU output of an earlier run.

### Registers the BROM initialises

The ROM programs the CPU PLLs during its clock-init routine (offset `0x21cc`).
It reads a two-bit speed grade from bits 15:14 of RTC/boot register
`0x07090160`, selects a base multiplier (25 for grade 1, 19 for grade 2, else
20) and scales it by a grade multiplier, then programs each CPU PLL
(observed in the binary, `a8e31821…` `0x21cc`, `0x13f4`, `0x1960`). The clock
registers are in the CPU_PLL_CFG block at `0x08870000`.

| Address | Value on the board | Order | Delay | Purpose |
| --- | --- | --- | --- | --- |
| `0x08871000` ctrl | `0xf8802600` (N=38) | before mux | 10 µs lock waits | CPU-L PLL enable, N and lock |
| `0x0887101c` mux | `0x03010000` (src 3, P=2) | after ctrl | 10 µs | CPU-L runs from its PLL ÷2 |
| `0x08872000` ctrl | `0xf8802700` (N=39) | with CPU-L | 10 µs | CPU-B PLL |
| `0x0887201c` mux | `0x03000000` (src 3, P=1) | after ctrl | 10 µs | CPU-B runs from its PLL ÷1 |
| `0x08873000` ctrl | `0xf8802600` (N=38) | with CPU-L | 10 µs | DSU PLL |
| `0x0887301c` mux | `0x03010105` (src 3, P=2) | after ctrl | 10 µs | DSU runs from its PLL ÷2 |

The board values are *confirmed on a board*
(`~/.cache/sunstoneos/evidence/cpu-clock-initial.json` and
`board-session-20261025-1/port-3-3` bundle 14). The computed CPU-L rate is
26 MHz × 38 ÷ 2 = 494 MHz; see the N=38 note under boot0 below, which applies to
whichever stage last wrote the PLL. The ROM also sets the progress record at
`0x07090304` and de-asserts module gates, including both crypto-engine bus
gates, in its secondary clock setup (observed in the binary, `a8e31821…` `0x264`,
`0x2444`; reported for remote-writer ownership in
[remote-writers.md](../remote-writers.md)). The complete recovered MMIO map is
in [`brom-mmio.json`](data/brom-mmio.json).

### Secure-boot checks and the state left for clients

The published dumps are the normal (non-monitor) BROM. The manual states that
secure boot verifies boot0 with ECDSA (256/384/521) or RSA (2048/3072/4096) and
SHA-256 or SHA-512, and that the verifying code lives in the BROM Monitor, which
is not in these blobs (A733 User Manual V1.00, section 3.1; signature constants
not present in `3a2a7d28…`). The FEL dispatcher reads a SID security word at
`0x030060a0` and configures an SMC0 secure-region default, but the enforcement
path is in the absent monitor (observed in the binary, `a8e31821…` `0x2a0c`,
`0x24d8`). On exit the ROM leaves: the shared SRAM routed to the CPU; the CPU
PLLs running as above; the crypto engine out of reset; the FEL stack and
scratchpad live in SRAM for a FEL client; and, for a media boot, boot0 copied
to `0x47000` and entered in AArch32. Which bus masters it leaves out of reset is
recorded, as far as the literal scan proves it, in
[remote-writers.md](../remote-writers.md).

## boot0

boot0 is the vendor first-stage loader. SunstoneOS does not run boot0 as a boot
stage; it calls boot0's board-setup routine (clock and PMIC) and its DRAM
training routine from a RAM wrapper ([boot.md](../boot.md)), and it reuses
boot0's package parser and
handoff logic as a model for its own cold entry ([cold-entry.md](../cold-entry.md)).
The curated inventories are [`boot0-functions.json`](data/boot0-functions.json),
[`boot0-mmio.json`](data/boot0-mmio.json) and
[`boot0-tables.json`](data/boot0-tables.json).

### Entry contract

The ROM enters boot0 at `0x47000`, which branches to the reset stub at `0x47b00`.
The stub enters AArch32 supervisor mode, clears the MMU-enable, alignment and
data-cache bits of SCTLR and sets the instruction-cache bit, sets the stack to
`0x8d000`, clears the BSS and calls `main` at `0x5e158` (observed in the binary,
boot0 `829ea8e3…` `0x47b00`–`0x47b5c`). boot0 therefore expects to start with
caches and translation off and a usable SRAM stack; SunstoneOS's RAM wrapper
reproduces that state before calling into the blob
([boot/a733/ddr-entry.S](../../boot/a733/ddr-entry.S)).

### Clock and PLL setup

`main` calls board setup at `0x48fac`, which reads the chip-version nibble at
SYSCTRL `0x03000024`, selects a target rate and calls the CPU-PLL routine at
`0x491b8` (observed in the binary, `829ea8e3…` `0x48fac`, `0x491b8`). That
routine first sets bit 24 of ten CCU PLL control words, among them PLL_GPU
`0x020020e0` and PLL_NPU `0x020022a0`. The words then read `0x41104500`
on the board where the manual gives a `0x40104500` default, and the PLLs stay
disabled (observed in the binary, `829ea8e3…` `0x491d4`–`0x49220`; the GPU case
is in [reference-backsolve-gpu.md](../reference-backsolve-gpu.md) and the NPU
case in [reference-backsolve-npu.md](../reference-backsolve-npu.md)). It then
programs the CPU-L, CPU-B and DSU PLLs unless bit 14 of `0x07090160` is set, in
which case it leaves them as the ROM set them (observed in the binary,
`829ea8e3…` `0x49224`–`0x49242`, literal `0x07090160` at `0x494b8`).

| Address | Value | Order | Delay | Purpose |
| --- | --- | --- | --- | --- |
| each PLL ctrl `0x088710/20/3000` | set bit 24 | first | — | PLL update request per domain |
| PLL ctrl | park mux on 24 MHz, gate output | next | 20 µs | safe reprogramming |
| PLL ctrl | write N (bits 15:8) | next | 20 µs | set multiplier |
| PLL ctrl | set update bit 26 | next | wait clear + 3 lock reads | apply, no timeout |
| PLL ctrl | ungate output | next | 20 µs | release |
| mux `…101c/201c/301c` | select source 3 (own PLL) | last | 20 µs each | run core from PLL |

The rates and the board-observed PLL words are in
[`boot0-tables.json`](data/boot0-tables.json) and match the BROM table above.

**The N=38 rate.** CPU-L runs at 494 MHz because its PLL word holds N=38 with a
post-divider of 2, and 26 MHz × 38 ÷ 2 = 494 MHz. N=38 is not a direct constant
in either binary. It is computed as a speed-grade base multiplier of 19,
doubled by the grade's scaling factor (19 × 2 = 38), read from bits 15:14 of
`0x07090160` (inferred from the BROM clock-init at `a8e31821…` `0x21cc`,
`0x13f4`, which performs the multiply, and corroborated by the board PLL word
`0xf8802600`). CPU-B reaches 1014 MHz by the same path with a ÷1 post-divider
and N=39. boot0's own PLL routine recomputes the same value unless the
`0x07090160` gate leaves the ROM's result in place; both stages reach N=38, so
the configured rate is stable across the handoff. The remaining uncertainty is
only which stage performs the final write, not the value.

### PMIC rails over TWI

boot0 talks to the AXP8191 PMIC over TWI through a register table of 36
regulators at `0x47000+0x81e98`, each entry giving the name, voltage range,
voltage register and enable bit (observed in the binary, `829ea8e3…` table at
`0x81e98`). The rail map and the full table are in
[`boot0-tables.json`](data/boot0-tables.json). boot0 writes only the DRAM supply
and a GPU-rail enable.

| Rail | Register | Action | Order | Delay | Purpose |
| --- | --- | --- | --- | --- | --- |
| DCDC2 | `0x13` voltage, `0x10` bit 1 enable | set voltage code, then enable | before DRAM training | none | DRAM/NPU supply, 800 or 840 mV |
| DCDC4 | `0x10` bit 3 enable | enable only, voltage left at default | board setup | none | GPU rail at PMIC power-on voltage |

boot0 writes no CPU rail; DCDC3 (CPU-B) and DCDC5 (CPU-L and DSU) read 800 mV on
the board at the PMIC power-on default (observed in the binary, `829ea8e3…` DRAM
supply routine `0x4b618`; GPU enable `0x16940`; *confirmed on a board*,
`board-session-20261025-1/port-3-3` bundle 15, and
[reference-backsolve-cpu.md](../reference-backsolve-cpu.md)). The PMIC
write-voltage path sets the voltage code before the enable bit with no delay
(observed in the binary, `829ea8e3…` `0x5d760`).

### DRAM training

boot0 holds up to 15 DRAM parameter sets in a table at `0x47374` and selects one
from a GPADC channel and a GPIO, per the `sys_config` `dram_select_para`
settings (observed in the binary, `829ea8e3…` selector `0x4b2e8`, table
`0x47374`; the thresholds are `[163,382,608,811,1050,1315,1569,1800]`). Three
sets are populated, all LPDDR5; the board selected set 5 (observed in the
binary; *confirmed on a board*, the DRAM log line
`vaild para:4  select dram para5`). `init_DRAM` at `0x49bdc` sets R_CCU
`0x07010250` bit 0, programs the mode registers, runs per-P-state training and
scans rank and size (observed in the binary, `829ea8e3…` `0x49bdc`).

The result record is the 128-byte parameter block, which the loader writes back
after training: the board read showed the selected 2040 MHz set, DRAM_VCC
560 mV, LPDDR5, four trained P-states at 2040, 1165, 816 and 408 MHz, a training
result code of 7, and a size of 12,288 MiB, with runtime `para1 = 0xa11a`,
`para2 = 0x30001001` and `dram_tpr13 = 0x10065` (*confirmed on a board*,
`~/.cache/sunstoneos/evidence/ddr-20261015-223537/firmware.log`; the stored
parameter block is in `dram-parameters.bin`). The parameter sets and the result
record are tabulated in [`boot0-tables.json`](data/boot0-tables.json). SunstoneOS
reruns this path through its wrapper; it does not reimplement training
([dram.md](../dram.md), [boot.md](../boot.md)).

### What boot0 parses from the boot package

The package parser at `0x5dc80` loads a TOC1 container, verifies its header
(magic `0x89119800`, additive checksum with the checksum field replaced by
`0x5f0a6c39` during the sum), and walks its items by name (observed in the
binary, `829ea8e3…` parser `0x5dc80`, header loader `0x5dc30`; the stock
container names `u-boot`, `monitor` and `scp` in
`~/.cache/sunstoneos/sources/board-boot/boot_package.cfg`). For each matched
item it copies the payload to its run address and records the pointer. It takes
the monitor load address from payload offset `0x2c`, not from the TOC
`run_addr`, and then overwrites that word with the DRAM size in MiB and copies
the DRAM parameter block and header fields into the monitor header (observed in
the binary, `829ea8e3…` `0x5dc80`–`0x5e0fc`; the full header mutation and
offsets are in [cold-entry.md](../cold-entry.md) and the evidence file
`~/.cache/sunstoneos/evidence/cold-entry-20261020/boot0-parser.txt`). The item
names, the checksum rule and the header mutation are in
[`boot0-tables.json`](data/boot0-tables.json).

### Handoff to the monitor and the SCP

`main` jumps to the next stage by type: to the ATF monitor with the monitor,
U-Boot and OP-TEE base pointers; to OP-TEE; to U-Boot; or to an RTOS (observed
in the binary, `829ea8e3…` handoff strings and code at `0x5e158`,
`0x6055a`–`0x605f7`). The jump requests an AArch64 warm reset to the monitor
address; the register state it passes is the mutated monitor header, with the
DRAM size at `+0x2c`, the DRAM parameters at `+0x30`, and the U-Boot and OP-TEE
entry pointers at `+0x10` and `+0x18` (observed in the binary; modelled by the
cold-entry check in [cold-entry.md](../cold-entry.md)). An `scp` item is copied
to DRAM at `0x48100000`, but boot0 starts no E902 core and contains no literal
of the E902 start registers (observed in the binary; reported in
[remote-writers.md](../remote-writers.md)). Its SMHC0 call on the handoff path
reaches a stub that returns 0, so the SD controller, clock and pins are left in
boot0's transfer state ([boot-slots.md](../boot-slots.md)).

### What boot0 deliberately does not do

boot0 makes no write to the PCK-600 power controller, the GPU register window,
SMC0 or the GPU clock words, and it writes no DCDC4 voltage, so the GPU rail
stays at the PMIC default and the GPU PLL stays disabled
([reference-backsolve-gpu.md](../reference-backsolve-gpu.md)). It does not clear
the shared-SRAM routing bit, leaving the SRAM with the CPU
([reference-backsolve-npu.md](../reference-backsolve-npu.md)). The normal-boot
path performs further NSI interconnect writes after DRAM init that the
FEL-loaded wrapper path skips ([reference-backsolve-gpu.md](../reference-backsolve-gpu.md),
boot0 `0x1f38`). boot0 starts no remote processor.

## SunstoneOS's use of these binaries

SunstoneOS depends on, reimplements or deliberately avoids specific behaviours:

- **DRAM, clock and PMIC bring-up: reused.** The RAM wrapper calls boot0's board
  setup (`0x48fad`) and `init_DRAM` (`0x49bdd`) and copies the selected parameter
  set, with UART output redirected to an SRAM log
  ([boot/a733/ddr-entry.S](../../boot/a733/ddr-entry.S),
  [tools/prepare-ddr.py](../../tools/prepare-ddr.py)). SunstoneOS does not
  reimplement DRAM training ([dram.md](../dram.md)).
- **CPU clock decode: reimplemented read-only.** The CPU clock driver decodes the
  same PLL and mux words that boot0 and the BROM leave, including the 494 MHz
  CPU-L rate, without rewriting them
  ([cpu-clock.md](../cpu-clock.md), [drivers/clock/a733_cpu_clock.c](../../drivers/clock/a733_cpu_clock.c)).
- **The boot-package parser and monitor handoff: reimplemented as a model.** The
  cold-entry recorder and the stage-2 slot loader follow boot0's parser, header
  mutation and warm-reset handoff
  ([cold-entry.md](../cold-entry.md), [boot-slots.md](../boot-slots.md),
  [boot/a733/cold-entry.S](../../boot/a733/cold-entry.S)).
- **Forced-FEL return: reused.** SunstoneOS returns to the ROM through the
  forced-FEL entry at `0x20` with an AArch32 warm reset
  ([watchdog.md](../watchdog.md), [boot.md](../boot.md)).
- **Shared-SRAM routing: not yet reproduced.** SunstoneOS inherits the BROM's
  CPU routing and has not added the U-Boot write that gives the SRAM to the NPU;
  [reference-backsolve-npu.md](../reference-backsolve-npu.md) specifies the fix.
- **Host microSD packaging: reuses the pinned boot0 and packer.** The card
  packager places the unmodified pinned boot0 in the container
  ([microsd-boot-package.md](../microsd-boot-package.md)).

## Open questions the binaries cannot answer

- **Secure-boot enforcement.** The signature and hash verification lives in the
  HS-BROM and the BROM Monitor, neither of which is in the published dumps, so
  the exact certificate format and the secure-world entry cannot be derived
  (`3a2a7d28…` contains no signature constants).
- **SYSCTRL `0x03000008` semantics.** The manual does not document the register.
  Bit 1 routing the shared SRAM to the CPU is inferred from the BROM and from
  U-Boot source; the other bits and the exact transition rules are unknown.
- **Which stage performs the final CPU PLL write.** The BROM and boot0 both
  compute N=38, gated on `0x07090160` bit 14; which write the board's value comes
  from is not resolved from the binaries alone.
- **The DRAM firewall and SMC0/SMC1 programming.** boot0 writes the MEMC and SMC
  registers during training, but the register fields are undocumented, so the
  secure-region and QoS configuration cannot be decoded
  ([designs/dram-controller.md](../designs/dram-controller.md)).
- **Remote-processor ownership at handoff.** boot0 copies an SCP image without
  starting the E902, and neither binary contains the E902 or OR100 start
  registers, so which masters are left out of reset at cold boot is not fully
  proven ([remote-writers.md](../remote-writers.md)).
- **The GPU PLL bit 24 and NPU PLL bit 24.** boot0 sets bit 24 of these PLL
  control words; the manual marks the affected field reserved, so the effect is
  unknown ([reference-backsolve-gpu.md](../reference-backsolve-gpu.md)).
