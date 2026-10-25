# A733 GPU firmware and NPU vendor binaries

This reference describes the contents of the prebuilt GPU firmware and the
prebuilt NPU libraries that SunstoneOS depends on for the Allwinner A733. It
records their formats, the host interfaces they define, the hardware they
program, and the behaviour that a FreeRTOS driver must reproduce or match. A
developer should be able to work with these binaries from this document and
the inventories it cites, without disassembling them again.

Status: **reference analysis**. The analysis read the binaries, the published
driver sources and the SunstoneOS sources. It made no board access and changed
no runtime code. No GPU firmware has executed under SunstoneOS; the NPU has
executed jobs once, with wrong output (see [NPU](../npu.md)).

The reader is assumed to know device drivers, ring buffers, page tables and
ELF files. Existing SunstoneOS documents describe each implemented component;
this document links to them instead of repeating them.

## Citations, labels and machine-readable inventories

Each fact carries a citation and one of four labels:

- **observed**: read from the binary itself. The citation is the first eight
  hex digits of the binary's SHA-256 and a file offset (`1db1c399@0x1f000`) or
  a firmware virtual address (`1db1c399:0xc0006df0`).
- **source**: matches a named published source file, cited as file and line
  or symbol.
- **board**: confirmed on an A733 board, citing the evidence directory.
- **inferred**: concluded from the observations; the basis is stated.

Firmware virtual addresses refer to the GPU firmware's own address space. The
firmware has no symbol table, so functions are named by their start address
and, where the binary reveals it, by the source module that the binary's
assertion strings name.

The complete inventories are JSON files outside Git, under
`~/.cache/sunstoneos/evidence/vendor-firmware-20261025/accel/`. The tables
below summarise them.

| File | Contents |
| --- | --- |
| `gpu/gpu-fw-image.json` | ELF header, program headers, sections, information footer, layout entries, procedure-descriptor summary, embedded strings |
| `gpu/gpu-fw-devinfo.json` | Errata, enhancement and feature bits with values, and the comparison with the vendor DDK configuration |
| `gpu/gpu-fw-functions.json` | All 254 firmware functions: frame size, calls, callers, GPU registers read and written, trace messages, and roles where established |
| `gpu/gpu-fw-registers.json` | Every statically resolved GPU register access: offset, register name, access sites and window |
| `gpu/gpu-fw-dispatch.json` | Command dispatch tables with each command identifier, case address and first call |
| `gpu/gpu-fw-trace-ids.json` | The 148 trace-message identifiers that the firmware code contains, with group, parameter count and format text from the open driver |
| `gpu/scripts/` | The analysis scripts that produced the GPU files from the pinned firmware and headers |
| `npu/npu-binaries.json` | Every cached SDK binary with hashes, imports and exports; the uncached SDK tree by family; the kernel driver's source files |
| `npu/npu-ioctl.json` | The 30 driver commands with payload structures, field offsets, kernel handlers and library call sites |
| `npu/npu-feature-database.json` | The four rows of the VIPLite feature database, with the A733 row interpreted |
| `npu/npu-constants.json` | 47 labelled constants: device node, identity values, register offsets, command opcodes and format constants |

Curated copies are tracked in the repository under [data/](data/). Each
starts with the binary's name, SHA-256 and source reference, and keeps only
short identifiers:

| Tracked file | Contents |
| --- | --- |
| [gpu-firmware-layout.json](data/gpu-firmware-layout.json) | ELF and section layout, information footer, layout entries, device information and the DDK comparison |
| [gpu-firmware-commands.json](data/gpu-firmware-commands.json) | Host-command, client-command and power-subtype dispatch with case addresses; firmware events the image builds; functions with established roles |
| [gpu-firmware-interface.json](data/gpu-firmware-interface.json) | Virtual memory map, shared-record addresses, power-state values, interrupt registers, every resolved register access and the trace-message identifiers |
| [npu-binaries.json](data/npu-binaries.json) | NPU binaries, exported and imported symbols, and the SDK tree inventory |
| [npu-ioctl.json](data/npu-ioctl.json) | Driver commands, payload layouts and handlers |
| [npu-feature-database.json](data/npu-feature-database.json) | The per-chip feature table |
| [npu-constants.json](data/npu-constants.json) | Register offsets, command opcodes and format constants |

[The vendor firmware sources](README.md) record where each binary can be
fetched, its revision, SHA-256 and license.

The scripts disassemble the firmware with `llvm-objdump-21 --mattr=+micromips`
in a scratch directory and keep only derived tables. Neither the inventories
nor this document contain disassembly listings or blob bytes.

## Binary identities

| Binary | SHA-256 | Bytes | Provenance | License |
| --- | --- | ---: | --- | --- |
| GPU firmware `rogue_36.56.104.183_v1.fw` | `1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2` | 131,072 | Imagination `linux-firmware` revision `8a58f818`, pinned in [sources.lock.json](../../sources.lock.json) as `gpu-firmware` | Binary redistribution under `LICENSE.powervr`, pinned as `gpu-firmware-license` |
| NPU hardware-abstraction library `libVIPhal.so` (VIPLite 2, AArch64) | `3ed5357b26bd6c4fb68fbdc0b21d637227dda711d1fe67b9e28accdc48bb11f2` | 39,416 | AI SDK `ZIFENG278/ai-sdk` revision `fc90006d`, `viplite-tina/lib/aarch64-none-linux-gnu/v2.0/` | Vendor binary; the SDK has no license file, and its headers reserve all rights to Vivante |
| NPU network loader `libNBGlinker.so` (VIPLite 2, AArch64) | `82f049b0ed0065dd…` (full value in [npu-binaries.json](data/npu-binaries.json)) | 174,728 | Same directory | Same |
| NPU model inspector `nbinfo` (x86-64 with DWARF) | `5ef6bb4f1590e0d1…` | 187,008 | AI SDK `tools/` | Same |
| SDK sample model for the A733, `v3/network_binary.nb` | `fc0d50bdc863e4dfc74eef6dad9d34655c560e759a2f426979a01c505fa9b95f` | 964,224 | AI SDK `examples/vpm_run/operator/v3/` | Same |
| VIPLite 2 kernel driver | Source, per-file hashes in `npu-binaries.json` | — | `orangepi-xunlong/linux-orangepi` `2ac08e8c`, `bsp/drivers/npu/aw_nna_vip/vip2/` | MIT or GPL-2.0 for the core files ([npu.md](../npu.md)) |

