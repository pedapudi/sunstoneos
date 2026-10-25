# Vendor firmware and closed binaries

The Allwinner A733 boots and runs through a chain of vendor binaries: two boot
ROMs, the first-stage loader boot0, the Trusted Firmware-A secure monitor
BL31, the firmware of the system control processor (SCP), the GPU firmware
and the NPU runtime libraries. SunstoneOS replaces some of them, calls into
others and loads a few unchanged. This directory describes each binary, pins
it by download location, SHA-256 and size, and records the facts derived from
it in curated inventories under [data/](data/).

This page is the overview. It explains how the binaries fit together and
which ones SunstoneOS relies on, and it summarises which peripherals each
binary programs. The analysis documents listed in the
[reading guide](#reading-guide) hold the detail. The tables from
[Fetch and verify the binaries](#fetch-and-verify-the-binaries) onwards pin
38 files; [sources.json](sources.json) is their machine-readable copy.

## The vendor boot chain and runtime

On the vendor's Linux system the binaries run in this order:

1. **Boot ROM.** After reset, Cortex-A55 core 0 runs the fixed secure boot ROM
   (FIXS-BROM) at address `0x0`. It programs the CPU PLLs, routes the 512 KiB
   shared SRAM at `0x74000` to the CPU by setting bit 1 of SYSCTRL
   `0x03000008`, and samples the FEL pad. FEL is the ROM's USB recovery
   protocol, served by the nonsecure boot ROM (NS-BROM) at `0x17000`. Without
   FEL, the ROM copies boot0 from the boot medium to SRAM at `0x47000` and
   enters it in AArch32.
2. **boot0.** The first-stage loader sets the CPU PLLs, enables the DRAM and
   GPU supplies on the AXP8191 PMIC, and trains DRAM. It then parses the
   second-stage boot package (TOC1). The package holds U-Boot, BL31 and the
   SCP image. boot0 copies BL31 to `0x48000000` and the SCP image to DRAM at
   `0x48100000`, writes the DRAM size and parameters into the BL31 header,
   and warm-resets core 0 into BL31 in AArch64.
3. **BL31.** The EL3 secure monitor initialises the GIC-600, makes most
   peripherals, clock registers and DMA channels Nonsecure, and serves PSCI
   calls for core power. It enters U-Boot at EL1 and stays resident.
4. **U-Boot.** U-Boot clears SYSCTRL `0x03000008` bit 1, which gives the
   shared SRAM to the NPU. Just before it starts Linux, it issues SMC
   `0x8000ff10`, and BL31 copies the SCP image into SRAM A2 and starts the
   E902 RISC-V core that runs it.
5. **SCP firmware.** The SCP polls a message box for BL31's requests. It runs
   system suspend and resume, power-off and reset through the PMIC, and
   wakeup-source configuration. It has no frequency-scaling or thermal code.
6. **GPU firmware.** The Linux GPU driver loads the PowerVR firmware into the
   GPU's internal MIPS processor and exchanges commands with it through
   shared-memory queues.
7. **NPU runtime.** The NPU has no firmware. The `vipcore` kernel driver
   programs the NPU registers. The user-space libraries `libVIPhal.so` and
   `libNBGlinker.so` load compiled models, and reach the driver through one
   `ioctl`.

SunstoneOS takes this position for each binary:

| Binary | Runs on | SunstoneOS | Analysis |
| --- | --- | --- | --- |
| Boot ROM (FIXS-BROM and NS-BROM) | Cortex-A55 core 0 | Relies on it. Every image is loaded over FEL, and images return to the ROM through its forced-FEL entry | [Boot ROM and boot0](boot-rom-and-boot0.md) |
| boot0 | Cortex-A55 core 0, AArch32 | Calls its board-setup and DRAM-training routines from a RAM wrapper. Reimplements its package parser and BL31 handoff. The microSD package carries it unmodified | [Boot ROM and boot0](boot-rom-and-boot0.md) |
| BL31 | Every Cortex-A55 and Cortex-A76 core, EL3 | Replaces it with the SunstoneOS EL3 monitor, which reproduces the CPU_ON register sequence | [TF-A monitor](tf-a-monitor.md) |
| SCP firmware | E902 RISC-V core | Never starts it. Core power-on, power-domain and PMIC control are reimplemented on the application cores; system suspend is not implemented | [SCP firmware](scp-firmware.md) |
| GPU firmware | GPU's internal MIPS processor | Loads the published image unmodified and reimplements the host side from the open driver | [GPU and NPU binaries](gpu-and-npu-binaries.md) |
| NPU runtime libraries | Linux user space | Does not load them. The native NPU driver and model loader reimplement the kernel driver and the libraries; the libraries serve as format and loader references | [GPU and NPU binaries](gpu-and-npu-binaries.md) |
| Radio firmware | AIC8800D80 radio | Loads it unmodified over SDIO | [Radio firmware](../radio-firmware.md) |

Two differences from the vendor chain follow from loading every image over
FEL. U-Boot never runs, so the shared SRAM stays routed to the CPU; the NPU
backsolve ties this routing to the input-independent output of an earlier NPU
run. BL31 never runs either, so its security setup is absent. A SunstoneOS
image that made BL31's security writes and applied the vendor's GPU supply
voltage and PLL rate read the GPU core identity
([TF-A monitor](tf-a-monitor.md#cold-boot-register-writes)).

## Reading guide

- [Boot ROM and boot0](boot-rom-and-boot0.md): ROM reset path, boot-media and
  FEL dispatch, the CPU PLL rates, boot0's PMIC rails, DRAM training, package
  parser and handoff.
- [TF-A monitor](tf-a-monitor.md): BL31's memory map, cold-boot register
  writes, PSCI power sequences, GIC setup, SMC services and the SCP start.
- [SCP firmware](scp-firmware.md): start-up, the message protocol, system
  suspend and resume, power-off, wakeup sources and DRAM retention.
- [GPU and NPU binaries](gpu-and-npu-binaries.md): the GPU firmware container
  and host interface, the NPU libraries, driver commands, model format and
  feature database.
- [Radio firmware](../radio-firmware.md): artifact selection, upload and start.
- [microSD packaging](../microsd-boot-package.md): the boot-package packer and
  the TOC1 format.

Three backsolve documents derive each bring-up sequence from these binaries,
the vendor sources and the manuals, and compare it with SunstoneOS. Each is on
its own branch, and its link resolves once that branch merges:

- [CPU backsolve](../reference-backsolve-cpu.md): secondary-core power-up, EL3 and GIC setup, idle and DVFS.
- [GPU backsolve](../reference-backsolve-gpu.md): the vendor GPU state from cold boot to firmware start.
- [NPU backsolve](../reference-backsolve-npu.md): NPU power, clocks, reset, shared-SRAM routing and initialisation.

### Terms used across the documents

- **CPUS domain:** the always-on subsystem at `0x07000000`. It holds the E902
  core, PRCM, RTC, S_TIMER, S_UART, S_TWI, S_MBOX and the PCK-600.
- **PRCM:** the CPUS-domain clock and power controller at `0x07010000`. The
  manual's memory map calls it S_PRCM and the vendor clock driver calls it
  R_CCU.
- **CPUIDLE:** the block at `0x07050000` that holds the per-core power
  requests and status words.
- **PCK-600 PPU:** one power-policy unit per power domain at
  `0x07060000 + 0x1000·n`. Domain 4 is the NPU, 5 the GPU top (TOP5) and 6
  the GPU core (CORE6).
- **SYSCTRL:** the system control block at `0x03000000`; `+0x08` bit 1 routes
  the shared SRAM, and `+0x24` holds the chip version.
- **SCP:** the system control processor, an E902 RISC-V core. Allwinner's
  older name for its firmware is arisc or AR100.
- **BVNC:** the PowerVR GPU's revision identifier, 36.56.104.183 on the A733.
- **NBG:** a network binary graph, the NPU's compiled-model file.

## Evidence labels and citations

Every document labels each fact with one of three labels:

- **observed:** read from the instructions or data of the binary at the cited
  location;
- **confirmed on a board:** a read on an A733 board matched, and the
  evidence directory under `~/.cache/sunstoneos/evidence/` is named;
- **inferred:** a conclusion from observed facts, the manual or a vendor
  source that the binary alone does not prove.

Two documents add labels for their sources. The TF-A monitor document marks
code that has the structure of the TF-A v2.5 release as **upstream**. The GPU
and NPU document marks facts from published driver source as **source**, and
facts read from the GPU firmware container as the open loader reads it as
**container**.

A citation `HASH@X` names a binary by the first eight hexadecimal digits of
its SHA-256 and gives a location `X`:

| Binary | Hash prefix | Load address | Meaning of `X` |
| --- | --- | --- | --- |
| Boot ROM captured on the board | `a8e31821` | `0x0` | ROM address, equal to the file offset |
| Published FIXS-BROM | `3a2a7d28` | `0x0` | ROM address, equal to the file offset |
| Published NS-BROM | `95be69c9` | `0x17000` | ROM address; file offset = address − `0x17000` |
| boot0 | `829ea8e3` | `0x47000` | Load address, as in its inventories; file offset = address − `0x47000` |
| BL31 (`monitor.fex`) | `8af16b86` | `0x48000000` | File offset; address = `0x48000000` + offset |
| SCP firmware (`scp.fex`) | `07e6b976` | `0x40004000` in the E902 view | File offset; E902 address = `0x40004000` + offset |
| GPU firmware | `1db1c399` | Loaded by segment | File offset in the container |
| `libVIPhal.so`, `libNBGlinker.so` | `3ed5357b`, `82f049b0` | Shared objects | File offset, equal to the virtual address |
| `nbinfo` | `5ef6bb4f` | `0x400000` | File offset = virtual address − `0x400000` |

Within one document, a bare `@X` cites the binary that the section describes.

## Peripherals each binary programs

This table combines the register-access inventories of the four binaries
whose code SunstoneOS analyses:
[brom-mmio.json](data/brom-mmio.json), [boot0-mmio.json](data/boot0-mmio.json),
[tf-a-monitor-mmio.json](data/tf-a-monitor-mmio.json) and
[scp-mmio.json](data/scp-mmio.json). **W** means the binary writes at least one
register of the block; **R** means it only reads; a dash means the inventory
lists no access.

| Peripheral | Base | Boot ROM | boot0 | BL31 | SCP |
| --- | --- | :---: | :---: | :---: | :---: |
| GPIO | `0x02000000` | — | W | W | — |
| CCU | `0x02002000` | W | W | W | W |
| NSI interconnect | `0x02020000` | — | W | — | W |
| SPC | `0x02054000` | — | — | W | — |
| UART0 | `0x02500000` | — | — | W | — |
| GPADC0 | `0x02521000` | — | W | — | — |
| SYSCTRL | `0x03000000` | W | W | W | W |
| CPUX message box | `0x03004000` | — | — | W | W |
| SID (eFuse) | `0x03006000` | R | — | W | R |
| TIMER0 | `0x03009000` | — | — | W | — |
| GIC-600 | `0x03400000` | — | — | W | — |
| Window below the NAND controller | `0x04000000` | — | — | R | — |
| DMAC1, DMAC0 | `0x04024000`, `0x04601000` | — | — | W | — |
| USB0, USB1 | `0x04100000`, `0x04200000` | — | — | W | R |
| S_SPC, S_TZMA | `0x07002000`, `0x07003000` | — | — | W | — |
| PRCM | `0x07010000` | W | W | W | W |
| S_WDG | `0x07021000` | W | — | — | W |
| S_TWD | `0x07022000` | W | — | — | — |
| S_INTC | `0x07024000` | — | W | — | W |
| S_GPIO (port L) | `0x07025000` | — | — | — | W |
| E902 configuration | `0x07032000` | — | — | W | W |
| CPUIDLE | `0x07050000` | R | — | W | W |
| PCK-600 PPUs | `0x07060000` | — | — | — | W |
| S_UART0, S_TWI0 | `0x07080000`, `0x07083000` | — | — | — | W |
| RTC | `0x07090000` | W | W | W | W |
| S_TIMER | `0x07091000` | — | — | — | W |
| S_MBOX | `0x07094000` | — | — | W | W |
| CPU_SUBSYS_CTRL | `0x08000000` | — | W | W | W |
| System counter | `0x08010000`, `0x08020000` | W | — | — | W |
| CPU_PLL_CFG | `0x08870000` | W | W | — | W |
| SMC0, SMC1 | `0x0a000000`, `0x0a010000` | — | — | W | W |
| DRAM controllers | `0x0a020000`–`0x0a040000`, `0x0a100000`, `0x0a500000` | — | — | — | W |
| DRAM PHY | `0x0a900000`, PHY memory `0x0aa40000` | — | — | — | W |
| E902 interrupt controller | `0xe0800000`, core-local | — | — | — | W |

The boot ROM and boot0 inventories come from a scan for constant addresses,
which misses an access through a computed base. Their documents describe such
accesses that the table omits: the boot ROM's storage and USB controllers,
and boot0's PMIC bus, SD controller, DRAM controller and PHY. The BL31 and SCP
inventories also list accesses through tables and loops.

The other binaries add no row:

- **GPU firmware.** Its license forbids disassembly, so the registers it
  programs are not analysed. The host-side register writes come from the
  open GPU driver and are described in [gpu-start.md](../gpu-start.md).
- **NPU libraries.** They contain no NPU register address. `libNBGlinker.so`
  reads four identity registers through the driver's register-read command.
  The kernel driver source holds every NPU register sequence for the window
  at `0x03600000`.
- **Radio firmware.** It runs on the radio's own processor and reaches the
  SoC only over SDIO.

None of the four analysed binaries writes a PCK-600 power domain at cold boot;
the vendor Linux drivers power the GPU and NPU domains. The SCP turns every
domain on only when it resumes from system suspend.

## Fetch and verify the binaries

Cache paths are relative to `~/.cache/sunstoneos`, or to `$SUNSTONE_CACHE` when
that variable is set.

```sh
python3 tools/fetch-vendor-firmware.py                 # fetch and verify every downloadable item
python3 tools/fetch-vendor-firmware.py --check         # verify the cache; no network access
python3 tools/fetch-vendor-firmware.py --only monitor  # restrict either mode to named items
```

Each item has one of four acquisition methods:

- **Download:** the tool fetches the pinned URL into a temporary file beside the
  cache path, and renames it into place only when its SHA-256 and size match.
  A mismatch stops the run and leaves the cache unchanged.
- **Derived:** the tool decodes the item from another item and verifies the
  result the same way. Both boot ROM binaries are decoded from published
  hexadecimal dumps.
- **Board:** the item is read from a board. The tool prints the recorded
  procedure, the tool identity and the expected hash.
- **Manual:** the item needs a browser session or a container. The tool prints
  the procedure.

A verified file is skipped, so a repeated run downloads nothing. An existing
file with a different hash is reported and kept; move it aside before fetching
again. `--check` exits with status 1 when any cached item differs or a downloadable
or derived item is missing. It reports a missing board or manual item without
failing.

[checks/vendor-firmware-sources.py](../../checks/vendor-firmware-sources.py)
validates the manifest schema and checks that this page lists every item with
its hash. It also compares each item that `sources.lock.json` also pins and
resolves every documented link without downloading an item. Pass `--offline`
to skip the link step.

## Commit only inventories derived from the binaries

The files listed here stay in the local cache. Do not commit them, the decoded
ROM images, disassembly listings or text extracted from the binaries beyond
short identifiers.

Facts derived from a vendor binary may be committed under
`docs/vendor-firmware/data/`. These are function, register-access, message-ID
and table inventories. Each inventory names the binary's SHA-256 from this
page and gives file offsets, and its text describes behaviour in the author's
own words. A new binary is added to [sources.json](sources.json) and to the
tables below before any inventory cites it.

An image that embeds the GPU firmware must be distributed with
`LICENSE.powervr`, as described under [licenses](#licenses).

## Boot ROM

The A733 has two mask ROMs. The fixed secure boot ROM (FIXS-BROM) at addresses
`0x0` to `0x10fff` runs first after reset and selects the boot device or USB
FEL, the ROM's USB recovery protocol. The nonsecure boot ROM (NS-BROM) at
`0x17000` to `0x21fff` implements FEL.

The [sunxi-blobs repository](https://github.com/smaeul/sunxi-blobs/tree/42aabf4f2c93158a0cd88a000f9af56ce024882a/sun60iw2p1)
publishes dumps of both ROMs at revision
`42aabf4f2c93158a0cd88a000f9af56ce024882a`. Each dump is an `xxd` listing; the
binary is the concatenated hexadecimal columns, without the offset and ASCII
columns.

The secure ROM on the board differs from the published FIXS-BROM: thirteen
words in its first 1 KiB differ, while the reset and redirect instructions
through `0x228` match. Checks of ROM behaviour on the board use the board
capture. The published NS-BROM has not yet been compared with a board capture.

| Item | Role | Pinned source | Bytes | SHA-256 |
| --- | --- | --- | ---: | --- |
| `fixs-brom-hex` | Published hexadecimal dump of the A733 fixed secure boot ROM (FIXS-BROM, addresses 0x0-0x10fff) | [blob.hex](https://raw.githubusercontent.com/smaeul/sunxi-blobs/42aabf4f2c93158a0cd88a000f9af56ce024882a/sun60iw2p1/fixs-brom/blob.hex) | 330,752 | `a7fc3c6ffe6254fb77929eb407edcf88d7925a64b4a968964a0ed79815373fc8` |
| `fixs-brom` | Binary decoded from the published FIXS-BROM dump; differs from the ROM captured on the board | Decoded from `fixs-brom-hex` | 69,632 | `3a2a7d289951f61095afdabe7813b067295223ce588c422d64200fc7e4431eb1` |
| `ns-brom-hex` | Published hexadecimal dump of the A733 nonsecure boot ROM (NS-BROM, addresses 0x17000-0x21fff), which holds the USB FEL implementation | [blob.hex](https://raw.githubusercontent.com/smaeul/sunxi-blobs/42aabf4f2c93158a0cd88a000f9af56ce024882a/sun60iw2p1/ns-brom/blob.hex) | 214,016 | `7bd968d6323c54a457184d78e7cd6e1a851a5cceb1b504558407dd3eff7eaf15` |
| `ns-brom` | Binary decoded from the published NS-BROM dump; not yet compared with a board capture | Decoded from `ns-brom-hex` | 45,056 | `95be69c916846a8259578565f3193a6b9ab0625294d5374f3dc6c2f667fc6169` |
| `board-secure-rom` | Secure boot ROM (addresses 0x0-0x10fff) copied from the board over USB FEL; thirteen words in its first 1 KiB differ from the published FIXS-BROM | Board capture (see [procedure](#reproduce-the-board-rom-capture)) | 69,632 | `a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa` |

### Reproduce the board ROM capture

The board coordinator runs this procedure; workers do not access the board.
It needs a board in FEL mode, the board lock from `tools/usb-access.py`, and
`sunxi-fel` built by `tools/build-sunxi-fel.py` from the `sunxi-tools` entry
of `sources.lock.json` with its four A733 patches. That tool has SHA-256
`4a54cdc179baafbdecac47eb85714c3745d6d9952e05b482c7016afd4b22bf3e`.

A FEL read of address 0 is refused, so the board CPU copies the ROM into SRAM
at `0x52000` with `memmove`. The `memmove` helper uses 128 bytes at `0x71000`.
Both areas are saved first and restored afterwards. `DEV` is the board's USB
bus and device number, such as `3:64`.

```sh
sunxi-fel -d DEV read 0x71000 128 scratch.bin read 0x52000 1024 buffer.bin
sunxi-fel -d DEV memmove 0x52000 0x0 1024 read 0x52000 1024 rom-prefix.bin
sunxi-fel -d DEV write 0x52000 buffer.bin write 0x71000 scratch.bin

sunxi-fel -d DEV read 0x71000 128 scratch2.bin read 0x52000 68608 buffer2.bin
sunxi-fel -d DEV memmove 0x52000 0x400 68608 read 0x52000 68608 rom-remainder.bin
sunxi-fel -d DEV write 0x52000 buffer2.bin write 0x71000 scratch2.bin

cat rom-prefix.bin rom-remainder.bin > physical-rom.bin
sha256sum physical-rom.bin
```

Read both restored areas back and compare them with the saved files. The
result is 69,632 bytes with SHA-256
`a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa`. The
procedure issues no reset. The recorded capture is under
`~/.cache/sunstoneos/evidence/rom-copy-20261016-084055/`, with each FEL
transaction in `session.json` and `remainder-session.json`.

## Boot package from the board build repository

The [orangepi-build repository](https://github.com/orangepi-xunlong/orangepi-build/tree/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin)
at revision `a95129bf30ea11886cc35f08ebb3e69695a26656` supplies the vendor
binaries for the A733 (`sun60iw2`). Its `boot_package.cfg` builds the
second-stage package (TOC1) from three entries:

- `u-boot.fex`, built from the board U-Boot source, which `sources.lock.json`
  pins as `board-uboot`; the repository holds no prebuilt copy;
- `monitor.fex`, the vendor build of Trusted Firmware-A BL31, the EL3 secure
  monitor;
- `scp.fex`, the firmware of the system control processor, an E902 RISC-V core.

boot0 is the first-stage loader that the ROM reads from storage. It sets up
clocks and the PMIC and trains DRAM; the repository has no separate DRAM
initialization binary for the A733. `tools/prepare-ddr.py` runs the DRAM
routines of the pinned Zero 3W boot0 from RAM. SunstoneOS builds its own
microSD boot package with the vendor packer `dragonsecboot`, as described in
[microSD packaging](../microsd-boot-package.md). The vendor monitor, SCP
firmware and the 1800M Zero 3W boot0 are references for CPU, power-domain and
GPU sequences. The four boot0 builds for other boards and media complete the
pinned directory; SunstoneOS does not use them.

| Item | Role | Pinned source | Bytes | SHA-256 |
| --- | --- | --- | ---: | --- |
| `boot0-sdcard-orangepizero3w` | boot0, the first-stage loader the boot ROM reads from storage (Thumb-2, load address 0x47000) for SD boot on the Orange Pi Zero 3W; it trains DRAM, so it is also the only DRAM initialization binary SunstoneOS uses | [boot0_sdcard_orangepizero3w.fex](https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/boot0_sdcard_orangepizero3w.fex) | 245,760 | `829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3` |
| `boot0-sdcard-orangepizero3w-1800m` | boot0, the first-stage loader the boot ROM reads from storage (Thumb-2, load address 0x47000) for SD boot on the Orange Pi Zero 3W, variant named 1800M; it differs from the default variant only in its DRAM parameter tables, DRAM training code and checksum | [boot0_sdcard_orangepizero3w_1800M.fex](https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/boot0_sdcard_orangepizero3w_1800M.fex) | 245,760 | `d2a9bcccf012dce554c4613cf3e2e4256fe9863f6215afbe979aca891d4a6e27` |
| `boot0-sdcard` | boot0, the first-stage loader the boot ROM reads from storage (Thumb-2, load address 0x47000) for SD boot, generic board; not used or analysed by SunstoneOS, recorded to pin the complete vendor directory | [boot0_sdcard.fex](https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/boot0_sdcard.fex) | 245,760 | `a1ed6f1b8c4f5d98c29febefdc7203e5c442c2d1b841d204278b62ad56a70fd2` |
| `boot0-sdcard-a733` | boot0, the first-stage loader the boot ROM reads from storage (Thumb-2, load address 0x47000) for SD boot, A733 reference board; not used or analysed by SunstoneOS, recorded to pin the complete vendor directory | [boot0_sdcard_a733.fex](https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/boot0_sdcard_a733.fex) | 245,760 | `9213f1c56ef932930d43b5751e8937bea0e9ae7460ec16187cc1b0dffdb75345` |
| `boot0-spinor-a733` | boot0, the first-stage loader the boot ROM reads from storage (Thumb-2, load address 0x47000) for SPI NOR boot, A733 reference board; not used or analysed by SunstoneOS, recorded to pin the complete vendor directory | [boot0_spinor_a733.fex](https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/boot0_spinor_a733.fex) | 237,568 | `7f0babc242c1e4614c7243bd2c45a980e4385727ffbb544a7c8f2d324cf8dc07` |
| `boot0-ufs-a733` | boot0, the first-stage loader the boot ROM reads from storage (Thumb-2, load address 0x47000) for UFS boot, A733 reference board; not used or analysed by SunstoneOS, recorded to pin the complete vendor directory | [boot0_ufs_a733.fex](https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/boot0_ufs_a733.fex) | 245,760 | `c5729d90686acfe7db9c649010c8026355fec212e72455e9eda6d17087c0942f` |
| `boot0-ufs-orangepizero3w` | boot0, the first-stage loader the boot ROM reads from storage (Thumb-2, load address 0x47000) for UFS boot on the Orange Pi Zero 3W; not used or analysed by SunstoneOS, recorded to pin the complete vendor directory | [boot0_ufs_orangepizero3w.fex](https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/boot0_ufs_orangepizero3w.fex) | 245,760 | `7046c43d43b3b2e47452ccb6d5db935e109dc9ca7bde7d2ab90105e7901998b3` |
| `monitor` | Vendor TF-A BL31 secure monitor (AArch64, load address 0x48000000), version string v2.5(debug):7dddb263a-dirty, built 30 December 2024; source not published | [monitor.fex](https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/monitor.fex) | 78,609 | `8af16b86ae8e2eff634a3ebca76a8cdf3c10b3540387573d46c3d785545b2632` |
| `scp` | Vendor system control processor (SCP) firmware for the E902 RISC-V core, derived from the arisc (AR100) firmware; runs system suspend, resume, power-off and reset; source not published | [scp.fex](https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/scp.fex) | 105,912 | `07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749` |
| `boot-package-cfg` | Vendor boot-package list: u-boot.fex, monitor.fex and scp.fex in that order, with OP-TEE disabled | [boot_package.cfg](https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/boot_package.cfg) | 209 | `09bd43f68e54c0746b5130a3acf37fd38191d9187e329e2b78711561b6f9d918` |
| `sys-config` | Vendor board configuration read by boot0 and U-Boot (power mode, DRAM and pin settings) | [sys_config.fex](https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/sys_config/sys_config.fex) | 5,006 | `eb598823d947fa39d0cf2102051b921d10f41b26538103b9fa3a266c6a486a29` |
| `u-boot-dts` | Vendor U-Boot device tree source for sun60iw2 (T736 demo board description) | [u-boot-current.dts](https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/dts/u-boot-current.dts) | 239,512 | `bd8e32740bc1c25532f28cae7860c61f07506dfc53a672d9726d2fd7ea71ff0c` |
| `dragonsecboot` | Vendor boot-package packer (x86-64 Linux executable) that builds the TOC1 package from a boot_package.cfg list | [dragonsecboot](https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/tools/dragonsecboot) | 9,105,656 | `e5f9076504a0ccde29c5978bcfc341c1c5ac02d947c47b849a5e8d26433545f6` |

## GPU firmware

The PowerVR firmware runs on the GPU's internal MIPS processor. SunstoneOS
selects it by the board's BVNC identifier, 36.56.104.183, and stages it from
memory, as described in the [GPU record](../gpu.md#matching-firmware-runs-on-an-internal-mips-processor).
Imagination publishes it in its
[linux-firmware repository](https://gitlab.freedesktop.org/imagination/linux-firmware/-/tree/8a58f81883f7be458daa34e418cc4079f995b279)
at revision `8a58f81883f7be458daa34e418cc4079f995b279`. The repository's
`WHENCE` file lists it as version 1.1.OS@6976702 under `LICENSE.powervr`.

The firmware's license forbids reverse engineering, decompilation and
disassembly. Its interfaces come from the open host driver in the
[board kernel](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/gpu/imagination),
whose firmware structures the GPU record pins by file hash. The vendor Linux
build loads a different firmware file, `rgx.fw`, from its driver package;
SunstoneOS does not use it.

| Item | Role | Pinned source | Bytes | SHA-256 |
| --- | --- | --- | ---: | --- |
| `gpu-firmware` | Imagination PowerVR firmware for BVNC 36.56.104.183 (ELF32 MIPS, version 1.1 build 6976702); SunstoneOS loads it into the GPU's internal MIPS processor | [rogue_36.56.104.183_v1.fw](https://gitlab.freedesktop.org/imagination/linux-firmware/-/raw/8a58f81883f7be458daa34e418cc4079f995b279/powervr/rogue_36.56.104.183_v1.fw) | 131,072 | `1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2` |
| `gpu-firmware-license` | LICENSE.powervr, the redistribution terms of the PowerVR firmware | [LICENSE.powervr](https://gitlab.freedesktop.org/imagination/linux-firmware/-/raw/8a58f81883f7be458daa34e418cc4079f995b279/LICENSE.powervr) | 2,190 | `1c9aa6bd6703a7ce1cdb879542fa1d8aca115a327bd819b193c971de9c53f402` |
| `gpu-firmware-whence` | WHENCE index of the Imagination firmware repository; lists the BVNC 36.56.104.183 firmware as version 1.1.OS@6976702 under LICENSE.powervr | [WHENCE](https://gitlab.freedesktop.org/imagination/linux-firmware/-/raw/8a58f81883f7be458daa34e418cc4079f995b279/WHENCE) | 341,319 | `ec86d7649ff9e80e728b129dd725db1728f8a441cf342f1715c0a76dcf8146c3` |

## NPU runtime, models and compiler

The [AI SDK](https://github.com/ZIFENG278/ai-sdk/tree/fc90006d0f6569da2f6726c2d8395877686f5aca)
at revision `fc90006d0f6569da2f6726c2d8395877686f5aca` provides the Linux NPU
runtime libraries, a model inspector and compiled sample models. SunstoneOS
does not load the libraries. Their instructions and debug information
establish the compiled-model format, checksum, relocation and command-ring
encodings that the native loader implements; see the [NPU record](../npu.md).
A second compiled model from
[TIM-VX](https://github.com/VeriSilicon/TIM-VX/tree/702715d714277376d7ff466b7b72833ff57564c1)
provides independent checksum evidence.

The compiler is an x86-64 Docker image published as a shared archive on the
Allwinner file server, linked from the
[Radxa setup instructions](https://docs.radxa.com/en/cubie/a7s/app-dev/npu-dev/cubie-acuity-env).
Its direct link requires a browser sharing session, so the archive is a manual
item. [tools/npu-compiler.lock.json](../../tools/npu-compiler.lock.json) pins
the nested archive, the image and the loaded image identity, and the
[compiler record](../npu-compiler.md) describes its use.

| Item | Role | Pinned source | Bytes | SHA-256 |
| --- | --- | --- | ---: | --- |
| `npu-libviphal` | AI SDK VIPLite 2 hardware-abstraction library (AArch64 Linux shared object); inspected for the ring-descriptor encoder | [libVIPhal.so](https://raw.githubusercontent.com/ZIFENG278/ai-sdk/fc90006d0f6569da2f6726c2d8395877686f5aca/viplite-tina/lib/aarch64-none-linux-gnu/v2.0/libVIPhal.so) | 39,416 | `3ed5357b26bd6c4fb68fbdc0b21d637227dda711d1fe67b9e28accdc48bb11f2` |
| `npu-libnbglinker` | AI SDK VIPLite 2 network loader library (AArch64 Linux shared object); inspected for model loading and address relocation | [libNBGlinker.so](https://raw.githubusercontent.com/ZIFENG278/ai-sdk/fc90006d0f6569da2f6726c2d8395877686f5aca/viplite-tina/lib/aarch64-none-linux-gnu/v2.0/libNBGlinker.so) | 174,728 | `82f049b0ed0065dd4d443e37eeb1edfcbef24457c9c24e36170d64d5b748ca66` |
| `npu-nbinfo` | AI SDK model inspection tool (x86-64 Linux executable with DWARF types); establishes the compiled model container layout | [nbinfo](https://raw.githubusercontent.com/ZIFENG278/ai-sdk/fc90006d0f6569da2f6726c2d8395877686f5aca/tools/nbinfo) | 187,008 | `5ef6bb4f1590e0d1b9603506574bee09321d6159e32a6b3aaef66148085b2639` |
| `npu-sdk-shufflenet-model` | AI SDK compiled ShuffleNetV2 UINT8 model (format 0x1001e); the reference fixture for model inspection, checksum and loading | [network_binary.nb](https://raw.githubusercontent.com/ZIFENG278/ai-sdk/fc90006d0f6569da2f6726c2d8395877686f5aca/examples/vpm_run/operator/v3/network_binary.nb) | 964,224 | `fc0d50bdc863e4dfc74eef6dad9d34655c560e759a2f426979a01c505fa9b95f` |
| `npu-sdk-older-model` | AI SDK compiled model in the older format 0x10014; the checksum verifier refuses it | [network_binary.nb](https://raw.githubusercontent.com/ZIFENG278/ai-sdk/fc90006d0f6569da2f6726c2d8395877686f5aca/examples/vpm_run/operator/v2/network_binary.nb) | 997,120 | `1adb8a4358a1411009c649d0ff42c6c29f11911ce44c1be2c037cc0dcac0858d` |
| `npu-sdk-model-input` | AI SDK sample input tensor for the ShuffleNetV2 model (150,528 bytes) | [input_0.dat](https://raw.githubusercontent.com/ZIFENG278/ai-sdk/fc90006d0f6569da2f6726c2d8395877686f5aca/examples/vpm_run/operator/input_0.dat) | 150,528 | `d38c22c2007db314572c9ddfeb513a9f3d11ec99e8deec0f65bc91481502f1b9` |
| `npu-timvx-model` | TIM-VX compiled convolution, ReLU and max-pool FP32 model (format 0x1001e, target 0xba); independent checksum evidence; the A733 assembler refuses its target | [conv2d_relu_maxpool2d_fp32.nbg](https://raw.githubusercontent.com/VeriSilicon/TIM-VX/702715d714277376d7ff466b7b72833ff57564c1/samples/nbg_runner_pybind/examples/models/conv2d_relu_maxpool2d_fp32.nbg) | 9,152 | `e898680018e90837ec6941c9f0fa5566bde8876ac29a366d9ee53469342cb276` |
| `npu-acuity-compiler` | Allwinner NPU compiler package: Docker image archive with ACUITY 6.30.22 and Vivante IDE 5.11.0 (x86-64 host tool, 2.9 GB) | [Sharing page](https://netstorage.allwinnertech.com:5001/sharing/Mh23BhPHq), manual download | 2,903,273,080 | `d876a96a74905b92d09ae5681256dd5fc6c430b0ebbd612097142473ec87e287` |

## Radio firmware

The AIC8800D80 radio provides Wi-Fi and Bluetooth, and the host loads its
firmware over SDIO. The native loader uploads the Bluetooth data, patch and
patch table, then one Wi-Fi image chosen by the chip revision, as described in
the
[radio firmware record](../radio-firmware.md). The files come from the
[Orange Pi firmware repository](https://github.com/orangepi-xunlong/firmware/tree/dc163b0ed2e0272d9ee31977a0e0a04884f0ae61/aic8800d80)
at revision `dc163b0ed2e0272d9ee31977a0e0a04884f0ae61`. The combined image,
the RF-test image and the user configuration share that directory; the loader
uses none of them.

| Item | Role | Pinned source | Bytes | SHA-256 |
| --- | --- | --- | ---: | --- |
| `radio-bt-adid` | AIC8800D80 Bluetooth ADID data (U02), uploaded to radio address 0x00201940 | [fw_adid_8800d80_u02.bin](https://raw.githubusercontent.com/orangepi-xunlong/firmware/dc163b0ed2e0272d9ee31977a0e0a04884f0ae61/aic8800d80/fw_adid_8800d80_u02.bin) | 1,708 | `a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56` |
| `radio-bt-patch` | AIC8800D80 Bluetooth patch (U02), uploaded to radio address 0x001e0000 | [fw_patch_8800d80_u02.bin](https://raw.githubusercontent.com/orangepi-xunlong/firmware/dc163b0ed2e0272d9ee31977a0e0a04884f0ae61/aic8800d80/fw_patch_8800d80_u02.bin) | 24,744 | `4ad4eede4d347c45ed61b23508e7843dc884f0e411bf957f95410001d0cfc4d0` |
| `radio-bt-patch-table` | AIC8800D80 Bluetooth patch table (U02): seven records of address/value pairs | [fw_patch_table_8800d80_u02.bin](https://raw.githubusercontent.com/orangepi-xunlong/firmware/dc163b0ed2e0272d9ee31977a0e0a04884f0ae61/aic8800d80/fw_patch_table_8800d80_u02.bin) | 23,288 | `5ff184105fdc4c4679e026418f4da293efec8df02a3a3ca22cee723c17df4a60` |
| `radio-wifi` | AIC8800D80 Wi-Fi image for non-H chip revisions (U02), uploaded to radio address 0x00120000 | [fmacfw_8800d80_u02.bin](https://raw.githubusercontent.com/orangepi-xunlong/firmware/dc163b0ed2e0272d9ee31977a0e0a04884f0ae61/aic8800d80/fmacfw_8800d80_u02.bin) | 341,212 | `55b5a20865aca7cd2edaae1733a36da45d2b0b29060bc243d8e9ec212e139f6c` |
| `radio-wifi-h` | AIC8800D80 Wi-Fi image for H chip revisions (U02), uploaded to radio address 0x00120000 | [fmacfw_8800d80_h_u02.bin](https://raw.githubusercontent.com/orangepi-xunlong/firmware/dc163b0ed2e0272d9ee31977a0e0a04884f0ae61/aic8800d80/fmacfw_8800d80_h_u02.bin) | 341,260 | `ae8d548df96c132c445f888b91eab5ef03c19991b8bdef4fc12ebfcbf9f953e7` |
| `radio-wifi-bt-combined` | AIC8800D80 combined Wi-Fi and Bluetooth image (U02); not selected by the normal SDIO mode and not loaded | [fmacfwbt_8800d80_u02.bin](https://raw.githubusercontent.com/orangepi-xunlong/firmware/dc163b0ed2e0272d9ee31977a0e0a04884f0ae61/aic8800d80/fmacfwbt_8800d80_u02.bin) | 341,512 | `efd4bf622e6b7a92bd264c64bcb9002a2e293f8941b7fd891ca4dd6915648cb6` |
| `radio-rf-test` | AIC8800D80 RF-test image (U02); not loaded | [lmacfw_rf_8800d80_u02.bin](https://raw.githubusercontent.com/orangepi-xunlong/firmware/dc163b0ed2e0272d9ee31977a0e0a04884f0ae61/aic8800d80/lmacfw_rf_8800d80_u02.bin) | 254,610 | `4e8b1efb5d9bb299aaf950d6a25e3ca71d42e012d42fd3ec3b49759cc64b38b2` |
| `radio-userconfig` | AIC8800D80 user configuration text (power and calibration overrides); not loaded | [aic_userconfig_8800d80.txt](https://raw.githubusercontent.com/orangepi-xunlong/firmware/dc163b0ed2e0272d9ee31977a0e0a04884f0ae61/aic8800d80/aic_userconfig_8800d80.txt) | 2,683 | `995dbb9e4f2e7b19db838291f52be39af9ab673ad51b270c988216ccff1a720e` |
| `radio-firmware-readme` | README of the Orange Pi firmware overlay | [README.md](https://raw.githubusercontent.com/orangepi-xunlong/firmware/dc163b0ed2e0272d9ee31977a0e0a04884f0ae61/README.md) | 39 | `f743cde35d8c5922c3f966c7d0bbeb937de08ec5e1c4610f6f89301263fa46f2` |

## Cache paths and consumers

| Item | Cache path | Used by |
| --- | --- | --- |
| `fixs-brom-hex` | `sources/brom/fixs-brom-blob.hex` | [sources.md](../../docs/sources.md) |
| `fixs-brom` | `sources/brom/fixs-brom.bin` | [a733-rom-recovery.py](../../checks/a733-rom-recovery.py), [watchdog.md](../../docs/watchdog.md) |
| `ns-brom-hex` | `sources/brom/ns-brom-blob.hex` | [video-platform.md](../../docs/video-platform.md) |
| `ns-brom` | `sources/brom/ns-brom.bin` | [build.py](../../apps/usb-returning/build.py), [a733-video-unit.c](../../checks/a733-video-unit.c), [usb-platform.md](../../docs/usb-platform.md), [usb-session-takeover.md](../../docs/designs/usb-session-takeover.md) |
| `board-secure-rom` | `evidence/rom-copy-20261016-084055/physical-rom.bin` | [a733-rom-recovery.py](../../checks/a733-rom-recovery.py), [remote-writers-sources.py](../../checks/remote-writers-sources.py), [watchdog-rom-reset-log.py](../../checks/watchdog-rom-reset-log.py), [check-a733-smp.py](../../tools/check-a733-smp.py), [check-fel-input.py](../../tools/check-fel-input.py), [check-watchdog-configuration.py](../../tools/check-watchdog-configuration.py), [check-watchdog-expiry.py](../../tools/check-watchdog-expiry.py), [watchdog.md](../../docs/watchdog.md), [usb-platform.md](../../docs/usb-platform.md), [video-platform.md](../../docs/video-platform.md), [README.md](../../apps/usb-returning/README.md) |
| `boot0-sdcard-orangepizero3w` | `sources/board-boot/boot0_sdcard_orangepizero3w.fex` | [prepare-microsd-boot.py](../../tools/prepare-microsd-boot.py), [prepare-ddr.py](../../tools/prepare-ddr.py), [cold-entry.py](../../checks/cold-entry.py), [boot0-monitor-handoff.py](../../checks/boot0-monitor-handoff.py), [remote-writers-sources.py](../../checks/remote-writers-sources.py), [cold-boot-cache.py](../../checks/cold-boot-cache.py), [boot.md](../../docs/boot.md), [microsd-boot-package.md](../../docs/microsd-boot-package.md), [cold-entry.md](../../docs/cold-entry.md) |
| `boot0-sdcard-orangepizero3w-1800m` | `sources/board-boot/boot0_sdcard_orangepizero3w_1800M.fex` | No repository consumer; reference analysis only |
| `boot0-sdcard` | `sources/board-boot/boot0_sdcard.fex` | No repository consumer; reference analysis only |
| `boot0-sdcard-a733` | `sources/board-boot/boot0_sdcard_a733.fex` | No repository consumer; reference analysis only |
| `boot0-spinor-a733` | `sources/board-boot/boot0_spinor_a733.fex` | No repository consumer; reference analysis only |
| `boot0-ufs-a733` | `sources/board-boot/boot0_ufs_a733.fex` | No repository consumer; reference analysis only |
| `boot0-ufs-orangepizero3w` | `sources/board-boot/boot0_ufs_orangepizero3w.fex` | No repository consumer; reference analysis only |
| `monitor` | `sources/cpu-startup/monitor.fex` | [a733-cpu-start.py](../../checks/a733-cpu-start.py), [cpu-start.md](../../docs/cpu-start.md), [a733-smp-supervised.md](../../docs/a733-smp-supervised.md), [cpu-pmu.md](../../docs/designs/cpu-pmu.md) |
| `scp` | `sources/board-boot/scp.fex` | [gpu-power.md](../../docs/gpu-power.md) |
| `boot-package-cfg` | `sources/board-boot/boot_package.cfg` | [microsd-boot-package.md](../../docs/microsd-boot-package.md) |
| `sys-config` | `sources/board-boot/sys_config-sys_config.fex` | [admission.c](../../boot/a733/stage2/admission.c), [boot-slots.md](../../docs/boot-slots.md) |
| `u-boot-dts` | `sources/gpu-backsolve-20261025/orangepi-build-a95129bf/dts/u-boot-current.dts` | No repository consumer; reference analysis only |
| `dragonsecboot` | `sources/board-boot/dragonsecboot` | [prepare-microsd-boot.py](../../tools/prepare-microsd-boot.py), [microsd-boot-package.md](../../docs/microsd-boot-package.md), [sources.md](../../docs/sources.md) |
| `gpu-firmware` | `sources/gpu/rogue_36.56.104.183_v1.fw` | [gpu.cmake](../../cmake/gpu.cmake), [a733-os.cmake](../../cmake/a733-os.cmake), [pvr-firmware-reference.py](../../checks/pvr-firmware-reference.py), [gpu.md](../../docs/gpu.md), [gpu-service.md](../../docs/gpu-service.md), [gpu-application.md](../../docs/gpu-application.md) |
| `gpu-firmware-license` | `sources/gpu/LICENSE.powervr` | [gpu.md](../../docs/gpu.md), [gpu-display-preparation.md](../../docs/gpu-display-preparation.md) |
| `gpu-firmware-whence` | `sources/gpu/imagination-firmware/WHENCE` | [gpu-service.md](../../docs/gpu-service.md) |
| `npu-libviphal` | `sources/npu/ai-sdk-fc90006d0f6569da2f6726c2d8395877686f5aca/viplite-tina/lib/aarch64-none-linux-gnu/v2.0/libVIPhal.so` | [nbg-ring-reference.py](../../checks/nbg-ring-reference.py), [nbg-rings.md](../../docs/nbg-rings.md) |
| `npu-libnbglinker` | `sources/npu/ai-sdk-fc90006d0f6569da2f6726c2d8395877686f5aca/viplite-tina/lib/aarch64-none-linux-gnu/v2.0/libNBGlinker.so` | [nbg-loader-reference.py](../../checks/nbg-loader-reference.py), [nbg-relocation-reference.py](../../checks/nbg-relocation-reference.py), [nbg-relocation.md](../../docs/nbg-relocation.md), [nbg-conv-loader.md](../../docs/nbg-conv-loader.md) |
| `npu-nbinfo` | `sources/npu/ai-sdk-fc90006d0f6569da2f6726c2d8395877686f5aca/tools/nbinfo` | [nbg-directory-reference.py](../../checks/nbg-directory-reference.py), [nbg-directory.md](../../docs/nbg-directory.md), [npu-conformance-references.json](../../tools/npu-conformance-references.json) |
| `npu-sdk-shufflenet-model` | `sources/npu/ai-sdk-fc90006d0f6569da2f6726c2d8395877686f5aca/examples/vpm_run/operator/v3/network_binary.nb` | [nbg-inspection.py](../../checks/nbg-inspection.py), [nbg-loader-reference.py](../../checks/nbg-loader-reference.py), [npu.md](../../docs/npu.md), [nbg-checksum.md](../../docs/nbg-checksum.md), [npu-models.json](../../tools/npu-models.json) |
| `npu-sdk-older-model` | `sources/npu/ai-sdk-fc90006d0f6569da2f6726c2d8395877686f5aca/examples/vpm_run/operator/v2/network_binary.nb` | [nbg-checksum.md](../../docs/nbg-checksum.md) |
| `npu-sdk-model-input` | `sources/npu/ai-sdk-fc90006d0f6569da2f6726c2d8395877686f5aca/examples/vpm_run/operator/input_0.dat` | [npu-conv-input.py](../../tools/npu-conv-input.py), [npu-models.json](../../tools/npu-models.json), [npu-complete-nn.md](../../docs/npu-complete-nn.md) |
| `npu-timvx-model` | `sources/npu/tim-vx-702715d714277376d7ff466b7b72833ff57564c1/samples/nbg_runner_pybind/examples/models/conv2d_relu_maxpool2d_fp32.nbg` | [nbg-checksum-reference.py](../../checks/nbg-checksum-reference.py), [nbg-checksum.md](../../docs/nbg-checksum.md) |
| `npu-acuity-compiler` | `sources/npu/acuity-download/docker_images_v2.0.x.zip` | [npu-compiler.lock.json](../../tools/npu-compiler.lock.json), [npu-compiler.md](../../docs/npu-compiler.md) |
| `radio-bt-adid` | `sources/radio/firmware/aic8800d80/fw_adid_8800d80_u02.bin` | [aic8800_firmware.c](../../drivers/net/aic8800_firmware.c), [aic8800-firmware.py](../../checks/aic8800-firmware.py), [board-single-core-host.py](../../checks/board-single-core-host.py), [radio-bootstrap-budget.py](../../checks/radio-bootstrap-budget.py), [radio-firmware.md](../../docs/radio-firmware.md) |
| `radio-bt-patch` | `sources/radio/firmware/aic8800d80/fw_patch_8800d80_u02.bin` | [aic8800_firmware.c](../../drivers/net/aic8800_firmware.c), [aic8800-firmware.py](../../checks/aic8800-firmware.py), [board-single-core-host.py](../../checks/board-single-core-host.py), [radio-bootstrap-budget.py](../../checks/radio-bootstrap-budget.py), [radio-firmware.md](../../docs/radio-firmware.md) |
| `radio-bt-patch-table` | `sources/radio/firmware/aic8800d80/fw_patch_table_8800d80_u02.bin` | [aic8800_firmware.c](../../drivers/net/aic8800_firmware.c), [aic8800-firmware.py](../../checks/aic8800-firmware.py), [board-single-core-host.py](../../checks/board-single-core-host.py), [radio-bootstrap-budget.py](../../checks/radio-bootstrap-budget.py), [radio-firmware.md](../../docs/radio-firmware.md) |
| `radio-wifi` | `sources/radio/firmware/aic8800d80/fmacfw_8800d80_u02.bin` | [aic8800_firmware.c](../../drivers/net/aic8800_firmware.c), [aic8800-firmware.py](../../checks/aic8800-firmware.py), [radio-bootstrap-budget.py](../../checks/radio-bootstrap-budget.py), [radio-firmware.md](../../docs/radio-firmware.md) |
| `radio-wifi-h` | `sources/radio/firmware/aic8800d80/fmacfw_8800d80_h_u02.bin` | [aic8800_firmware.c](../../drivers/net/aic8800_firmware.c), [aic8800-firmware.py](../../checks/aic8800-firmware.py), [board-single-core-host.py](../../checks/board-single-core-host.py), [radio-bootstrap-budget.py](../../checks/radio-bootstrap-budget.py), [radio-firmware.md](../../docs/radio-firmware.md) |
| `radio-wifi-bt-combined` | `sources/radio/firmware/aic8800d80/fmacfwbt_8800d80_u02.bin` | [radio.md](../../docs/radio.md) |
| `radio-rf-test` | `sources/radio/firmware/aic8800d80/lmacfw_rf_8800d80_u02.bin` | [radio.md](../../docs/radio.md) |
| `radio-userconfig` | `sources/radio/firmware/aic8800d80/aic_userconfig_8800d80.txt` | [radio.md](../../docs/radio.md) |
| `radio-firmware-readme` | `sources/radio/firmware/README.md` | [radio.md](../../docs/radio.md) |

## Licenses

Two vendor licenses restrict how SunstoneOS may use these files:

- **GPU firmware.** [LICENSE.powervr](https://gitlab.freedesktop.org/imagination/linux-firmware/-/raw/8a58f81883f7be458daa34e418cc4079f995b279/LICENSE.powervr),
  pinned as `gpu-firmware-license`, permits redistribution of the firmware in
  unmodified binary form. A distribution must reproduce the copyright notice
  and disclaimer. The license forbids reverse engineering, decompilation and
  disassembly (line 14), so the firmware's code is not analysed. An image that
  embeds the GPU firmware is distributed with `LICENSE.powervr`.
- **NPU compiler.** The Vivante End User License Agreement inside the compiler
  image grants a license to possess and use the software, and forbids its
  disassembly, decompilation and reverse engineering. SunstoneOS runs the
  compiler to produce models and takes model-format facts from the compiled
  files, the AI SDK inspector's debug information and the MIT-licensed TIM-VX
  sources. [tools/npu-compiler.lock.json](../../tools/npu-compiler.lock.json)
  lists the license paths inside the image.

The TIM-VX model `npu-timvx-model` is covered by the
[TIM-VX repository's MIT license](https://github.com/VeriSilicon/TIM-VX/blob/702715d714277376d7ff466b7b72833ff57564c1/LICENSE).

## Sources pinned elsewhere

[sources.lock.json](../../sources.lock.json) pins source archives, single source
files and manuals, and `tools/fetch-sources.py` fetches them. This covers the
board kernel, U-Boot, Trusted Firmware-A, the FEL tools, the A733 user manual
and datasheet, the board schematic and manual, and the Arm reference manuals.
Component manifests such as `apps/rtc/sources.json` and
`docs/typec-pd-sources.json` pin the source files that one component adapts.
[docs/sources.md](../sources.md) describes how each source is used.
