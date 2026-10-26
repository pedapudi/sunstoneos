# The A733 boot ROM and boot0 vendor binaries

The Allwinner A733 starts from an on-chip boot ROM (BROM) that SunstoneOS
cannot replace. The board's microSD boot package begins with a vendor
first-stage loader, boot0, that SunstoneOS reuses for DRAM and clock bring-up
but does not otherwise run. This document records what those binaries do, so
that a developer can rely on them, reimplement a part, or decide an ownership
question without disassembling them again. The
[vendor firmware overview](README.md) pins each binary and gives the fetch
commands.

Each fact carries one of the labels observed, confirmed on a board or
inferred, and a citation `HASH@X`, as the
[overview defines them](README.md#evidence-labels-and-citations). Boot ROM
citations give ROM addresses, and boot0 citations give load addresses (file
offset + `0x47000`). The full inventories are outside Git under
`~/.cache/sunstoneos/evidence/vendor-firmware-20261025/boot/`; the curated
extracts are the JSON files under [`data/`](data/), linked from each section.
Register tables give the address, the value, the order, any delay and the
purpose. No disassembly listing or binary byte sequence is reproduced here.
Related SunstoneOS records cover the surrounding boot path:

- [boot.md](../boot.md): the cold-start sequence and the physical bring-up
  probes;
- [cold-entry.md](../cold-entry.md): the SunstoneOS EL3 payload and its
  handoff capture;
- [boot-slots.md](../boot-slots.md): the two-slot card layout;
- [microsd-boot-package.md](../microsd-boot-package.md): the host packaging of
  a card image;
- [watchdog.md](../watchdog.md): the forced-FEL reset path and the captured
  board ROM.

## Identity and provenance

| Binary | Role | SHA-256 | Size | Load address |
| --- | --- | --- | --- | --- |
| FIXS-BROM | Reset, media boot and FEL dispatch | `3a2a7d28…` | 69,632 B | `0x00000000` |
| NS-BROM | USB FEL device-side protocol | `95be69c9…` | 45,056 B | `0x00017000` |
| boot0 | First-stage loader | `829ea8e3…` | 245,760 B | `0x00047000` |
| boot0, 1800M variant | Same clock, PMIC and parser code; other DRAM tables and training code | `d2a9bccc…` | 245,760 B | `0x00047000` |
| Board BROM capture | FIXS-BROM read from the original board | `a8e31821…` | 69,632 B | `0x00000000` |

The two BROM images come from the published `smaeul/sunxi-blobs` dump at
revision `42aabf4f2c93158a0cd88a000f9af56ce024882a`, path `sun60iw2p1/`, decoded
from hexadecimal to binary. The manual's memory map places FIXS-BROM at
`0x00000000`–`0x00010FFF` (68 KiB), a high-security HS-BROM at
`0x00011000`–`0x00016FFF`, and NS-BROM at `0x00017000`–`0x00021FFF` (44 KiB)
(A733 User Manual V1.00, section 2). The published FIXS dump and the board
capture (`a8e31821…`) cover the same 68 KiB region. Thirteen words in their
first 1 KiB differ, while the reset and redirect code through offset `0x228`
is identical. The board therefore runs a close revision of the published ROM
(confirmed on a board, `~/.cache/sunstoneos/evidence/rom-copy-20261016-084055/`).
The board capture was read through the FEL tool's `memmove` command, which
copies ROM through SRAM, because a direct FEL read at address zero is refused
([watchdog.md](../watchdog.md#captured-board-rom-and-reset-ownership)). The
HS-BROM and the secure BROM Monitor are not in either dump.

boot0 comes from the Orange Pi build repository
`orangepi-xunlong/orangepi-build` at revision
`a95129bf30ea11886cc35f08ebb3e69695a26656`, path
`external/packages/pack-uboot/sun60iw2/bin/`. The fetch commands for every
binary here are in the [vendor firmware overview](README.md).

boot0's header is an eGON.BT0 block (observed,
`829ea8e3@0x47000`–`0x47040`):

| Header offset | Field |
| --- | --- |
| `0x0` | Branch to the reset stub |
| `0x4` | Magic `eGON.BT0` |
| `0xc` | Additive 32-bit checksum, stock value `0x2fce7bb3` |
| `0x10` | Length `0x3c000` |
| `0x18` | Run address `0x47000` |
| `0x38` | 128-byte DRAM parameter block, overwritten at run time |

The `1800M` variant differs only in the DRAM parameter tables, the DRAM
training code and the checksum. All 113 byte runs that differ between
`829ea8e3…` and `d2a9bccc…` lie in the DRAM tables from `0x47374` and in the
training code; none lies in the clock, PMIC or package-parser code (observed). The source for boot0 is not published.

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
an eight-entry vector table based at `0x070501c4` in the CPUIDLE block (observed, `a8e31821@0x44`–`0x178`). Before that dispatch, two
software-redirect checks run on core 0: if `0x070501c0` holds `0xfa50392f`, the
ROM loads the program counter from `0x070501c4`; and if the low 16 bits of
`0x070501d4` equal `0xefe8`, it loads the counter from `0x070501d8` (observed, `a8e31821@0x164`, `0x1f0`; literals at `0x2c0`, `0x2d4`). These
are the recovery routes that [watchdog.md](../watchdog.md) evaluates; a board
read found all four words zero (confirmed on a board,
`rom-copy-20261016-084055/recovery-registers.json`).

### Boot-media order and selection

The ROM reads a BOOT_MODE bit to choose between a GPADC-pin selection and an
eFuse selection of the boot medium (A733 User Manual V1.00, section 3.1.3). The
media walker tries controllers in a fixed order and, on the first that returns a
valid boot0 header, copies boot0 to `0x47000` (observed,
`a8e31821@0xa00`–`0xad8`). The try order and the GPADC fallback order are in
[`brom-tables.json`](data/brom-tables.json). The board under test boots from the
microSD controller SMHC0; neither board has eMMC or UFS populated
([handoff.md](../handoff.md)).

### FEL entry and protocol

The media-boot top level samples the FEL pad before trying storage: it reads bit
8 of SYSCTRL `0x03000024` four times, and four low samples select FEL (observed, `a8e31821@0x10134`, reached from `0xb30`). A board read of
`0x03000024` returned `0x00000101`, so the sampled bit was high and the board
would continue to storage; the user nonetheless observed automatic FEL entry
after a USB reconnect with a card inserted (confirmed on a board,
`rom-copy-20261016-084055/`, and [watchdog.md](../watchdog.md)). The forced-FEL
entry at offset `0x20` zeroes the shared SRAM from `0x44000` to `0xf3fff`, sets a
selector and reaches the FEL dispatcher at `0x2514`, bypassing the pad sample
(observed, `a8e31821@0x20`–`0x40`, `0x2514`).

The USB device side is NS-BROM. It presents a DesignWare USB gadget and answers
the Allwinner FEL protocol: a 16-byte request whose first halfword selects the
command. The dispatcher serves version, control read and write, legacy execute,
and the windowed upload, download and run commands; it checks every memory
address against a four-entry whitelist before access (observed,
`95be69c9@0x19f40`, whitelist table at `0x21370`). The command list
and the whitelist extents are in [`brom-tables.json`](data/brom-tables.json).
The identity response reports SoC `0x00001903`, protocol version 1 and
scratchpad `0x000ee800` (observed, `95be69c9@0x1a2c8`;
confirmed on a board, `~/.cache/sunstoneos/evidence/ddr-20261015-223537/`
version line).

### SRAM, stack and the shared-SRAM routing

The FEL stack and scratch live inside the shared SRAM: the whitelist names
`0x000f0f00`–`0x000f3400` as the FEL stack region and `0x000f3500`–`0x000f3ffc`
as upper scratch, and the scratchpad is `0x000ee800` (observed,
whitelist table `95be69c9@0x21370`). The ROM's own stack sits in this SRAM as well.
Before its boot dispatch the ROM sets bit 1 of SYSCTRL `0x03000008`, which routes
the 512 KiB shared SRAM at `0x00074000`–`0x000f3fff` to the CPU rather than the
NPU (inferred from the instruction sequence, reported in
[NPU backsolve](../reference-backsolve-npu.md); the manual does not
document this bit). A board read of `0x03000008` returned `2`, so bit 1 is set
and the shared SRAM is routed to the CPU on a freshly powered board entered
through FEL (confirmed on a board,
`~/.cache/sunstoneos/evidence/board-session-20261025-4/port-3-1/bb9-read/` and
`board-session-20261025-3/port-3-3/`; all 21 predicted words matched on both
boards). This routing is the one BROM behaviour whose omission changes an
accelerator result. U-Boot clears the bit to give the SRAM back to the NPU,
and SunstoneOS images loaded through FEL never do. The
[NPU backsolve](../reference-backsolve-npu.md) ties this to the
input-independent NPU output of an earlier run.

### Registers the BROM initialises

The ROM programs the CPU-L and DSU PLLs during its clock-init routine
(offset `0x21cc`), and not the CPU-B PLL. It reads a two-bit speed grade from
bits 15:14 of RTC/boot register `0x07090160`, selects a base multiplier (25
for grade 1, 19 for grade 2, else 20) and scales it by a grade multiplier
(observed, `a8e31821@0x21cc`, `0x13f4`). The routine at `0x1898` then calls
the PLL writer at `0x1774` twice with that N, for `0x08871000` (CPU-L) and
`0x08873000` (DSU), the two PLL addresses in its literal pool
(`0x1f78`–`0x1fa0`); `0x1af0` selects both muxes, and `0x1960` programs a
fourth PLL word at `0x08870000` on one branch. No routine and no literal
names `0x08872000`, the CPU-B PLL. The clock registers are in the
CPU_PLL_CFG block at `0x08870000`.

| Address | Value on the board | Written by | Purpose |
| --- | --- | --- | --- |
| `0x08871000` ctrl | `0xf8802600` (N=38) | ROM, on every entry | CPU-L PLL enable, N and lock |
| `0x0887101c` mux | `0x03010000` (src 3, P=2) | ROM | CPU-L runs from its PLL ÷2 |
| `0x08873000` ctrl | `0xf8802600` (N=38) | ROM | DSU PLL |
| `0x0887301c` mux | `0x03010105` (src 3, P=2) | ROM | DSU runs from its PLL ÷2 |
| `0x08872000` ctrl | `0xf8802700` (N=39) | boot0's board setup, not the ROM | CPU-B PLL |
| `0x0887201c` mux | `0x03000000` (src 3, P=1) | boot0's board setup | CPU-B runs from its PLL ÷1 |

The values in the table are confirmed on a board
(`~/.cache/sunstoneos/evidence/cpu-clock-initial.json` and
`board-session-20261025-1/port-3-3` bundle 14). The computed CPU-L rate is
26 MHz × 38 ÷ 2 = 494 MHz. Each FEL return enters the ROM again, which
writes CPU-L and the DSU back to these words and leaves CPU-B as the last
image set it: in `board-session-cpu-top-opp-7/port-3-3`, after an image left
all three at their top points, the next image read CPU-L and the DSU at
`0xf8802600` and CPU-B still at `0xf8804d00` (2002 MHz). CPU-B's boot word
comes from boot0's board setup, which the SunstoneOS cold DDR start runs once
per power cycle. The ROM also sets the progress record at
`0x07090304` and de-asserts module gates, including both crypto-engine bus
gates, in its secondary clock setup (observed, `a8e31821@0x264`,
`0x2444`; reported for remote-writer ownership in
[remote-writers.md](../remote-writers.md)). The complete recovered MMIO map is
in [`brom-mmio.json`](data/brom-mmio.json).

### Secure-boot checks and the state left for clients

The published dumps hold the normal BROM, without the BROM Monitor. The manual
states that secure boot verifies boot0 with ECDSA (256-, 384- or 521-bit) or
RSA (2048-, 3072- or 4096-bit) signatures and SHA-256 or SHA-512 hashes. The
verifying code lives in the BROM Monitor, which is not in these dumps (A733
User Manual V1.00, section 3.1; `3a2a7d28…` holds no signature constants). The FEL dispatcher reads a SID security word at
`0x030060a0` and configures an SMC0 secure-region default, but the enforcement
path is in the absent BROM Monitor (observed, `a8e31821@0x2a0c`,
`0x24d8`). On exit the ROM leaves:

- the shared SRAM routed to the CPU;
- the CPU-L and DSU PLLs running as above, and the CPU-B PLL as the last
  writer left it;
- the crypto engine out of reset;
- the FEL stack and scratchpad live in SRAM for a FEL client;
- for a media boot, boot0 copied to `0x47000` and entered in AArch32.

[remote-writers.md](../remote-writers.md) records which bus masters the ROM
leaves out of reset, as far as the constant scan proves it.

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
`0x8d000`, clears the BSS and calls `main` at `0x5e158` (observed,
`829ea8e3@0x47b00`–`0x47b5c`). boot0 therefore expects to start with
caches and translation off and a usable SRAM stack; SunstoneOS's RAM wrapper
reproduces that state before calling into the blob
([boot/a733/ddr-entry.S](../../boot/a733/ddr-entry.S)).

### Clock and PLL setup

`main` calls board setup at `0x48fac`, which reads the chip-version nibble at
SYSCTRL `0x03000024`, selects a target rate and calls the CPU-PLL routine at
`0x491b8` (observed, `829ea8e3@0x48fac`, `0x491b8`). That
routine first sets bit 24 of the CCU PLL control words listed in a table at
`0x49484`, among them PLL_GPU `0x020020e0` and PLL_NPU `0x020022a0`. Both
words then read `0x41104500` on the board where the manual gives a
`0x40104500` default, and both PLLs stay disabled (observed,
`829ea8e3@0x491d4`–`0x49220`; the GPU case
is in [GPU backsolve](../reference-backsolve-gpu.md) and the NPU
case in [NPU backsolve](../reference-backsolve-npu.md)). It then
programs the CPU-L, CPU-B and DSU PLLs unless bit 14 of `0x07090160` is set, in
which case it leaves them as the ROM set them (observed,
`829ea8e3@0x49224`–`0x49242`, literal `0x07090160` at `0x494b8`).

| Address | Value | Order | Delay | Purpose |
| --- | --- | --- | --- | --- |
| mux `…101c/201c/301c`, PLL ctrl `0x08871000/2000/3000` | park mux on 24 MHz, gate output | first | 20 µs | safe reprogramming |
| PLL ctrl | write N (bits 15:8) | next | 20 µs | set multiplier |
| PLL ctrl | set update bit 26 | next | wait clear + 3 lock reads | apply, no timeout |
| PLL ctrl | ungate output | next | 20 µs | release |
| mux `…101c/201c/301c` | select source 3 (own PLL) | last | 20 µs each | run core from PLL |

The rates and the board-observed PLL words are in
[`boot0-tables.json`](data/boot0-tables.json) and match the BROM table above.

**The N=38 rate.** CPU-L runs at 494 MHz because its PLL word holds N=38 with a
post-divider of 2: 26 MHz × 38 ÷ 2 = 494 MHz. N=38 is not a direct constant in
either binary. The BROM clock-init reads the speed grade from bits 15:14 of
`0x07090160`, takes the base multiplier 19 for grade 2 and doubles it, which
gives 38 (inferred from `a8e31821@0x21cc` and `@0x13f4`, which perform the
multiply; the board PLL word `0xf8802600` agrees). CPU-B runs at 1014 MHz with
N=39 and a ÷1 post-divider. boot0 reprograms the CPU PLLs unless bit 14 of
`0x07090160` is set; the board's word has bit 14 clear. The
[CPU backsolve](../reference-backsolve-cpu.md) reads boot0's two rate targets
as matching CPU-B's N=39 but not N=38. The ROM is the last writer of CPU-L
and the DSU: it rewrites them with N=38 on every entry after boot0, and it
never writes CPU-B, so CPU-B keeps boot0's N=39.

### PMIC rails over TWI

boot0 talks to the AXP8191 PMIC over TWI through a register table of 36
regulators at `0x81e98`, each entry giving the name, voltage range,
voltage register and enable bit (observed, table at `829ea8e3@0x81e98`, file
offset `0x3ae98`). The rail map and the full table are in
[`boot0-tables.json`](data/boot0-tables.json). boot0 writes only the DRAM supply
and a GPU-rail enable.

| Rail | Register | Action | Order | Delay | Purpose |
| --- | --- | --- | --- | --- | --- |
| DCDC2 | `0x13` voltage, `0x10` bit 1 enable | set voltage code, then enable | before DRAM training | none | DRAM/NPU supply, 800 or 840 mV |
| DCDC4 | `0x10` bit 3 enable | enable only, voltage left at default | board setup | none | GPU rail at PMIC power-on voltage |

boot0 writes no CPU rail; DCDC3 (CPU-B) and DCDC5 (CPU-L and DSU) read 800 mV on
the board at the PMIC power-on default (observed, DRAM supply routine
`829ea8e3@0x4b618` and GPU enable `@0x5d940`; confirmed on a board,
`board-session-20261025-1/port-3-3` bundle 15, and
[CPU backsolve](../reference-backsolve-cpu.md)). The PMIC
write-voltage path sets the voltage code before the enable bit with no delay
(observed, `829ea8e3@0x5d760`).

### DRAM training

boot0 holds up to 15 DRAM parameter sets in a table at `0x47374` and selects one
from a GPADC channel and a GPIO, per the `sys_config` `dram_select_para`
settings (observed, selector `829ea8e3@0x4b2e8`, table
`@0x47374`; the thresholds are `[163,382,608,811,1050,1315,1569,1800]`). Three
sets are populated, all LPDDR5; the board selected set 5 (observed; confirmed on a board, the DRAM log line
`vaild para:4  select dram para5`). `init_DRAM` at `0x49bdc` sets PRCM
`0x07010250` bit 0, programs the mode registers, runs per-P-state training and
scans rank and size (observed, `829ea8e3@0x49bdc`).

The result record is the 128-byte parameter block, which the loader writes back
after training. The board read showed the selected 2040 MHz LPDDR5 set with
DRAM_VCC at 560 mV and four trained P-states at 2040, 1165, 816 and 408 MHz.
It also showed a training result code of 7, a size of 12,288 MiB, and the run
time values `para1 = 0xa11a`, `para2 = 0x30001001` and
`dram_tpr13 = 0x10065` (confirmed on a board,
`~/.cache/sunstoneos/evidence/ddr-20261015-223537/firmware.log`; the stored
parameter block is in `dram-parameters.bin`). The parameter sets and the result
record are tabulated in [`boot0-tables.json`](data/boot0-tables.json). SunstoneOS
reruns this path through its wrapper; it does not reimplement training
([dram.md](../dram.md), [boot.md](../boot.md)).

### What boot0 parses from the boot package

The package parser at `0x5dc80` loads a TOC1 container and verifies its
header: the magic is `0x89119800`, and the additive checksum treats the
checksum field as `0x5f0a6c39` during the sum. It then walks the items by name (observed, parser `829ea8e3@0x5dc80`, header loader `@0x5dc30`; the stock
container names `u-boot`, `monitor` and `scp` in
`~/.cache/sunstoneos/sources/board-boot/boot_package.cfg`). For each matched
item it copies the payload to its run address and records the pointer. It
takes the BL31 load address from payload offset `0x2c` rather than from the
TOC `run_addr`. It then overwrites that word with the DRAM size in MiB, and
copies the DRAM parameter block and header fields into the BL31 header
(observed, `829ea8e3@0x5dc80`–`0x5e0fc`; the full header mutation and
offsets are in [cold-entry.md](../cold-entry.md) and the evidence file
`~/.cache/sunstoneos/evidence/cold-entry-20261020/boot0-parser.txt`). The item
names, the checksum rule and the header mutation are in
[`boot0-tables.json`](data/boot0-tables.json).

### Handoff to BL31 and the SCP image

`main` jumps to the next stage by type: to BL31 with the BL31,
U-Boot and OP-TEE base pointers; to OP-TEE; to U-Boot; or to an RTOS (observed, handoff code and strings at `829ea8e3@0x5e158` and
`@0x6055a`–`0x605f7`). The jump requests an AArch64 warm reset to the BL31
address. The state it passes is the mutated BL31 header: the DRAM size at
`+0x2c`, the DRAM parameters at `+0x30`, and the U-Boot and OP-TEE entry
pointers at `+0x10` and `+0x18` (observed; modelled by the
cold-entry check in [cold-entry.md](../cold-entry.md)). An `scp` item is copied
to DRAM at `0x48100000`, but boot0 starts no E902 core and contains no literal
of the E902 start registers (observed; reported in
[remote-writers.md](../remote-writers.md)). Its SMHC0 call on the handoff path
reaches a stub that returns 0, so the SD controller, clock and pins are left in
boot0's transfer state ([boot-slots.md](../boot-slots.md)).

### Writes boot0 does not make

boot0 makes no write to the PCK-600 power controller, the GPU register window,
SMC0 or the GPU clock words. It writes no DCDC4 voltage, so the GPU rail stays
at the PMIC default, and the GPU PLL stays disabled
([GPU backsolve](../reference-backsolve-gpu.md)). It does not clear
the shared-SRAM routing bit, leaving the SRAM with the CPU
([NPU backsolve](../reference-backsolve-npu.md)). The normal-boot
path performs further NSI interconnect writes after DRAM init that the
FEL-loaded wrapper path skips (`829ea8e3@0x48f38`;
[GPU backsolve](../reference-backsolve-gpu.md)). boot0 starts no remote processor.

## SunstoneOS's use of these binaries

SunstoneOS uses these binaries as follows:

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
- **The boot-package parser and BL31 handoff: reimplemented as a model.** The
  cold-entry recorder and the stage-2 slot loader follow boot0's parser, header
  mutation and warm-reset handoff
  ([cold-entry.md](../cold-entry.md), [boot-slots.md](../boot-slots.md),
  [boot/a733/cold-entry.S](../../boot/a733/cold-entry.S)).
- **Forced-FEL return: reused.** SunstoneOS returns to the ROM through the
  forced-FEL entry at `0x20` with an AArch32 warm reset
  ([watchdog.md](../watchdog.md), [boot.md](../boot.md)).
- **Shared-SRAM routing: not yet reproduced.** SunstoneOS inherits the BROM's
  CPU routing and has not added the U-Boot write that gives the SRAM to the NPU;
  [NPU backsolve](../reference-backsolve-npu.md) specifies the fix.
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
- **The DRAM firewall and SMC0/SMC1 programming.** boot0 writes the MEMC and SMC
  registers during training, but the register fields are undocumented, so the
  secure-region and QoS configuration cannot be decoded
  ([designs/dram-controller.md](../designs/dram-controller.md)).
- **Remote-processor ownership at handoff.** boot0 copies an SCP image without
  starting the E902, and neither binary contains the E902 or OR100 start
  registers, so which masters are left out of reset at cold boot is not fully
  proven ([remote-writers.md](../remote-writers.md)).
- **Bit 24 of the CCU PLL words.** The manual marks the field reserved, so the
  effect of boot0 setting it is unknown
  ([GPU backsolve](../reference-backsolve-gpu.md)). The table at
  `829ea8e3@0x49484` lists thirteen PLL words, from `0x02002020` to
  `0x020022e0`. The function inventory and the NPU backsolve count ten words
  starting at PLL_GPU, and the GPU backsolve counts thirteen; the loop bounds
  decide which count is right. Both counts include PLL_GPU and PLL_NPU.