The two `variant.fw` files under
`~/.cache/sunstoneos/evidence/gpu-start-20261016/` and `gpu-stop-20261016/`
are SunstoneOS test fixtures. They differ from the pinned image only in
device-information bytes between offsets `0x1e022` and `0x1e0da`, which the
startup and shutdown checks change to exercise alternate feature paths. They
are not vendor firmware.

`LICENSE.powervr` permits redistribution of the unmodified firmware binary
with its notice. It also states that no reverse engineering, decompilation or
disassembly of the firmware is permitted. The image-format, device-information
and string facts below come from reading the file as data. The code
organisation, dispatch tables, register inventory, and the power, recovery and
trace behaviour come from disassembling the firmware. Whether those
disassembly-derived sections and their data files may stay in the repository
is a decision for the project owner. The repository keeps the binary itself
outside Git, and no disassembly listing or firmware byte appears here.

## GPU firmware

### The image is a microMIPS ELF followed by two 4 KiB information blocks

The file has three parts. The driver locates the two trailing blocks from the
end of the file (`pvr_fw.c:101-183` in the board driver).

| Part | File offsets | Contents | Label |
| --- | --- | --- | --- |
| ELF32 executable | `0x0`–`0x1d083` | Little-endian MIPS executable; flags `0x72001001` (MIPS32r2, o32, microMIPS); entry `0xbfc00001`, where the low bit selects microMIPS | observed `1db1c399@0x0` |
| Zero padding | `0x1d084`–`0x1dfff` | All zero | observed |
| Device-information block | `0x1e000`–`0x1efff` | Four 64-bit counts (1, 1, 2, 29) followed by one errata word, one enhancement word, two feature words and 29 feature parameters; the rest is zero | observed `1db1c399@0x1e000`; layout matches `pvr_fw_info.h` `struct pvr_fw_device_info_header` |
| Information footer | `0x1f000`–`0x1ffff` | 48-byte header, then six 24-byte layout entries; the rest is zero | observed `1db1c399@0x1f000`; matches `pvr_fw_info.h` `struct pvr_fw_info_header` |

The information header records:

| Field | Value | Meaning |
| --- | --- | --- |
| Information version | 3 | The only version the driver accepts |
| BVNC | 36.56.104.183 | GPU revision the image supports, as branch, version, number of scalable units and configuration |
| Firmware page size | 4,096 | MIPS page size |
| Flags | `0x1` | Open-source compatible; the driver refuses images without this bit |
| Firmware version | 1.1, build 6976702 | ABI major 1 |
| Device-information size | 4,096 | Size of the preceding block |

The five program headers are the only bytes the host loads. Section headers,
`.pdr`, `.comment` and the trailing blocks are metadata.

| Segment | Sections | Virtual address | File offset | Bytes in file | Memory bytes | Flags |
| --- | --- | --- | --- | ---: | ---: | --- |
| 0 | `.exctext` | `0x9fc02000` | `0xd4` | 3,088 | 3,088 | read, execute |
| 1 | `.bootandnmitext` | `0xbfc00000` | `0xce4` | 1,748 | 1,748 | read, execute |
| 2 | `.text`, `.text.startup` | `0xc0000000` | `0x13b8` | 93,916 | 93,916 | read, execute |
| 3 | `.bss` | `0xc0032000` | `0x18298` | 0 | 8 | read, write |
| 4 | `.data`, `.shdata`, `.shdatabss`, `.rodata`, `.data_noncritical`, `.rodata_noncritical`, `.eh_frame` | `0xc0032008` | `0x182c8` | 10,712 | 10,712 | read, write |

