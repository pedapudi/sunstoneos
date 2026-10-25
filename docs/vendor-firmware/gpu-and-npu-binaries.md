# A733 GPU firmware and NPU vendor binaries

This reference describes the GPU firmware and the NPU vendor libraries that
SunstoneOS depends on for the Allwinner A733: their container formats, the
host interfaces their open drivers and headers define, and the behaviour a
FreeRTOS driver must reproduce or match. The GPU firmware's own code is not
analysed, because its license forbids it; see
[the license note](#the-gpu-firmware-itself-is-not-analysed). The
[vendor firmware overview](README.md) pins each binary.

Status: **reference analysis**. The facts come from the open driver sources,
the firmware container headers, the NPU libraries and the SunstoneOS sources. No
GPU firmware has executed under SunstoneOS. The NPU has executed jobs once,
with wrong output ([NPU](../npu.md)).

The reader is assumed to know device drivers, ring buffers, page tables and
ELF files. Existing SunstoneOS documents describe each implemented component;
this document links to them instead of repeating them.

## The GPU firmware itself is not analysed

`LICENSE.powervr` line 14 states that no reverse engineering, decompilation
or disassembly of the GPU firmware is permitted. This document therefore does
not describe the firmware's code, its internal functions, the hardware
registers it programs, or its power, recovery or performance-counter
behaviour. The GPU facts here come only from:

- the open-source DDK host driver and its firmware-interface headers, cited
  by file and line (the board kernel's `bsp/drivers/gpu/imagination/`, which
  carries an MIT or GPL-2.0 notice per file);
- parsing the firmware container as the open driver's loader does,
  which reads the ELF program headers, the information footer and the
  device-information counts (`pvr_fw.c:101-183`, `pvr_fw_info.h`);
- SunstoneOS's own board evidence.


## Citations, labels and machine-readable inventories

Citations and labels follow the
[overview's conventions](README.md#evidence-labels-and-citations). Each fact
carries a citation and one of these labels:

- **source**: matches a named published source file, cited as file and line
  or symbol.
- **container**: read from the firmware container as the open loader reads
  it; the citation is the SHA-256 prefix and file offset (`1db1c399@0x1f000`).
- **observed** (NPU only): read from an NPU binary, cited as `HASH@offset`.
- **confirmed on a board**: a read on an A733 board matched, citing the
  evidence directory.
- **inferred**: a conclusion that the cited sources alone do not prove.

The GPU and NPU backsolve documents linked here are in preparation on their
own branches; their links resolve once they merge.

The complete inventories are JSON files outside Git, under
`~/.cache/sunstoneos/evidence/vendor-firmware-20261025/accel/`.

| File | Contents |
| --- | --- |
| `gpu-container/gpu-firmware-container.json` | The firmware container as the open loader parses it: ELF program headers, information footer, layout entries and device-information counts |
| `npu/npu-binaries.json` | Every cached SDK binary with hashes, imports and exports; the uncached SDK tree by family; the kernel driver's source files |
| `npu/npu-ioctl.json` | The 30 driver commands with payload structures, field offsets, kernel handlers and library call sites |
| `npu/npu-feature-database.json` | The four rows of the VIPLite feature database, with the A733 row interpreted |
| `npu/npu-constants.json` | Labelled constants: device node, identity values, register offsets, command opcodes and format constants |

Curated copies are tracked in the repository under [data/](data/). Each
starts with the binary's name, SHA-256 and source reference, and keeps only
short identifiers:

| Tracked file | Contents |
| --- | --- |
| [gpu-firmware-container.json](data/gpu-firmware-container.json) | The container parse: ELF program headers, information footer, layout entries and device-information counts |
| [npu-binaries.json](data/npu-binaries.json) | NPU binaries, exported and imported symbols, and the SDK tree inventory |
| [npu-ioctl.json](data/npu-ioctl.json) | Driver commands, payload layouts and handlers |
| [npu-feature-database.json](data/npu-feature-database.json) | The per-chip feature table |
| [npu-constants.json](data/npu-constants.json) | Register offsets, command opcodes and format constants |

[The vendor firmware overview](README.md) records where each binary can be
fetched, its revision and its SHA-256.

Neither the inventories nor this document contain firmware disassembly or
firmware bytes.

## Binary identities

| Binary | SHA-256 | Bytes | Provenance |
| --- | --- | ---: | --- |
| GPU firmware `rogue_36.56.104.183_v1.fw` | `1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2` | 131,072 | Imagination `linux-firmware` revision `8a58f818`, pinned in [sources.lock.json](../../sources.lock.json) as `gpu-firmware` |
| NPU hardware-abstraction library `libVIPhal.so` (VIPLite 2, AArch64) | `3ed5357b26bd6c4fb68fbdc0b21d637227dda711d1fe67b9e28accdc48bb11f2` | 39,416 | AI SDK `ZIFENG278/ai-sdk` revision `fc90006d`, `viplite-tina/lib/aarch64-none-linux-gnu/v2.0/` |
| NPU network loader `libNBGlinker.so` (VIPLite 2, AArch64) | `82f049b0ed0065dd4d443e37eeb1edfcbef24457c9c24e36170d64d5b748ca66` | 174,728 | Same directory |
| NPU model inspector `nbinfo` (x86-64 with DWARF) | `5ef6bb4f1590e0d1b9603506574bee09321d6159e32a6b3aaef66148085b2639` | 187,008 | AI SDK `tools/` |
| SDK sample model for the A733, `v3/network_binary.nb` | `fc0d50bdc863e4dfc74eef6dad9d34655c560e759a2f426979a01c505fa9b95f` | 964,224 | AI SDK `examples/vpm_run/operator/v3/` |
| VIPLite 2 kernel driver | Source, per-file hashes in `npu-binaries.json` | — | `orangepi-xunlong/linux-orangepi` `2ac08e8c`, `bsp/drivers/npu/aw_nna_vip/vip2/` |

The two `variant.fw` files under
`~/.cache/sunstoneos/evidence/gpu-start-20261016/` and `gpu-stop-20261016/`
are SunstoneOS test fixtures. The repository keeps every firmware binary
outside Git.

## GPU firmware

The firmware is a binary for the GPU's internal processor. What follows is
the container the host loads, the host-facing interface the open driver and
its headers define, and what SunstoneOS and board evidence establish. For the
firmware's behaviour on the GPU, the open DDK driver source is the authority.

### The container is a MIPS ELF with two trailing information blocks

The open loader reads the file as an ELF32 executable followed by two 4 KiB
blocks at the end (`pvr_fw.c:101-183`; structures in `pvr_fw_info.h`). The
container parse is in [gpu-firmware-container.json](data/gpu-firmware-container.json).

| Part | File offsets | Contents | Label |
| --- | --- | --- | --- |
| ELF32 executable | `0x0`–`0x1d083` | Little-endian MIPS executable; machine `EM_MIPS`; flags `0x72001001`; entry `0xbfc00001` | container `1db1c399@0x0` |
| Device-information block | `0x1e000`–`0x1efff` | Four 64-bit counts (1, 1, 2, 29), then one errata word, one enhancement word, two feature words and 29 feature parameters | container `1db1c399@0x1e000`; layout `pvr_fw_info.h` `pvr_fw_device_info_header` |
| Information footer | `0x1f000`–`0x1ffff` | 48-byte header and six 24-byte layout entries | container `1db1c399@0x1f000`; `pvr_fw_info.h` `pvr_fw_info_header` |

The information header (container `1db1c399@0x1f000`):

| Field | Value | Meaning |
| --- | --- | --- |
| Information version | 3 | The only version the loader accepts (`pvr_fw.c:106`) |
| BVNC | 36.56.104.183 | GPU revision the image supports |
| Firmware page size | 4,096 | MIPS page size |
| Flags | `0x1` | Open-source compatible; the loader refuses images without this bit (`pvr_fw.c:119`) |
| Firmware version | 1.1, build 6976702 | Firmware ABI major 1 |
| Device-information size | 4,096 | Size of the preceding block |

The loader copies five program segments into two host allocations, a code
allocation and a data allocation. The six layout entries name each segment's
identifier, allocation, size and offset.

| Segment (program header) | Virtual address | File offset | Bytes |
| --- | --- | --- | ---: |
| 0 (`MIPS_EXCEPTIONS_CODE`) | `0x9fc02000` | `0xd4` | 3,088 |
| 1 (`MIPS_BOOT_CODE`) | `0xbfc00000` | `0xce4` | 1,748 |
| 2 (`MIPS_CODE`) | `0xc0000000` | `0x13b8` | 93,916 |
| 3 (`MIPS_PRIVATE_DATA`, `.bss`) | `0xc0032000` | `0x18298` | 0 (8 in memory) |
| 4 (`MIPS_PRIVATE_DATA`) | `0xc0032008` | `0x182c8` | 10,712 |

The six layout entries, their reserved sizes and the resulting host staging
are described in [gpu.md](../gpu.md#matching-firmware-runs-on-an-internal-mips-processor)
and implemented in [pvr_firmware.c](../../drivers/gpu/pvr_firmware.c) and
[pvr_mips.c](../../drivers/gpu/pvr_mips.c). SunstoneOS already checks every
layout field against the open driver's own staging
([gpu-mips.md](../gpu-mips.md)).

### The firmware processor is the GPU's internal MIPS core

The device-information feature mask sets the MIPS feature and not META or
RISC-V (container `1db1c399@0x1e030`, decoded through the open enumeration
`pvr_rogue_fwif_dev_info.h`). The DDK core header for this revision declares
`RGX_FEATURE_MIPS` (`rgxcore_km_36.56.104.183.h`). The open MIPS support code
targets a microAptiv core (`pvr_rogue_mips.h`, `pvr_fw_mips.c`,
`pvr_vm_mips.c`). SunstoneOS builds the firmware's MIPS page tables and boot
configuration from those sources; [gpu-mips.md](../gpu-mips.md) records the
addresses, the page-table format and the wrapper register plan, all taken
from the driver source and checked against it.

### The host-facing interface comes from the open driver headers

The host and firmware share three fixed records and a set of queues. The open
driver defines their layout and offsets; SunstoneOS prepares them and checks
them byte for byte against the open initialisers.

| Object | Defined in | SunstoneOS |
| --- | --- | --- |
| Connection control, OS initialisation, system initialisation records at the top of the firmware heap | `pvr_fw.c:29-42`, `pvr_rogue_fwif.h` | [gpu-fw-state.md](../gpu-fw-state.md), [pvr_fw_state.c](../../drivers/gpu/pvr_fw_state.c) |
| Host-command queue (KCCB), return statuses, firmware-event queue (FWCCB) | `pvr_ccb.c`, `pvr_rogue_fwif.h` | [gpu-queues.md](../gpu-queues.md), [pvr_ccb.c](../../drivers/gpu/pvr_ccb.c) |
| Client command queues (CCCB) | `pvr_cccb.c` | [gpu-cccb.md](../gpu-cccb.md), [pvr_cccb.c](../../drivers/gpu/pvr_cccb.c) |
| 64-byte firmware compatibility record in the OS record | `pvr_rogue_fwif.h` `rogue_fwif_osinit` | [gpu-control.md](../gpu-control.md) |

Command and event identifiers are constants in `pvr_rogue_fwif.h`. The upper
half of each command word carries the magic `0x2abc`. The identifiers that
SunstoneOS emits, and the source that defines each, are:

| ID | Name (`pvr_rogue_fwif.h`) | SunstoneOS |
| ---: | --- | --- |
| 101 | `KCCB_CMD_KICK` | [pvr_cccb.c](../../drivers/gpu/pvr_cccb.c) |
| 102 | `KCCB_CMD_MMUCACHE` | [pvr_gpu_flush.c](../../drivers/gpu/pvr_gpu_flush.c) |
| 106 | `KCCB_CMD_CLEANUP` | [pvr_gpu_service.c](../../drivers/gpu/pvr_gpu_service.c) |
| 107 | `KCCB_CMD_POW` | [pvr_gpu_stop.c](../../drivers/gpu/pvr_gpu_stop.c) |
| 110, 112 | `KCCB_CMD_FREELIST_GROW_UPDATE`, `..._FREELISTS_RECONSTRUCTION_UPDATE` | [pvr_free_list.c](../../drivers/gpu/pvr_free_list.c) |
| 115 | `KCCB_CMD_HEALTH_CHECK` | [pvr_gpu_control.c](../../drivers/gpu/pvr_gpu_control.c) |
| 117 | `KCCB_CMD_COMBINED_GEOM_FRAG_KICK` | [pvr_cccb.c](../../drivers/gpu/pvr_cccb.c) |
| 201, 203, 204, 205 (task bit `0x8000`) | client `GEOM`, `FRAG`, `FRAG_PR`, `CDM` | [pvr_render_service.c](../../drivers/gpu/pvr_render_service.c), [pvr_compute_job.c](../../drivers/gpu/pvr_compute_job.c) |
| 213 | client `UPDATE` | [pvr_mimus_service.c](../../drivers/gpu/pvr_mimus_service.c) |

The firmware-to-host events that SunstoneOS decodes, 101 to 108 plus the
restart and page-fault notifications 109 and 112, are defined in
`pvr_rogue_fwif.h:1371-1398`; their handling is in
[pvr_gpu_events.c](../../drivers/gpu/pvr_gpu_events.c) and
[gpu-recovery.md](../gpu-recovery.md). The power command's request subtypes
(off, forced idle, number-of-units change, active-power-management latency)
are `enum rogue_fwif_power_type`; the firmware power states that the stop
sequence waits for are `enum rogue_fwif_pow_state` (`pvr_rogue_fwif.h:105-110`),
which [gpu-isolation.md](../gpu-isolation.md) uses.

Host-to-firmware signalling writes the GPU's scheduler register, and
firmware-to-host signalling uses the MIPS wrapper interrupt registers. The
open driver names these (`pvr_fw_mips.c:176-186`); SunstoneOS uses them in
[gpu-interrupts.md](../gpu-interrupts.md) and
[gpu-control.md](../gpu-control.md).

### The host configures memory, wrapper and reset before start

The open driver's start sequence is `pvr_fw_startstop.c` and `rgxstartstop.c`.
SunstoneOS reproduces and checks it in [gpu-start.md](../gpu-start.md),
[pvr_gpu_start.c](../../drivers/gpu/pvr_gpu_start.c) and
[pvr_mips.c](../../drivers/gpu/pvr_mips.c). In summary, the host first
publishes the firmware heap, the fixed records and the queues. It loads the
five segments at their layout offsets and builds the firmware page table and
boot configuration. It writes the MIPS wrapper registers (boot mode, remaps,
address-zero workaround), configures the AXI-ACE interface and system-bus
security, and holds then releases the reset. Every value is from the driver
source or its headers.

### Power: the host owns the GPU core, the firmware runs on it

The host owns the SoC-level power for the GPU: the PCK-600 power domains, the
GPU PLL and clock gates, the reset and the DCDC4 supply. The vendor kernel's
GPU platform layer writes the GPU-core domain policy itself and does not wait
for its status; this is derived from the vendor kernel source and board
evidence in the [GPU backsolve](../reference-backsolve-gpu.md) and
[gpu-power.md](../gpu-power.md).
SunstoneOS's power owner is [a733_gpu_power.c](../../drivers/gpu/a733_gpu_power.c).
Idle and power-off requests pass to the firmware through power command 107 and
are acknowledged in the shared power-sync word and the firmware power state,
as the open stop sequence does ([gpu-stop.md](../gpu-stop.md),
[pvr_gpu_stop.c](../../drivers/gpu/pvr_gpu_stop.c)). How the firmware carries
out a power transition on the GPU is not analysed.

### Version and BVNC binding

The information header binds the image to BVNC 36.56.104.183 (container
`1db1c399@0x1f000`). The board's vendor DDK build selects the same BVNC
(`bxm-2ac08e8c…/linux/rogue_km/build/linux/sunxi_linux/Makefile`). The
device-information block's errata, enhancement and feature masks decode,
through the open enumeration, to:

| Kind | Firmware container | Vendor DDK core header for 36.56.104.183 | Label |
| --- | --- | --- | --- |
| Errata (BRN) | 44079, 63553 | 63553, 71317 | container `1db1c399@0x1e020`; source `rgxcore_km_36.56.104.183.h` |
| Enhancements (ERN) | 35421, 38748, 42290, 42606, 47025, 57596 | 42290, 42606, 47025, 57596 | container `@0x1e028`; source |
| Features | 51 present, 20 absent | Every DDK feature with a container bit agrees in presence and value | container `@0x1e030`; source `rgxconfig_km_36.V.104.183.h` |

Selected feature values (container `1db1c399@0x1e040`, decoded through the
open driver's feature enumeration):

| Feature | Value |
| --- | --- |
| Firmware processor | MIPS (META and RISC-V absent) |
| Clusters, raster pipes, ISP pipes | 1, 1, 6 |
| Physical and virtual address bits | 36, 40 |
| System-level cache | 16 KiB, one bank, 512-bit lines, size configurable |
| OS IDs and virtualisation | 8, present |
| Multicore support | present |
| Compute, tessellation | present, absent |
| Tile size | 16 × 16 |
| Firmware watchdog timer, dynamic dust power | absent, absent |

The open firmware's errata list differs from the DDK core header's: the
container omits BRN 71317 (for which the open enumeration has no bit) and adds
BRN 44079. SunstoneOS acts only on BRN 63553, the address-zero remap
([gpu-mips.md](../gpu-mips.md)). The vendor Linux build loads a different,
unpublished binary, the DDK's own `rgx.fw` (`config_kernel.h:2`); SunstoneOS
uses this open firmware with the upstream-style driver ABI. No SunstoneOS
evidence compares the two firmwares.

## NPU binaries

### The SDK ships user libraries and models, with no kernel module or firmware

The A733 NPU is a Vivante VIP core driven by the VIPLite 2 stack. The AI SDK
contains user-space libraries, a model inspector and compiled models. Its
tree, which the cache records completely (`ai-sdk-tree.json`, not
truncated), contains no kernel module, no firmware file and no microcode
(observed). The kernel driver exists only as source in the board kernel and
builds as the `vipcore` module. It never requests firmware (source).

| Binary or family | Role | Label |
| --- | --- | --- |
| `libVIPhal.so` | Hardware-abstraction library. Exports 70 `viphal_*` functions: 29 wrappers that each issue one driver command, file, mutex, print and memory helpers, a user-space heap, a feature query and three command generators | observed `3ed5357b` dynamic symbols |
| `libNBGlinker.so` | Network loader. Exports 40 `vip_*` functions: network creation and preparation, buffers, groups, queries, power control and PPU parameters. Parses and checks models, relocates their commands and submits them through `libVIPhal` | observed `82f049b0` |
| `nbinfo` | x86-64 model inspector with debug information. Contains its own copy of the model parser and no driver call | observed `5ef6bb4f` |
| VIPLite 1.13 `libVIPlite.so`, `libVIPuser.so` | Older single-library runtime and its driver shim, for four toolchains with debug variants | tree listing; role inferred |
| VIPLite 2 debug variants | Same libraries built with bandwidth, CNN-bandwidth and debug options; the release `libVIPhal.so` is byte-identical to the bandwidth variant | tree listing (Git blob identities) |
| `unified-tina` | OpenVX runtime (`libOpenVX`, `libovxlib`), the `galcore` user library `libGAL`, shader compilers (`libVSC`, `libCLC`, `libGLSLC`), performance models and prebuilt shader-kernel libraries of 5–65 MB | tree listing; roles inferred |
| Models | Twelve `.nb` files for LeNet, ResNet-50, YOLOv5, YOLACT and the ShuffleNet operator sample, most in v2 and v3 variants | tree listing |

Neither cached library embeds a firmware, microcode or shader blob. Their
dense regions are code, unwind tables and debug data; `libNBGlinker.so`
holds one 104-byte numeric table at `0x1fd48` (observed). The SDK's v2
sample model targets another chip: its header names format `0x10014` and
product `0x10000016`, whereas the v3 sample names `0x1001e` and the A733
product `0x1000003b` (observed, header bytes 4–11 of each file).

### User libraries reach the hardware only through one ioctl

`libVIPhal.so` opens `/dev/vipcore` (string at `3ed5357b@0x7120`) and makes
one `ioctl` call, in `viphal_os_call_drv` at `0x5c7c`, and no other. The request
number is the plain integer 30000 (source `vip2/os/linux/vip_drv_os.h:59`).
Its argument is a 24-byte record: a driver command number at byte 0, an
error code at 4, the payload size at 8 and a user pointer at 16. The library
takes the payload size from a 30-entry table at `0x7358` (observed). The
library imports no `mmap`; the driver has no mmap entry point and creates
user mappings inside the allocation commands (source
`vip_drv_device_driver.c:342-362`). The device node has mode 0666.

| Command | Name | Payload bytes | Sent by `libVIPhal` |
| ---: | --- | ---: | --- |
| 0, 1 | Initialise, destroy | 8, 0 | yes |
| 2, 3 | Read register, write register | 16 | yes; the kernel applies no offset or value check (source `vip_drv_hardware.c:948-1044`) |
| 4, 5 | Wait for task, submit task | 8, 36 | yes |
| 6, 7 | Allocate, free device memory | 56, 4 | yes |
| 8, 14 | Unused | — | no |
| 9 | Query address information | 16 | no; compiled out |
| 10 | Query feature database | 392 | yes |
| 11 | CPU cache operation | 8 | yes |
| 12, 13 | Wrap, unwrap user memory | 40, 4 | yes |
| 15 | Power management | 12 | yes |
| 16, 17 | Wrap, unwrap user physical memory | 6,192, 4 | yes |
| 18, 19 | Wrap, unwrap a buffer file descriptor | 48, 4 | yes |
| 20, 21, 22 | Query profiling, driver status, MMU information | 204, 20, 288 | yes |
| 23, 24 | Map, unmap user logical address | 16, 4 | yes |
| 25 | Cancel task | 8 | yes |
| 26 | Query register dump | 16 | yes |
| 27, 28, 29 | Create task, set task property, destroy task | 24, 36, 4 | yes |

Every payload size in the library equals the size of the matching kernel
structure compiled for AArch64. That match implies the library was built
with preemption, physical-memory reservation and the system heap enabled,
32-bit sizes and at most eight cores (inferred). [npu-ioctl.json](data/npu-ioctl.json)
gives each payload's field offsets, handler line and library call site.

`libNBGlinker.so` imports 54 of the `viphal_*` functions and no system call
that reaches the driver (observed). It does import `viphal_read_reg` and
reads identity registers `+0x20`, `+0x24`, `+0x28` and `+0x30` when it
writes a capture header (`82f049b0@0x1d644`–`0x1d680`). Its
`vip_power_management` always returns "not supported" (`@0xf9c0`).

### Models are command templates that the loader completes

A network binary graph (NBG) holds per-operation command fragments, weights,
tensor descriptions and relocation records. The loader allocates memory,
patches addresses into the fragments, and generates the surrounding commands
itself. The fragments are not a ready command buffer (observed in
`libNBGlinker.so`; source in the driver). The existing SunstoneOS documents
describe each part:

| Topic | Fact | Detail |
| --- | --- | --- |
| Magic and versions | `VPMN`; A733 fixed-header format `0x1001e` and directory format `0x20000`. `nbinfo` accepts versions up to `0x10020` or exactly `0x20000` | [nbg-model.md](../nbg-model.md), [nbg-directory.md](../nbg-directory.md) |
| Directory records | 48-byte header, 12-byte subheader, 44-byte section records (size check at `82f049b0@0x8708`) | [nbg-directory.md](../nbg-directory.md) |
| Relocations | 40-byte records in `0x1001e`; 28 or 40 bytes in `0x20000`, by address width; four encodings | [nbg-relocation.md](../nbg-relocation.md) |
| Checksum | Reflected CRC-32, polynomial `0xedb88320` (`82f049b0@0x12bc0`) | [nbg-checksum.md](../nbg-checksum.md) |
| Neural-operation descriptors | 192 bytes; address, ring and stride fields | [nbg-rings.md](../nbg-rings.md), [nbg-strides.md](../nbg-strides.md) |
| Loader output | Complete command buffers at two allocation bases | [nbg-loader-trace.md](../nbg-loader-trace.md), [nbg-assembly.md](../nbg-assembly.md) |

The command stream uses the Vivante front-end encoding. The opcode is in
bits 31–27 (source `vip_drv_hardware.c`; observed in both libraries):

| Command | Encoding | Use |
| --- | --- | --- |
| Load state | `0x08000000` \| count << 16 \| state | Writes `count` registers starting at byte offset `state × 4` |
| End | `0x10000000` | Ends a stream |
| No-op | `0x18000000` | Padding |
| Wait, link | `7 << 27`, `8 << 27` | Wait-link loop; compiled out in this build |
| Stall | `0x48000000` after semaphore state `0x0e02` | Front end waits for the pixel engine |
| Chip enable | `0x68000000` \| core mask | Selects cores; `0x6800ffff` selects all |
| Event | state `0x0e01` = `0x44` | Completion interrupt for event 4 from the pixel engine |
| Cache flush | state `0x0e03` = `0xc23` | Flush before completion |
| MMU flush | state `0x0061` = `0xffffff7f` | Prepended by the kernel when tables change |

On one core the kernel only prepends the MMU flush. It rewrites the
synchronisation and chip-enable words, and appends the tail again, only when
a task moves to another core (source `task_schedule.c:680-766`).
`libVIPhal.so` also generates 192-byte transpose descriptors and transpose
load states `0x0e4c`, `0x0e5e`, `0x0e5a`, `0x0428` and `0x0429` at run time
(`3ed5357b@0x4620`, `0x4d04`–`0x4d24`). The loader calls these generators
from code that splits copies into 64 KiB and 128 KiB pieces
(`82f049b0@0x12538`, `0x125ec`); the piece size suggests staging through the
internal SRAM (inferred).

### The feature database describes an eight-core A733 NPU with 512 KiB SRAM

The driver selects a row of its feature database by the product ID it reads
from register `+0x030`; it never compares the revision (source
`vip_drv_context.c:385-397`). `libVIPhal.so` carries an identical copy of the
four-row table at `3ed5357b@0x91b0` and makes the same selection (observed).

| Field | A733 row (product `0x1000003b`, revision `0x9202`) |
| --- | --- |
| VIP cores | 1 (one front end and one interrupt) |
| Neural-network cores | 8 |
| Tensor-processor cores | 0; transposes run on the neural cores (`nn_transpose_phase2` = 1) |
| Internal SRAM | 512 KiB at NPU virtual address `0x400000` |
| AXI SRAM | none; the initialisation stream omits states `0x0e4f`/`0x0e50` |
| MMU | present, page-descriptor mode |
| Virtual address width | 32 bits |
| Hardware cancel | yes, register `+0x3a8` bit 3 |
| Other flags | OCB counters, neural-core probe, shader conformance, XYDP0 |
| Widths | SRAM 128 bytes, AXI 16 bytes, minimum burst 256 bytes |
| Latency hiding at full AXI bandwidth | 1024, the highest of the four rows |

The other rows are product `0xee` (three neural cores, one tensor processor,
no MMU), `0x10000016` (six and two) and `0x10000021` (twelve and none). All
values are source `vip2/inc/vip_feature_database.h:161-183` and observed in
the library copy; their meanings follow the field names (inferred).
[npu-feature-database.json](data/npu-feature-database.json) lists every field.
The identity values `0x1000003b` and `0x9202` were read on a board (confirmed
on a board,
`~/.cache/sunstoneos/evidence/processor-completion-20261024/port-3-1/npu-numerical/runtime/session.json`).

### The kernel driver holds every NPU register sequence

The user libraries contain no NPU register address: the window base
`0x03600000` and the AXI SRAM states are absent from both (observed). Every
register sequence is in the kernel driver source: the
[NPU backsolve](../reference-backsolve-npu.md) derives power, clocks, reset,
initialisation, MMU, trigger, interrupt, cancellation and clock suspension
from it, with line citations. The driver's debug and profiling paths read
further registers (source):

| Offset | Use |
| --- | --- |
| `+0x078`, `+0x07c` | Total and idle cycle counters |
| `+0x040`, `+0x044` | Read and write byte counters |
| `+0x5c0`, `+0x5d0` | OCB read and write counters |
| `+0x438`, `+0x45c` | Memory-controller and shader cycle counters, for hang dumps |
| `+0x660`–`+0x66c`, `+0x450` | Front-end DMA state and command address, for hang dumps |
| `+0x348`, `+0x358` | Error status |
| `+0x380`, `+0x384`, `+0x3b8` | MMU debug and status |

### A bare-metal driver must replace the kernel driver and the loader

A FreeRTOS NPU driver cannot use the libraries, which need Linux file, ioctl
and thread services. It must reproduce:

1. the platform sequence: shared-SRAM routing, power domain, PLL, gates and
   resets ([NPU backsolve](../reference-backsolve-npu.md)). After a FEL boot,
   the boot ROM leaves the shared SRAM routed to the CPU: SYSCTRL
   `0x03000008` read 2 on both boards (confirmed on a board,
   `~/.cache/sunstoneos/evidence/board-session-20261025-4/port-3-1/bb9-read/`
   and `board-session-20261025-3/port-3-3/`). U-Boot normally routes it to
   the NPU ([boot ROM](boot-rom-and-boot0.md#sram-stack-and-the-shared-sram-routing));
2. the controller sequence: identity, software reset, initialisation stream,
   MMU enable, trigger, completion and cancellation;
3. the NPU page tables and memory ownership;
4. the loader: model checks, checksum, relocation and command assembly,
   including the generated prefix and tail;
5. the runtime transpose generation, for models that need it;
6. task scheduling, which the kernel provides for Linux.

## SunstoneOS's use

| Binary | SunstoneOS loads | SunstoneOS reimplements | Must match |
| --- | --- | --- | --- |
| GPU firmware | The pinned image, embedded at build time after a SHA-256 check ([cmake/a733-os.cmake](../../cmake/a733-os.cmake) lines 72–78 and 295) | Host side only, all from the open DDK driver: staging [pvr_firmware.c](../../drivers/gpu/pvr_firmware.c), MIPS tables and boot data [pvr_mips.c](../../drivers/gpu/pvr_mips.c), records [pvr_fw_state.c](../../drivers/gpu/pvr_fw_state.c), queues [pvr_ccb.c](../../drivers/gpu/pvr_ccb.c), start [pvr_gpu_start.c](../../drivers/gpu/pvr_gpu_start.c), control [pvr_gpu_control.c](../../drivers/gpu/pvr_gpu_control.c), stop [pvr_gpu_stop.c](../../drivers/gpu/pvr_gpu_stop.c), events [pvr_gpu_events.c](../../drivers/gpu/pvr_gpu_events.c) | The container layout and BVNC, the boot configuration and fixed-record layout from the open headers, the command and event identifiers the ABI defines, and client types and power subtypes |
| `libVIPhal.so`, `libNBGlinker.so` | Nothing; they run only on the Linux host as format and loader references | Controller [a733_npu_controller.c](../../drivers/npu/a733_npu_controller.c), startup [a733_npu_startup.c](../../drivers/npu/a733_npu_startup.c), tables [a733_npu_mmu.c](../../drivers/npu/a733_npu_mmu.c), memory [a733_npu_memory.c](../../drivers/npu/a733_npu_memory.c), adapter [npu_native.c](../../platform/a733/npu_native.c), loader [nbg_model.c](../../drivers/npu/nbg_model.c), [nbg_checksum.c](../../drivers/npu/nbg_checksum.c), [nbg_relocation.c](../../drivers/npu/nbg_relocation.c), [nbg_assembly.c](../../drivers/npu/nbg_assembly.c), service [npu_service.c](../../drivers/npu/npu_service.c), [npu_session.c](../../drivers/npu/npu_session.c) | Model formats `0x1001e` and `0x20000`, CRC-32, relocations, the generated prefix and tail, the register sequences of the kernel driver, and the A733 feature row |
| `nbinfo` | Nothing | [tools/inspect-nbg.py](../../tools/inspect-nbg.py) | Reports used as a reference by [checks/nbg-inspection.py](../../checks/nbg-inspection.py) |

## New insights

Each item changes or extends a SunstoneOS assumption. None has been tested on
a board.

| Finding | Affects |
| --- | --- |
| The open firmware container's errata list (BRN 44079, 63553) differs from the vendor DDK core header's (63553, 71317): the container omits 71317 and adds 44079. A workaround taken from the DDK source for BRN 71317 has no counterpart in the open firmware. | [gpu-start.md](../gpu-start.md), [gpu-mips.md](../gpu-mips.md) |
| The container's device-information block carries seven feature parameter values that the DDK KM configuration header does not state. The open loader's decode, rather than the DDK configuration header, gives this image's feature values. | [gpu-mips.md](../gpu-mips.md), [gpu-fw-state.md](../gpu-fw-state.md) |
| `libNBGlinker.so` imports a register-read function and reads the NPU identity registers. The NPU backsolve states that it imports none; its conclusion, that the libraries add no register sequence, still holds. | [NPU backsolve](../reference-backsolve-npu.md) |
| The vendor driver selects the feature row by product ID only; SunstoneOS also requires revision `0x9202`. The stricter check matches the board but would refuse another revision of the same product. | [a733_npu_controller.c](../../drivers/npu/a733_npu_controller.c) line 138 |
| The vendor completion event is number 4 (`0x44`); SunstoneOS uses event 0 (`0x40`) and waits for interrupt bit 0. Both are self-consistent; the choice matters only if other events share the status register. | [nbg_command.h](../../drivers/npu/nbg_command.h), [a733_npu_controller.c](../../drivers/npu/a733_npu_controller.c) |
| The A733 NPU has no tensor processor. Models that need transposes rely on neural-core transpose descriptors, which `libVIPhal.so` generates at run time; SunstoneOS's assembler has no such generator. | [nbg-assembly.md](../nbg-assembly.md), [npu-session.md](../npu-session.md) |
| The SDK's v2 sample model is for a different chip (`0x10000016`). Only the v3 samples apply to the A733. | [npu.md](../npu.md), [npu-compiler.md](../npu-compiler.md) |

## Repository content that states GPU firmware facts

Several SunstoneOS documents and checks state GPU firmware addresses, section
sizes and register sequences. Each traces to the open DDK driver source or to
a parse of the container headers as the open loader reads them:

| Passage | Apparent source | Classification |
| --- | --- | --- |
| [docs/gpu.md:90-125](../gpu.md) firmware section table (addresses `0x9fc02000`, `0xbfc00000`, `0xc0000000`, sizes), entry `0xbfc00001`, the information header and six layout entries | ELF program headers and the information footer, read as the open loader reads them | Container parse |
| [docs/gpu-mips.md:41-72](../gpu-mips.md) MIPS heap at `0xc0000000`, page-table format, the eleven wrapper register writes, microMIPS boot mode | The reference check compiles the original `pvr_fw_mips.c` and `rgxstartstop.c` functions (`docs/gpu-mips.md:86`) | Open driver source |
| [docs/gpu-fw-state.md:41-72](../gpu-fw-state.md) record addresses `0xc0fd0000`/`0xc0fe0000`/`0xc0ff0000`, the `0xdeadbee0` fault word, trace buffers of 12,000 words | The reference check compiles the original initialisation functions with the open firmware-interface headers (`docs/gpu-fw-state.md:95`); the fault word is `pvr_fw.c:361` | Open driver source |
| [docs/gpu-start.md:41-72](../gpu-start.md) the 35-step start sequence, reset masks, "Rascal and Dust" release | The reference check compiles the original startup, cache, AXI-ACE and MIPS wrapper functions (`docs/gpu-start.md:126`) | Open driver source |
| `checks/pvr-mips-reference.py`, `pvr-firmware-reference.py`, `pvr-gpu-start-reference.py`, `pvr-gpu-stop-reference.py` | Compile and run the open DDK functions against staged firmware; region tuples are the container layout | Open driver source and container parse |

No passage in the repository was found that decodes firmware instructions,
names an internal firmware function, or lists registers the firmware accesses.

## Open questions

GPU firmware:

- How the vendor DDK firmware `rgx.fw`, which the vendor Linux build loads,
  differs from this open image. `rgx.fw` is not published, and
  `LICENSE.powervr` forbids disassembling the open image.
- Whether the open firmware behaves identically to the DDK firmware across
  the shared ABI. Only board execution can answer it.

NPU binaries:

- What the transpose load states (`0x0e4c`, `0x0e5e`, `0x0e5a`, `0x0428`,
  `0x0429`) and the loader-generated states (`0x0240`, `0x0e1c`, a four-state
  load at `0xd800`) control, and which A733 models reach the runtime
  transpose path.
- Which format fields the loader's roughly 60 comparisons against versions
  `0x10002`–`0x10020` select.
- What the `VNPD` tag (`0x44504e56`, `82f049b0@0x1ebe8`) marks.
- Which chip product `0x10000016` is.
- Whether the completion event number affects any other bit of status
  register `+0x010`.

## Reproducing the inventories

The GPU container parse is
`~/.cache/sunstoneos/evidence/vendor-firmware-20261025/accel/gpu-container/container.py`.
It reads the firmware as the open loader does (`pvr_fw.c`, `pvr_fw_info.h`):
the ELF program headers, the information footer and the device-information
counts. It performs no instruction decoding. Run it with `python3 -I`, passing
the firmware path and `pvr_rogue_fwif_dev_info.h` from the board driver cache
under `~/.cache/sunstoneos/sources/gpu/`.

The NPU scripts in `~/.cache/sunstoneos/evidence/vendor-firmware-20261025/accel/npu/scripts/`
produced the NPU inventories. `gen.py` writes the four JSON files. `sizes.c`
and `layout.c` compute the driver's structure sizes and field offsets with
Clang for AArch64. The Python helpers locate constants, call sites, the
library's command-size table and its feature-table copy, and scan for
embedded blobs.