The six layout entries tell the host where each segment belongs in its two
allocations. [gpu.md](../gpu.md#matching-firmware-runs-on-an-internal-mips-processor)
lists them with their reserved sizes; SunstoneOS stages them in
[pvr_firmware.c](../../drivers/gpu/pvr_firmware.c). In summary, the code
allocation holds main code at offset 0, exception code at 94,208 and boot
code at 98,304. The data allocation holds private data at 0, boot data at
12,288 and the stack at 16,384.

Other metadata in the image (all observed):

- **Compiler.** `.comment` at `0x1cc78` names Codescape GNU Tools 2014.07-1
  for MIPS Bare Metal, GCC 4.9.0.
- **Functions.** `.pdr` at `0x1acb8` holds 254 32-byte procedure descriptors.
  Each gives a function start, saved-register mask and frame size. There are
  238 functions in main code, 12 in exception code and 4 in boot code. No
  symbol table exists.
- **Strings.** `.rodata_noncritical` at `0x1a7c8` holds the only text: ten
  firmware source-file names, 17 assertion expressions and messages, eleven
  performance-counter block names and a hexadecimal digit table. The
  firmware emits trace messages as numeric identifiers; their format text
  exists only in the host driver's `pvr_rogue_fwif_sf.h`.

### The firmware runs on a microAptiv MIPS core with a fixed virtual memory map

The firmware processor is the GPU's internal MIPS32r2 core, a microAptiv
variant in the open driver's naming (`pvr_rogue_mips.h`, cache-line constant).
The device-information block advertises the MIPS feature and not META or
RISC-V. The core sees 32-bit virtual addresses. Its translation reaches the
36-bit system bus through the GPU's address-remap registers.

| MIPS virtual range | Contents | Set up by | Label |
| --- | --- | --- | --- |
| `0xbfc00000`–`0xbfc00fff` | Boot and NMI code, through the wrapper's boot remap of physical `0x1fc00000` | Host wrapper writes before reset release | source `pvr_rogue_mips.h`; observed entry |
| `0xbfc01000`–`0xbfc01fff` | Boot data: the host's 64-byte boot configuration at offset 0, NMI shared data from offset `0x100`, exception scratch near `0x800` | Host writes the configuration; firmware writes the rest | source `pvr_rogue_mips.h` `struct rogue_mipsfw_boot_data`; observed accesses `1db1c399:0xbfc01104`, `0xbfc01810` |
| `0x9fc02000`–`0x9fc02fff` | Exception vectors and handlers, through the code remap | Host wrapper writes | source; observed |
| `0xc0000000`–`0xc0ecffff` | Main firmware heap: code at the base, private data at `0xc0032000`, and host-allocated firmware objects | Host page table | source `pvr_fw.c:29-42`, `pvr_fw_mips.c:16-21` |
| `0xc0ed0000`–`0xc0fcffff` | 1 MiB reserved window for mappings that the firmware creates at run time | Firmware writes these page-table entries itself | observed stores to page-table bytes `0x3b40`–`0x3b80` at `1db1c399:0xc00000a0`–`0xc0000464`; size check against `RGXMIPSFW_RUNTIME_MAPPING_MAX_SIZE` |
| `0xc0fd0000`, `0xc0fe0000`, `0xc0ff0000` | Connection control, OS initialisation and system initialisation records, 64 KiB apart | Host | source `pvr_fw.c:36-42`; observed constants at `1db1c399:0xc0008b10`, `0xc0008a04`, `0xc0000878` |
| `0xcf000000` | The host-built page table, four 4 KiB pages | Firmware TLB entry from boot data | source `ROGUE_MIPSFW_PT_VIRTUAL_BASE`; observed |
| `0xcf600000` | Stack page | Firmware TLB entry from boot data | source; observed |
| `0xcf800000` | GPU register bank, offset 0 = `RGX_CR` offset 0 | Firmware TLB entry from the boot data register base | source `ROGUE_MIPSFW_REGISTERS_VIRTUAL_BASE`; observed |
| `0xcfa00000` | Second view of the register bank, 2 MiB above the first | Firmware | observed; meaning inferred, see [below](#the-firmware-programs-gpu-registers-through-two-windows) |

The firmware masks firmware addresses taken from host records with
`0xc0fffffc` at 107 sites before dereferencing them. This confines those
pointers to the 16 MiB heap and clears their two flag bits (observed, for
example `1db1c399:0xc000087c`; purpose inferred).

### Exception handlers refill the TLB from the host page table

The MIPS core has a software-refilled TLB. The refill vector at `0x9fc02000`
indexes the page table through `0xcf000000` with the faulting address. For
each refill it also writes `RGX_CR_MIPS_ADDR_REMAP_RANGE_CONFIG` (`0x878`,
`0x87c`) with the physical frame from the page-table entry (observed
`1db1c399:0x9fc02014`–`0x9fc02060`). The remap registers extend the core's
32-bit physical addresses to the 36-bit bus; their format is in
`pvr_rogue_mips.h` (`ROGUE_MIPSFW_ENTRYLO_PFN_MASK_ABOVE_32BIT`).

The general-exception handler at `0x9fc02180` records the fault in the NMI
shared area (`0xbfc01104`) for the host's debug dump. The boot vector at
`0xbfc00000` tests the CP0 Status NMI bit; on NMI it saves registers and
calls `0xbfc0048d`, which copies the 32 wrapper remap entries through
`RGX_CR_MIPS_ADDR_REMAP_RANGE_READ`/`_DATA` (`0x880`, `0x888`) into the NMI
area (observed `1db1c399:0xbfc0052e`–`0xbfc0056a`). The open driver's debug
code reads these areas (`pvr_rogue_mips.h`, NMI offsets).

### Code is organised into modules that the assertion strings name

The assertion calls pass a source-file string, which identifies the module of
each function that asserts. The roles of other functions follow from their
dispatch position, register use and trace messages; the functions inventory
marks each role with its basis.

| Address | Module or role | Evidence |
| --- | --- | --- |
| `0xbfc00000` | Boot and NMI vector | Entry point; NMI test |
| `0x9fc02000` | TLB refill and exception vectors | Page-table walk, remap writes, `ERET`, `WAIT` |
| `0xc0016df4` | `main` (`.text.startup`) | Enables `RGX_CR_MIPS_WRAPPER_IRQ_ENABLE` (`0x8a0`), then calls `0xc00011b4` |
| `0xc00011b4` | Initialisation and OS state | Associates MTS threads and data-master interrupts, logs the boot state of private data and OS state transitions |
| `0xc000086c` | `rgxfw_init.c` | Checks the host alignment-check array and trace-buffer control |
| `0xc00016ec` | `rgxfw_gpu_init.c` | Reinitialises GPU units after power-up or recovery; writes `RGX_CR_CLK_CTRL` and `RGX_CR_CLK_CTRL2` |
| `0xc00050d4` | `rgxfw_bif.c` | Memory-context activation; writes BIF catalogue bases |
| `0xc00056ec`, `0xc0005d38`, `0xc000644c` | `rgxfw_client_ccb.c` | Client-queue fence, update and kick dispatch |
| `0xc0006df0` | `rgxfw_kernel_ccb.c` | Host-command dispatch |
| `0xc0007730` | `rgxfw_irq.c` | Interrupt task; reads `RGX_CR_EVENT_STATUS` |
| `0xc000c3b4` | `rgxfw_hwr.c` | Hardware recovery |
| `0xc000ee74` | `rgxfw_gpu_memory.c` | GPU memory helpers |
| `0xc0013420`, `0xc0013ff0` | `rgxfw_pm.c` | Parameter manager and free-list reconstruction |
| `0xc0001c54` | Trace-buffer writer | Called by seven wrappers at `0xc0001f34`–`0xc0002090` that pass zero to six parameters; the 1-parameter wrapper has 116 callers |
| `0xc0009298` | Firmware-event post | Called by every function that builds a firmware-to-host event; writes `RGX_CR_MIPS_WRAPPER_IRQ_STATUS` |

### Host and firmware exchange three fixed records, three kinds of queue and return statuses

[gpu-fw-state.md](../gpu-fw-state.md) defines the host-built records and
[gpu-queues.md](../gpu-queues.md) the queue layouts. The firmware side of
that interface follows. CCB is the driver's name for a circular command
buffer; HWPerf is the firmware's hardware performance-counter stream.

| Object | Host role | Firmware behaviour | Label |
| --- | --- | --- | --- |
| Boot configuration (`0xbfc01000`) | Writes stack, register base, four page-table addresses and page count | Reads at reset to build its permanent TLB entries | source `pvr_fw_mips.c:70-95`; observed reads in boot code |
| System initialisation (`0xc0ff0000`) | Points to runtime configuration, trace, HWPerf, fault page and firmware system data | Read at start; writes the firmware-started flag | source `pvr_rogue_fwif.h` `struct rogue_fwif_sysinit`; observed 111 references |
| OS initialisation (`0xc0fe0000`) | Points to the host-command, return-status and firmware-event queues and OS data | Publishes a 64-byte compatibility record at byte 40 | source `struct rogue_fwif_osinit`; [gpu-control.md](../gpu-control.md) |
| Connection control (`0xc0fd0000`) | Requests OS state changes | Logs transitions offline, ready, active and offloading (trace `0x700410a6`) | source `struct rogue_fwif_connection_ctl`; observed |
| Host-command queue (KCCB) | Writes commands, advances write offset, writes `RGX_CR_MTS_SCHEDULE` | Background task dispatches each command | observed dispatcher `1db1c399:0xc0006df0` |
| Return statuses | Clears one word per command | Writes completion flags | source |
| Client queues (CCCB) | Writes fence, update and kick commands per context | Fence, update and kick tables below | observed |
| Firmware-event queue (FWCCB) | Reads events after the interrupt | Writes events, then raises the host interrupt | observed `1db1c399:0xc0009298` |

#### Host commands

The KCCB dispatcher subtracts `0x2abc0065` (command 101 with the
`0x2abc` magic in the upper half) from the command word. It then indexes a
118-entry jump table at `0xc0033db8`, covering commands 101 to 218 (observed
`1db1c399:0xc0006e5e`–`0xc0006e7c`). Commands outside that range, and
entries that point to the default case, reach the "unknown KCCB command"
trace and assertion.

| ID | Command (open-driver name) | Handled | Case address | SunstoneOS sends |
| ---: | --- | --- | --- | --- |
| 101 | `KICK` | yes | `0xc00071df` | yes, [pvr_cccb.c](../../drivers/gpu/pvr_cccb.c) |
| 102 | `MMUCACHE` | yes | `0xc00071a7` | yes, [pvr_gpu_flush.c](../../drivers/gpu/pvr_gpu_flush.c) |
| 103 | `BP` (breakpoint) | yes | `0xc00071ed` | no |
| 105 | `SLCFLUSHINVAL` | yes | `0xc00070f5` | no |
| 106 | `CLEANUP` | yes | `0xc0007109` | yes, [pvr_gpu_service.c](../../drivers/gpu/pvr_gpu_service.c) |
| 107 | `POW` | yes | `0xc0006e7f` | yes, [pvr_gpu_stop.c](../../drivers/gpu/pvr_gpu_stop.c) |
| 108, 109 | `ZSBUFFER_BACKING_UPDATE`, `ZSBUFFER_UNBACKING_UPDATE` | yes | `0xc0007163`, `0xc00070bd` | no |
| 110 | `FREELIST_GROW_UPDATE` | yes | `0xc0007067` | yes, through the free-list service |
| 112 | `FREELISTS_RECONSTRUCTION_UPDATE` | yes | `0xc0006fff` | yes, through the free-list service |
| 115 | `HEALTH_CHECK` | yes | `0xc0006ec3` | yes, [pvr_gpu_control.c](../../drivers/gpu/pvr_gpu_control.c) |
| 116 | `FORCE_UPDATE` | yes | `0xc0006fe7` | no |
| 117 | `COMBINED_GEOM_FRAG_KICK` | yes | `0xc0006fcf` | yes, [pvr_cccb.c](../../drivers/gpu/pvr_cccb.c) |
| 201 | `HWPERF_UPDATE_CONFIG` | yes | `0xc0006fc1` | no |
| 203 | `HWPERF_CTRL_BLKS` | yes | `0xc0006f99` | no |
| 204 | `CORECLKSPEEDCHANGE` | yes | `0xc0006f8d` | no |
| 206 | `LOGTYPE_UPDATE` | yes | `0xc0006f7f` | no |
| 208 | `OSID_PRIORITY_CHANGE` | yes; shares the health-check case | `0xc0006ec3` | no |
| 209 | `STATEFLAGS_CTRL` | yes | `0xc0006f63` | no |
| 217 | `HWPERF_CONFIG_ENABLE_BLKS` | yes | `0xc0006f01` | no |
| 218 | `HWPERF_SELECT_CUSTOM_CNTRS` | yes | `0xc0006f71` | no |
| 114, 118, 200, 207, 212, 213, 215, 216, 220 | `NOTIFY_WRITE_OFFSET_UPDATE`, `OS_ONLINE_STATE_CONFIGURE`, `REGCONFIG`, `PDVFS_LIMIT_MAX_FREQ`, `PDVFS_LIMIT_MIN_FREQ`, `PHR_CFG`, `WDG_CFG`, `COUNTER_DUMP`, `HWPERF_CONFIG_BLKS` | no: default case, or outside the table; so are the unnamed identifiers 104, 111, 113, 119–199, 202, 205, 210, 211 and 214 | — | no |

Command names come from `pvr_rogue_fwif.h:1156-1238`; the handled set is
observed. The power command reads its subtype from byte 8 and branches for
subtypes 1 to 4: power-off request, forced idle, dust-count change (a dust
is a group of shader units powered together) and
active-power-management latency change (observed `1db1c399:0xc0006e84`–
`0xc0006e9a`; names from `enum rogue_fwif_power_type`).

#### Client-queue commands

Three tables dispatch client-queue commands (all observed; names from
`pvr_rogue_fwif.h:1508-1549`):

| Table | Function | Range | Handled types |
| --- | --- | --- | --- |
| `0xc0033d8c`, 11 entries | `0xc000644c` (kick) | 201–211 with the task bit `0x8000` | GEOM, TQ_3D, FRAG, FRAG_PR, CDM, PRE_TIMESTAMP, NULL, ABORT; 206–208 (TQ_TDM, FBSC_INVALIDATE, TQ_2D) reach the default case |
| `0xc0033d48`, 10 entries | `0xc00056ec` (fences) | 212–221 | FENCE, UPDATE, RMW_UPDATE, FENCE_PR, PRIORITY, POST_TIMESTAMP, UNFENCED_UPDATE, UNFENCED_RMW_UPDATE, PADDING; 220 reaches the default case |
| `0xc0033d70`, 7 entries | `0xc0005d38` (updates) | 213–219 | UPDATE, RMW_UPDATE, POST_TIMESTAMP, UNFENCED_UPDATE, UNFENCED_RMW_UPDATE |

SunstoneOS sends kick types 201 (GEOM), 203 (FRAG), 204 (FRAG_PR) and
205 (CDM, compute) and update type 213
([pvr_render_service.c](../../drivers/gpu/pvr_render_service.c),
[pvr_compute_job.c](../../drivers/gpu/pvr_compute_job.c),
[pvr_mimus_service.c](../../drivers/gpu/pvr_mimus_service.c)). The firmware
handles all of them.

#### Firmware events

The command words that the firmware builds identify the events it can send.
Each word appears as a constant in its own function, which then calls the
event post at `0xc0009298` (observed; names from `pvr_rogue_fwif.h:1371-1398`).

| ID | Event | Built in |
| ---: | --- | --- |
| 101 | `ZSBUFFER_BACKING` | `0xc0007df4` |
| 102 | `ZSBUFFER_UNBACKING` | `0xc0007ed0` |
| 103 | `FREELIST_GROW` | `0xc00124c4` |
| 104 | `FREELISTS_RECONSTRUCTION` | `0xc0013338` |
| 105 | `CONTEXT_RESET_NOTIFICATION` | `0xc0007f74`, `0xc0008018` |
| 106 | `DEBUG_DUMP` | `0xc000bcac`, `0xc000c3b4`, `0xc000cedc` |
| 107 | `UPDATE_STATS` | `0xc0008794` |
| 109 | `REQUEST_GPU_RESTART` | `0xc00080f4` |
| 112 | `CONTEXT_FW_PF_NOTIFICATION` | `0xc000b990` |

No function builds event 108 (`CORE_CLK_RATE_CHANGE`). The firmware therefore
never asks the host to change the GPU clock (inferred from its absence).

#### Interrupts in both directions

- **Host to firmware.** The host writes `RGX_CR_MTS_SCHEDULE` (`0xb00`) after
  publishing a command. The MTS (the GPU's hardware task scheduler) starts
  the firmware's background or interrupt task. The firmware itself writes
  `RGX_CR_MTS_SCHEDULE` from 29 functions to queue its own tasks (observed;
  see the registers inventory).
- **GPU to firmware.** The interrupt task at `0xc0007730` reads
  `RGX_CR_EVENT_STATUS` (`0x130`), clears events through
  `RGX_CR_EVENT_CLEAR` (`0x138`) and dispatches per data master (the GPU front end for one
  workload type, such as geometry, fragment or compute); it logs
  the event status with trace `0x70017006`. Data-master interrupt enables are
  `RGX_CR_MTS_DM0`–`DM4_INTERRUPT_ENABLE` (`0xb58`–`0xb78`).
- **Firmware to host.** Five functions write
  `RGX_CR_MIPS_WRAPPER_IRQ_STATUS` (`0x8a8`): the event post, the update
  path, the KCCB dispatcher, the power-state publication at `0xc000ad1c` and
  one interrupt-task helper (observed). The host sees bit 0 of that register
  and clears it through `RGX_CR_MIPS_WRAPPER_IRQ_CLEAR` (`0x8b0`), as
  [gpu-control.md](../gpu-control.md) and [gpu-interrupts.md](../gpu-interrupts.md)
  describe. Writing the status register raises the host interrupt (inferred
  from these sites and the driver's clear path in `pvr_fw_mips.c:176-186`).

### The firmware programs GPU registers through two windows

Static tracking of constant base addresses resolves 295 distinct register
offsets in 1,165 accesses. Names come from the board driver's
`pvr_rogue_cr_defs.h` and the vendor DDK's `rgx_cr_defs_km.h`. Those headers
define only registers that a host may touch, so 211 offsets used by the
firmware have no public name. The unnamed offsets cluster in blocks that
belong to the geometry, ISP, PDS/USC, texture and performance units; the
inventory lists each one.

The firmware loads registers through `0xcf800000` and performs most stores
through `0xcfa00000`, the same offsets plus 2 MiB. For example, the
counter-block routine at `0xc0003148` adds `0x200000` to a register offset
before a store. The boot configuration supplies one register base. The second
window is therefore an alias of the same bank, probably mapped with a
different cache or write attribute (inferred).

| Function | Registers | Firmware use | Label |
| --- | --- | --- | --- |
| Identity | `CORE_ID__PBVNC` `0x20`, `MULTICORE_SYSTEM` `0xf308` | Read 67 and 66 times; firmware checks its own core and core count | observed |
| Time | `TIMER` `0x160`/`0x164` | Read 112 times for deadlines and timestamps | observed |
| Per-core selection | `XPU_BROADCAST` `0x890` | Selects one core or broadcast before register sequences (read 76, written 14 times) | observed |
| Reset | `SOFT_RESET` `0x100`/`0x104` | Written by recovery (`0xc000c3b4`) and two helpers it calls | observed |
| Clock gating | `CLK_CTRL` `0x0`, `CLK_CTRL2` `0xd200` | Written only by `0xc00016ec`, after power-up or recovery | observed |
| Unit power | Unnamed `0x38` | Written by the unit power request `0xc000a690`, which then polls `EVENT_STATUS` against `TIMER` and retries when the power controller aborts (trace `0x7000a033`) | observed; register meaning inferred |
| Bus security | `SYS_BUS_SECURE` `0xa100` | Read and written during initialisation and unit power changes | observed |
| Memory management (BIF, the GPU bus interface and MMU) | `BIF_CAT_BASE1`–`6`, `BIF_PM_CAT_BASE_*`, `BIF_CTRL_INVAL` `0x12a0`, `BIF_FAULT_BANK0_MMU_STATUS` `0x12b0`, `MCU_FENCE` `0x1740` | Context activation, invalidation and fault reporting | observed |
| System-level cache | `SLC_CTRL_MISC` `0x3800`, `SLC_CTRL_FLUSH_INVAL` `0x3818`, `SLC_CTRL_BYPASS` `0x3828`, `SLC_SIZE_IN_KB` `0x3970` | Flush and invalidate from nine functions; bypass set from the cache size after power-up | observed |
| MIPS wrapper | `MIPS_ADDR_REMAP_*` `0x868`–`0x88c`, `MIPS_WRAPPER_IRQ_ENABLE`/`STATUS` | TLB support, NMI dump, host interrupt | observed |
| Performance | `PERF_3D_PHASE`, `PERF_COMPUTE_PHASE`, `PERF_3D_CYCLE`, `PERF_COMPUTE_CYCLE` (`0x6010`–`0x6030`) | Read for statistics and lockup detection | observed |
| Scratch | `SCRATCH15` `0x1a78` | Written once by the interrupt task | observed |

The firmware's register window covers only the GPU at `0x01800000`. It
contains no access to A733 clock-controller, PCK-600 power-controller or PMIC
addresses (observed: no constant in those ranges is used as an address). It
requests SoC power and clock changes only indirectly, through the host.

### The host prepares memory, wrapper and reset state before the firmware starts

[gpu-start.md](../gpu-start.md) gives the 35-step start sequence that
SunstoneOS reproduces and checks against the board driver. The binary adds
these requirements to it:

| Requirement | Reason in the binary | SunstoneOS |
| --- | --- | --- |
| Load only the five program segments, at the layout-entry offsets, with every other allocation byte zero | Section headers and trailers are not loaded; `.bss` is 8 bytes | [pvr_firmware.c](../../drivers/gpu/pvr_firmware.c) |
| Boot configuration at boot-data offset 0 with the stack, register base and page-table addresses | Boot code builds its permanent TLB entries from it | [pvr_mips.c](../../drivers/gpu/pvr_mips.c) |
| A complete page table for the 16 MiB heap at the four page-table pages | The TLB refill handler reads it for every miss | [pvr_mips.c](../../drivers/gpu/pvr_mips.c) |
| Page-table pages writable by the firmware, and the 1 MiB window at `0xc0ed0000` left unmapped by the host | The firmware writes its own runtime-mapping entries there | Reserved by [pvr_fw_state](../gpu-fw-state.md); writability open, see [new insights](#new-insights) |
| Initialisation records at `0xc0fd0000`, `0xc0fe0000` and `0xc0ff0000` | Fixed constants in the firmware | [pvr_fw_state.c](../../drivers/gpu/pvr_fw_state.c) |
| Host pointers inside `0xc0000000`–`0xc0fffffc` | The firmware masks every pointer to that range | Builder checks placement |
| Boot, code and data remaps, wrapper idle control, and microMIPS boot mode | Entry `0xbfc00001` is microMIPS boot code | [pvr_gpu_start.c](../../drivers/gpu/pvr_gpu_start.c) |
| Bus-security register cleared, then reset sequence | Firmware also rewrites `SYS_BUS_SECURE` itself | [pvr_gpu_start.c](../../drivers/gpu/pvr_gpu_start.c) |
| Powered GPU domains, a running GPU clock and no pending reset | The firmware cannot reach SoC power or clock registers | [a733_gpu_power.c](../../drivers/gpu/a733_gpu_power.c); [gpu-power.md](../gpu-power.md) |

### Power: the host owns the SoC domains and the firmware owns GPU units

- **SoC power domains and clocks.** The host powers the GPU's PCK-600
  domains, enables the GPU PLL and gates, and sets DCDC4. The vendor kernel
  writes the GPU-core domain policy directly and never waits for its status
  ([reference-backsolve-gpu.md](https://github.com/pedapudi/sunstoneos/blob/work/backsolve-gpu-1025/docs/reference-backsolve-gpu.md)
  on branch `work/backsolve-gpu-1025`). The firmware has no access to these
  controls (observed above).
- **GPU units.** Inside the GPU, the firmware switches its own power islands.
  The unit power request at `0xc000a690` logs "HW request on or off" with a
  64-bit unit mask (trace `0x7003a045`). The dust-count change at
  `0xc000ab80` handles power subtype 3 (observed).
- **Idle.** When no work remains, `0xc000adb0` logs readiness for power-down
  (trace `0x7003a006`). The power-state publication at `0xc000ad1c` writes
  state 3, `ROGUE_FWIF_POW_IDLE` ("idle waiting for host handshake"), at byte 8
  of firmware system data and raises the host interrupt (observed; enum in
  `pvr_rogue_fwif.h:105-110`). Without a host request, the firmware does not
  power itself off.
- **Forced idle and power-off.** Power subtypes 2 and 1 lead to states 2
  (forced idle) and 0 (off) at the same byte, which
  [gpu-isolation.md](../gpu-isolation.md) checks. A power-off request is
  logged with its forced flag (trace `0x7002a03b`, `0xc000b418`).
- **Active power management.** Subtype 4 changes the idle latency. The
  runtime configuration's `active_pm_latency_ms` sets the initial value; the
  reference initialiser writes 0 with the persistent flag set
  (`pvr_fw.c:432-444`).
- **DVFS.** The firmware accepts a new core clock through command 204 and
  logs it (trace `0x7001105d`). It rejects both proactive-DVFS limit
  commands (207, 212) and never sends event 108. Frequency and voltage
  changes are therefore host-initiated only (observed absence).

### Recovery, health, HWPerf and trace are built in

- **Lockup detection and recovery.** `rgxfw_hwr.c` at `0xc000c3b4` and its
  helpers contain 27 recovery trace messages. They cover data-master lockup
  and deadline overrun, false-positive detection, page faults on bank 0,
  signature and CRC checks, soft reset with per-core retry, SLC and MMU
  flush failures, and free-list reconstruction. Recovery ends with event 105
  for the affected context, event 106 for a debug dump, or event 109 when the
  firmware needs a full GPU restart (observed).
- **Health check.** Commands 115 and 208 go straight to the dispatcher's
  common completion path, which advances the queue read offset and
  increments a per-OS command counter (observed `1db1c399:0xc0006ec2`–
  `0xc0006eea`). Command 215 (`WDG_CFG`) is not handled, and the device
  information does not advertise the firmware watchdog-timer feature.
- **HWPerf.** Commands 201, 203, 217 and 218 configure performance
  counters; 216 and 220 are not handled. The routine at `0xc0003148`
  dispatches counter-block IDs `0x00`–`0x6f` to register writes. The strings
  name the TA, rasterisation, BIF/PM cache, Tornado, Jones, TPU MCU, USC,
  Texas, raster, Blackpearl and PBE blocks (observed `1db1c399@0x1aad8`–
  `0x1abc0`).
- **Trace.** The code contains 148 identifiers from the open driver's
  775-entry trace table: 45 main, 27 recovery, 17 in the SPM group, 10 parameter manager,
  9 context switch, 8 cleanup, 7 miscellaneous, 7 render-target data, 6 BIF,
  6 power, 5 MTS and one null entry. Each identifier encodes its group in
  bits 12–15 and its parameter count in bits 16–19
  (`pvr_rogue_fwif_sf.h:57-67`). The host decodes them from the trace
  buffer.
- **GPIO and breakpoints.** Three miscellaneous messages refer to a GPIO
  test interface, and command 103 sets USC breakpoints. Neither has a
  SunstoneOS use.

### The image binds to BVNC 36.56.104.183 through its device information

The information header names BVNC 36.56.104.183. The board's vendor DDK
selects the same BVNC
(`bxm-2ac08e8c…/linux/rogue_km/build/linux/sunxi_linux/Makefile`). The
device-information block tells the host which errata and features to apply.

| Kind | Firmware bits | Vendor DDK for 36.56.104.183 | Label |
| --- | --- | --- | --- |
| Errata (BRN) | 44079, 63553 | 63553, 71317 | observed `1db1c399@0x1e020`; source `rgxcore_km_36.56.104.183.h` |
| Enhancements (ERN) | 35421, 38748, 42290, 42606, 47025, 57596 | 42290, 42606, 47025, 57596 | observed `@0x1e028`; source |
| Features | 51 present, 20 absent | Every DDK feature with a firmware bit agrees in presence and value | observed `@0x1e030`; source `rgxconfig_km_36.V.104.183.h` |

Selected feature values (observed `1db1c399@0x1e040`):

| Feature | Value |
| --- | --- |
| Firmware processor | MIPS (META and RISC-V absent) |
| Clusters, raster pipes, ISP pipes | 1, 1, 6 |
| Physical and virtual address bits | 36, 40 |
| System-level cache | 16 KiB, one bank, 512-bit lines, size configurable |
| OS IDs and virtualisation | 8, present |
| Multicore support, XPU register broadcast | present, 1 |
| Compute, compute overlap, tessellation | present, present, absent |
| Tile size | 16 × 16 |
| Frame-buffer compression | FBCDC 50, algorithm 50, architecture 7 |
| Common store | 19,456 dwords |
| Firmware watchdog timer, dynamic dust power | absent, absent |

The firmware carries seven values that the DDK configuration omits, among them
the common-store size and the 12 partitions. The DDK defines two values with
no firmware bit (`FAULT_DECODE_VERSION` 0, `TFBC_VERSION` 10). The errata
lists differ: the open firmware omits BRN 71317, for which the open driver
defines no bit, and adds BRN 44079. SunstoneOS acts only on BRN 63553, the
address-zero remap ([gpu-mips.md](../gpu-mips.md)).

The vendor Linux build loads a different binary, the DDK's own `rgx.fw`
(`config_kernel.h:2`). That binary is not published, and its ABI belongs to
the DDK driver. SunstoneOS uses the open firmware with the upstream-style
driver ABI. No SunstoneOS evidence compares the two firmwares.

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
The identity values `0x1000003b` and `0x9202` were read on a board (board,
`~/.cache/sunstoneos/evidence/processor-completion-20261024/port-3-1/npu-numerical/runtime/session.json`).

### The kernel driver holds every NPU register sequence

The user libraries contain no NPU register address: the window base
`0x03600000` and the AXI SRAM states are absent from both (observed). Every
register sequence is in the kernel driver source:
[reference-backsolve-npu.md](https://github.com/pedapudi/sunstoneos/blob/work/backsolve-npu-1025/docs/reference-backsolve-npu.md)
on branch `work/backsolve-npu-1025` derives power, clocks, reset,
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
   resets ([reference-backsolve-npu.md](https://github.com/pedapudi/sunstoneos/blob/work/backsolve-npu-1025/docs/reference-backsolve-npu.md)).
   After a FEL boot (the boot ROM's USB loader mode), the boot ROM leaves the shared SRAM routed to the CPU:
   SYSCTRL `0x03000008` read 2 on both boards (board, the read-only state
   read of 2026-10-25 in [the handoff](../handoff.md)). U-Boot normally
   routes it to the NPU;
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
| GPU firmware | The pinned image, embedded at build time after a SHA-256 check ([cmake/a733-os.cmake](../../cmake/a733-os.cmake) lines 72–78 and 295) | Host side only: staging [pvr_firmware.c](../../drivers/gpu/pvr_firmware.c), MIPS tables and boot data [pvr_mips.c](../../drivers/gpu/pvr_mips.c), records [pvr_fw_state.c](../../drivers/gpu/pvr_fw_state.c), queues [pvr_ccb.c](../../drivers/gpu/pvr_ccb.c), start [pvr_gpu_start.c](../../drivers/gpu/pvr_gpu_start.c), control [pvr_gpu_control.c](../../drivers/gpu/pvr_gpu_control.c), stop [pvr_gpu_stop.c](../../drivers/gpu/pvr_gpu_stop.c), events [pvr_gpu_events.c](../../drivers/gpu/pvr_gpu_events.c) | Information footer and layout, BVNC, boot configuration, fixed record addresses, command identifiers 101, 102, 106, 107, 110, 112, 115 and 117, client types, power subtypes 1 and 2, and events 101–107, 109 and 112 |
| `libVIPhal.so`, `libNBGlinker.so` | Nothing; they run only on the Linux host as format and loader references | Controller [a733_npu_controller.c](../../drivers/npu/a733_npu_controller.c), startup [a733_npu_startup.c](../../drivers/npu/a733_npu_startup.c), tables [a733_npu_mmu.c](../../drivers/npu/a733_npu_mmu.c), memory [a733_npu_memory.c](../../drivers/npu/a733_npu_memory.c), adapter [npu_native.c](../../platform/a733/npu_native.c), loader [nbg_model.c](../../drivers/npu/nbg_model.c), [nbg_checksum.c](../../drivers/npu/nbg_checksum.c), [nbg_relocation.c](../../drivers/npu/nbg_relocation.c), [nbg_assembly.c](../../drivers/npu/nbg_assembly.c), service [npu_service.c](../../drivers/npu/npu_service.c), [npu_session.c](../../drivers/npu/npu_session.c) | Model formats `0x1001e` and `0x20000`, CRC-32, relocations, the generated prefix and tail, the register sequences of the kernel driver, and the A733 feature row |
| `nbinfo` | Nothing | [tools/inspect-nbg.py](../../tools/inspect-nbg.py) | Reports used as a reference by [checks/nbg-inspection.py](../../checks/nbg-inspection.py) |

## New insights

Each item changes or extends a SunstoneOS assumption. None has been tested on
a board.

| Finding | Affects |
| --- | --- |
| The GPU firmware writes its own page-table entries for the 1 MiB window at `0xc0ed0000`. The page-table pages are therefore firmware-written state while the firmware runs; a host rebuild or republication of the MIPS tables while the firmware runs must preserve those entries and must not write back stale cached copies. | [pvr_mips.c](../../drivers/gpu/pvr_mips.c), [gpu-mips.md](../gpu-mips.md) ("live table updates"), the MIPS-mapping rebuild in [pvr_gpu_service.c](../../drivers/gpu/pvr_gpu_service.c) |
| Every TLB refill writes the MIPS wrapper's remap-range registers. TLB misses therefore produce GPU register traffic, and the remap registers are not static after start. | [gpu-start.md](../gpu-start.md), diagnostics that read remap registers |
| When the GPU becomes idle, the firmware writes power state 3 (idle, waiting for the host) and raises the host interrupt without posting an event. The host can see interrupts with an empty event queue, and must treat state 3 as neither forced idle (2) nor off (0). | [pvr_gpu_control.c](../../drivers/gpu/pvr_gpu_control.c), [gpu-isolation.md](../gpu-isolation.md) |
| The firmware programs GPU clock gating (`CLK_CTRL`, `CLK_CTRL2`) and unit power (register `0x38`) itself after each power-up and recovery. The host does not need to program them, and SunstoneOS writes neither (no write in `drivers/gpu/`). A future host write to them would compete with the firmware. | [gpu-power.md](../gpu-power.md), [gpu-start.md](../gpu-start.md) |
| The firmware never requests a clock change (no event 108) and rejects the proactive-DVFS commands. GPU DVFS is a host decision followed by command 204. | [gpu-power.md](../gpu-power.md); event 108 decoding in [pvr_gpu_events.c](../../drivers/gpu/pvr_gpu_events.c) is unreachable |
| Commands 114, 118, 200, 207, 212, 213, 215, 216 and 220 reach the firmware's unknown-command path. In particular, register configuration (200) and firmware watchdog configuration (215) are unavailable with this image. | Future GPU features; [gpu-control.md](../gpu-control.md) |
| The open firmware's errata list (BRN 44079, 63553) differs from the vendor DDK's (63553, 71317). Any workaround taken from the DDK source for BRN 71317 has no counterpart in the open firmware's interface. | [gpu-start.md](../gpu-start.md), [gpu-power.md](../gpu-power.md) audit |
| `libNBGlinker.so` does import a register-read function and reads the identity registers. The NPU backsolve states that it imports none; its conclusion, that the libraries add no register sequence, still holds. | [reference-backsolve-npu.md](https://github.com/pedapudi/sunstoneos/blob/work/backsolve-npu-1025/docs/reference-backsolve-npu.md) |
| The vendor driver selects the feature row by product ID only; SunstoneOS also requires revision `0x9202`. The stricter check matches the board but would refuse another revision of the same product. | [a733_npu_controller.c](../../drivers/npu/a733_npu_controller.c) line 138 |
| The vendor completion event is number 4 (`0x44`); SunstoneOS uses event 0 (`0x40`) and waits for interrupt bit 0. Both are self-consistent; the choice matters only if other events share the status register. | [nbg_command.h](../../drivers/npu/nbg_command.h), [a733_npu_controller.c](../../drivers/npu/a733_npu_controller.c) |
| The A733 NPU has no tensor processor. Models that need transposes rely on neural-core transpose descriptors, which `libVIPhal.so` generates at run time; SunstoneOS's assembler has no such generator. | [nbg-assembly.md](../nbg-assembly.md), [npu-session.md](../npu-session.md) |
| The SDK's v2 sample model is for a different chip (`0x10000016`). Only the v3 samples apply to the A733. | [npu.md](../npu.md), [npu-compiler.md](../npu-compiler.md) |

## Open questions

GPU firmware:

- What register `0x38` is. The firmware writes it to request unit power
  changes and then waits on an event, but no public header names it.
- Why the firmware stores through the second register view at `0xcfa00000`.
  The boot code's TLB attributes for the two views would answer it.
- Whether active power management triggers the idle handshake with the
  reference runtime configuration (latency 0, persistent), and how often.
- Whether the firmware ever needs the 211 registers that the public headers
  do not name to hold particular values set by the host. The binary shows
  only that it writes them itself.
- How the vendor DDK firmware `rgx.fw` differs in behaviour; it is not
  published.

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

The GPU scripts in `~/.cache/sunstoneos/evidence/vendor-firmware-20261025/accel/gpu/scripts/`
run in order: `parse.py` reads a disassembly listing made with
`llvm-objdump-21 -d --mattr=+micromips,+mips32r2`, then `jt.py`, `cases.py`,
`consts.py`, `regs.py` and `funcs.py` build the tables. `tail.py` and
`devinfo.py` decode the trailing blocks. Each takes the firmware path and, where
needed, a header from the board driver cache under
`~/.cache/sunstoneos/sources/gpu/`. Run them with `python3 -I` from a scratch
directory. The register names use the board driver's `pvr_rogue_cr_defs.h`
and the DDK `rgx_cr_defs_km.h` cached under
`~/.cache/sunstoneos/evidence/gpu-wrapper-reference-20261025/`.

The NPU scripts in `~/.cache/sunstoneos/evidence/vendor-firmware-20261025/accel/npu/scripts/`
produced the NPU inventories. `gen.py` writes the four JSON files. `sizes.c`
and `layout.c` compute the driver's structure sizes and field offsets with
Clang for AArch64. The Python helpers locate constants, call sites, the
library's command-size table and its feature-table copy, and scan for
embedded blobs.
