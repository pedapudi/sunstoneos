# Design: one memory allocator for the CPU, GPU, NPU and DMA

Status: **design**, with the ledger core **implemented** and checked on the host and in QEMU (`kernel/umem/umem.c`, `kernel/umem/umem_policy.c`, `include/umem.h`). The OS image admits its large regions through the ledger, and its NPU reservation: the largest extent the NPU can address is reserved when the application is admitted, trimmed to what the runtime places, mapped in the application's tables and published for the return (`apps/os/device-ranges.h`); the NPU service's EL1 metadata comes from kernel-owned ledger chunks. The EL3 tables map all DRAM that the decode check passed. The mappings of the GPU, NPU and IOMMU spaces, the SDK calls and the other migration steps are not built. The inventory records the code as it stands before the migration; every section after the inventory specifies the end state. Board runs listed under the migration order are unscheduled.

Readers are assumed to know AArch64 stage-1 translation, the FreeRTOS SMP port and non-coherent DMA. They should also have read [memory-architecture.md](memory-architecture.md), [memory-map-12g.md](memory-map-12g.md), [application-memory-layout.md](application-memory-layout.md), [large-app-regions.md](large-app-regions.md), [dma-high-memory.md](dma-high-memory.md), [npu-resources.md](npu-resources.md) and [../memory-protection.md](../memory-protection.md).

The board has 12 GiB of LPDDR5 at `0x4000_0000`..`0x3_4000_0000`. Today four allocators and a set of fixed windows divide it: the region ledger over upper memory, the `dma32` and `uncached` device pools, the NPU pool, and the manifest tool's application window. A buffer in one of them cannot move to another, and each fixed window bounds what a device can use.

One ledger owns all DRAM above the OS image. A buffer has an owner, a size, a cache policy and a set of mappings: in the EL0 address spaces of applications, in the GPU MMU, in the NPU MMU and in the shared IOMMU tables. Buffers are allocated at run time and freed back to the ledger. Every bound is one of two kinds: the memory itself, or a hardware limit that a source or a board measurement supports. A build parameter, a window chosen for convenience, or a second fixed table is an artificial cap, and the migration removes each one.

## Inventory of fixed memory bounds

Each row gives the value, where the code sets it, the reason the code or its documents give, and whether that reason is a hardware fact (with its source) or a software choice. "Vendor driver" means the pinned board kernel `2ac08e8c` unless a path says otherwise. Rows marked **hardware** are the bounds that remain after the migration (see "What stays fixed"). Rows marked **software** go away or become computed values.

### CPU, boot map and application memory

| # | Bound | Value | Set in | Stated reason | Kind and source |
| --- | --- | --- | --- | --- | --- |
| C1 | OS code and read-only data | 2 MiB at `0x4100_0000` | `CODE` in `platform/a733-smp-cached/link.ld`, `apps/board-single-core/link.ld` | The OS and the application share the block; the OS uses 0.75 MB | Software. The entry address `0x4100_0000` comes from the boot chain; the length does not |
| C2 | OS data, bss, stacks, heap | 14 MiB at `0x4120_0000`, `ASSERT(__bss_end <= 0x42000000)` | `RAM` in `link.ld`; `__heap_end <= 0x41c00000` in `apps/os/cpu-return-host.py` | Image placement | Software |
| C3 | Monitor, result, contract, boot tables | `0x4200_0000`..`0x4220_0000`, tables at `0x421f_0000` | `MONITOR` in `link.ld` | The EL3 monitor, boot record and translation tables live there | Software, fixed by the boot chain ([application-memory-layout.md](application-memory-layout.md), "What stays fixed") |
| C4 | Local write-back device pool | 2 MiB (4 MiB with the large NPU graph) below `0x41f0_0000` | `__device_pool_size` in `cmake/a733-board.cmake` and 12 more cmake files; `ASSERT(<= 2097152)` in `link.ld`; `A733_LOCAL_DEVICE_POOL_BYTES` in `include/a733_device_memory_layout.h` | "allocation registry capacity"; pool inside image RAM | Software |
| C5 | Local uncached block | one 2 MiB block at `0x41c0_0000` | `__uncached_pool_size`, `ASSERT(== 2097152)` in `link.ld`; `a733_memory_boot_uncached()` | "never share cached and uncached aliases", so one complete level-2 block | Software |
| C6 | Extended `dma32` pool | `0x4800_0000`..`0x7800_0000`, 768 MiB | `A733_DEVICE_WB_START/END` in `include/a733_device_memory_layout.h` | Pool below 4 GiB, in 2 MiB blocks | Software |
| C7 | Extended `uncached` pool | `0x7800_0000`..`0x7900_0000`, 16 MiB, Normal non-cacheable | `A733_DEVICE_NC_START/END` | Same | Software |
| C8 | Pool ledger size | 32 allocations, 196,608 pages (768 MiB) | `DEVICE_MEMORY_MAX_ALLOCATIONS`, `DEVICE_MEMORY_MAX_PAGES` in `include/device_memory_pool.h` | Array size; "ledger size is independent" of the pool | Software |
| C9 | Boot-table DRAM end | `0x3_4000_0000` (12 GiB) | `A733_DRAM_MAP_END` in `include/a733_device_memory_layout.h` | Installed memory | Hardware: training reports 12,288 MiB on both boards ([status/memory-dma.md](../status/memory-dma.md)); the address test passes over `0x4400_0000`..`0x3_4000_0000` (record `1793600664425885881-a871e54d`) |
| C10 | EL1 map above the image is conditional | no mapping above `0x7900_0000` unless the decode check passes; the check is off by default for other cached images and always on in the OS image | `SUNSTONE_A733_DRAM_DECODE_PROBE` (default OFF) in `cmake/a733-smp-cached.cmake`; weak `a733_dram_decode_check()` returns 0 | No pinned source documents the controller and firewall registers | Software. The address-test definition passed on the board |
| C11 | Unowned hole | `0x7900_0000`..`0x8000_0000`, 112 MiB, invalid at EL1 | `platform/a733-smp-cached/memory.c`; `include/a733_device_memory_layout.h` | "no owner, and the boot0 secure region is not located" | Unproven hardware fact. The secure region has no pinned source |
| C12 | Vendor monitor reservation | `0x4800_0000`..`0x4900_0000` | Vendor `sun60iw2p1.dtsi` | Reserved for the vendor BL31 | Hardware source, but the SunstoneOS TOC1 package omits that monitor, so the range is free ([remote-writers.md](../remote-writers.md)) |
| C13 | EL0 reach through the shared table | `0x4000_0000`..`0x4800_0000`, 128 MiB | `platform/a733/platform.c` (64 blocks); `el0` in `tools/memory_layout.py`; `APP_REGION_FLOOR` in `include/app_region.h` | "DRAM above `0x48000000` is closed to EL0" in the cached profile; one boot table serves every EL0 task | Software |
| C14 | Application window | `0x4400_0000`..`0x4700_0000`, 48 MiB | `window` in `tools/memory_layout.py` | Placement window for manifest regions, GPU buffers and the NPU pool | Software |
| C15 | Slot 1 area and upper area | `0x4800_0000`..`0x7900_0000`; `0x8000_0000`..`0x3_4000_0000` | `slot1_area`, `upper_area` in `tools/memory_layout.py` | Manifest tool placement order | Software |
| C16 | Boot assets and USB arena | `0x4220_0000`..`0x4800_0000`; USB arena `0x4220_0000`..`0x42a0_0000`, 8 MiB | `include/a733_usb_memory.h` (`A733_USB_MEMORY_BASE/END`), existing manifests | Pinned addresses for runner uploads and the USB host's pool | Software |
| C17 | Region ledger granule and floor | 2 MiB extents, 12 level-1 slots, floor `0x4800_0000` | `APP_REGION_BLOCK`, `APP_REGION_SLOTS`, `APP_REGION_FLOOR` in `include/app_region.h` | Smallest extent that a block descriptor maps; the floor is the end of the shared EL0 window | Granule: software (4 KiB pages exist). Slots: hardware (level-1 entries over 12 GiB) |
| C18 | Application spaces per image | one space, 13 table pages | `arena_pages[APP_SPACE_PAGES][512]` and one `space` in `apps/os/large.c` | Admission builds the one application's space | Software |
| C19 | ASID count | 255, or 65,535 with `TCR_EL1.AS` | `app_asid_pool` in `include/app_region.h` | `ID_AA64MMFR0_EL1.ASIDBits` and `TCR_EL1.AS` | Hardware (Arm ARM D8) |
| C20 | Regions per manifest; heap | 8 regions; heap above 4 MiB comes from the ledger, at most 11 GiB | `SS_REGIONS` in `include/sunstone_app.h`; `heap_link_max` in `tools/memory_layout.py` | Manifest format; "not a limit" per the layout design | Software |
| C21 | Table arena | 16 pages (64 KiB) | [large-app-regions.md](large-app-regions.md), "Page tables and TLB reach" | Sized for 13 pages per space | Software |
| C22 | Runner reach and boot-slot load | FEL addresses below 4 GiB; slots load below `0x4700_0000` | `runner_dram`, `boot_slot_load` in `tools/memory_layout.py` | The FEL request record has 32-bit address and length (sunxi-tools `b92f4ed3`) | Hardware for the host runner only. It does not bound run-time memory |
| C23 | SD loader buffers | three 128 KiB buffers below 4 GiB | `apps/os/large_sd.c` | "no board result shows a transfer into memory above 4 GiB" | Software; the controller descriptor holds the address shifted right by 2, which would reach 2^34 |
| C24 | CPU translation | 4 KiB granule, 39-bit input range, 64-byte cache line | `A733_CPU_COHERENCE_TCR` in `include/a733_cpu_coherence.h`; `aarch64_cache_stride_from_ctr()` | 512 GiB covers 12 GiB | Hardware (Arm ARM D8; `CTR_EL0`) |

### NPU

| # | Bound | Value | Set in | Stated reason | Kind and source |
| --- | --- | --- | --- | --- | --- |
| N1 | Pool size | 8 to 48 MiB below `0x4700_0000`; 10 to 768 MiB with a base in the slot 1 area | `OS_NPU_POOL_BYTES`, `OS_NPU_POOL_BASE` in `apps/os/npu.h`; `SUNSTONE_OS_NPU_POOL_BYTES/BASE` in `cmake/sunstone-os-core.cmake`; `npu_pool` in `tools/memory_layout.py` | "the pool must be write-back RAM that EL0 reaches", which is `0x4000_0000`..`0x4800_0000` | Software. Entry C13 is the reason |
| N2 | Pool below 4 GiB | "every pool address is below 4 GiB" | [npu-resources.md](npu-resources.md), "The pool"; `a733_npu_mmu_build` | Tables sit in one 4 GiB window | Window: hardware (N8). The tensors' own addresses: software, unmeasured above 4 GiB |
| N3 | Mapped regions, registered buffers | 512 and 128 (125 usable) | `A733_NPU_MMU_MAX_REGIONS` in `include/a733_npu_mmu.h`; `A733_NPU_MEMORY_MAX_BUFFERS` in `include/a733_npu_memory.h` | Array sizes of the builder and memory layer | Software |
| N4 | Table arena | 4,198,400 bytes (4096 + 256 × 16,384) | `A733_NPU_MMU_MAX_BYTES` | 256 directory entries, one 16 KiB leaf table per 16 MiB at 4 KiB pages | Hardware-derived maximum; real need is computed from the mappings |
| N5 | Device address layout | regions from `0x1000_0000`, 2 MiB groups, a guard page after each | `NPU_RUNTIME_DEVICE_BASE`, `NPU_RUNTIME_GROUP_BYTES` in `include/npu_runtime.h` | Placement policy | Software. The 32-bit span is hardware (N8) |
| N6 | Command allocation per slot | 524,288 bytes when graphs are registered later | `NPU_RUNTIME_COMMAND_BYTES` | A later graph's stream length is unknown | Software. The fetch size `A733_NPU_FETCH_BYTES` (524,280) is hardware (N8) |
| N7 | Model container | 256 MiB | `NBG_MODEL_MAX_BYTES` in `include/nbg_model.h`; `MAX_FILE_BYTES` in `tools/inspect-nbg.py` | The loader's own choice; "the format does not bound" it | Software |
| N8 | NPU MMU geometry | 32-bit device address; 40-bit physical; 256 directory entries of 16 MiB; one page size per 16 MiB (4 KiB, 64 KiB, 1 MiB, 16 MiB); directory and leaf tables share physical bits 39:32; device addresses below `0x10_0000` unmapped and the SRAM window `0x40_0000`..`0x48_0000` unmapped; command fetch at most 65,535 × 8 bytes | `drivers/npu/a733_npu_mmu.c` (`PA_LIMIT`, `VA_LIMIT`, `BLOCK_BYTES`); `A733_NPU_MMU_LOW_RESERVED` | Vendor driver `vip2/memory/vip_drv_mmu.c` (page encoding, same-upper-byte constraint, SRAM window at lines 721-729); `vipdMAX_CMD_SIZE` | Hardware, from the vendor driver. Physical addresses above 4 GiB are unproven on the board |
| N9 | Per-service fixed buffers | convolution service: command 524,288, input 524,288, output 262,144, model 8,192 or 1 MiB, I/O 512, table 20,480 bytes; session bootstrap 524,288, 8 models | `NPU_CONVOLUTION_*` in `include/npu_convolution.h`, `include/npu_convolution_layout.h`; `NPU_SESSION_*` in `include/npu_session.h` | Layout of the trusted first convolution and the finite session | Software |
| N10 | Tensor size in the SDK | 1 MiB per tensor | `SS_NPU_MAX_TENSOR_BYTES` in `include/sunstone_app.h` | `ss_npu_run` copies tensors in and out | Software |
| N11 | Clients, slots, handles, tensors | 31, 31, 31, 8 | `NPU_SERVICE_CLIENTS`, `SS_NPU_QUEUE_MAX_HANDLES`, `SS_NPU_QUEUE_MAX_TENSORS`, `NBG_ASSEMBLY_MAX_TENSORS` | 32-bit command mask; array sizes | Software. Another stream (`work/npu-nocaps-1103`) removes these |

### GPU

| # | Bound | Value | Set in | Stated reason | Kind and source |
| --- | --- | --- | --- | --- | --- |
| G1 | Firmware reservations | 34 write-back pages (100 KiB code, 20 KiB data, 16 KiB MIPS tables) and 45 uncached pages (19 shared records) | `A733_GPU_RESERVED_WB_BYTES`, `A733_GPU_RESERVED_NC_BYTES` in `include/a733_gpu_reserved.h` | The pinned firmware image's allocation profile | The sizes are firmware facts; the constants duplicate what `pvr_fw_requirements` computes. Software |
| G2 | Compute payload and tables | 88 payload pages; 256 table pages; a client buffer of at most 64 pages | `A733_GPU_COMPUTE_PAYLOAD_PAGES`; `PVR_COMPUTE_SERVICE_TABLE_PAGES` in `include/pvr_compute_service.h`; `pvr_compute_service.c` | Fixed evidence workload | Software |
| G3 | Graphics tables and maps | 35 table pages; `PVR_RENDER_REGIONS + 10` maps | `A733_GPU_GRAPHICS_TABLE_PAGES`, `A733_GPU_GRAPHICS_MAX_MAPS` in `include/a733_gpu_graphics_memory.h` | Fixed render profile | Software |
| G4 | Texture size | 1920 × 1080 | `PVR_RENDER_TEXTURE_MAX_*` in `include/pvr_render_texture.h` | Service profile | Software |
| G5 | Session storage in image RAM | CPU address in `0x4100_0000`..`__device_pool_start` | `private_image()` in `platform/a733/gpu_reserved.c`, `gpu_compute_reserved.c`, `gpu_graphics_memory.c` | Identity-mapped image storage excludes aliases into the pools | Software |
| G6 | Firmware image and allocation | 4 MiB image, 16 MiB allocation | `PVR_FW_MAX_IMAGE`, `PVR_FW_MAX_ALLOCATION` in `include/pvr_firmware.h` | Parser limits | Software |
| G7 | GPU MMU geometry | 40-bit virtual, 36-bit physical, catalogue of 1,024 entries, page sizes 4, 16, 64 and 256 KiB, one page size per 2 MiB region; table pages at most 1 + 1,024 + 524,288 | `PA_LIMIT`, `MAX_TABLE_PAGES` in `platform/a733/gpu_vm_memory.c`; `size_code()` in `drivers/gpu/pvr_gpu_vm.c`; `include/pvr_gpu_vm.h` | The firmware feature record states the widths; erratum BRN 71317 excludes 1 MiB and 2 MiB data pages | Hardware, from the pinned firmware feature record and the vendor erratum list. Physical identity is unverified on the board |
| G8 | Firmware virtual layout | PDS and USC heaps 4 GiB each at `0xda_0000_0000` and `0xe0_0000_0000`; general heap 128 GiB at `0x80_0000_0000`; firmware base `0xc090_0000`, `0xc0a0_0000` | `heap_base[]` in `platform/a733/gpu_compute_reserved.c`; `FW_BASE`, `GPU_BASE`, `TABLE_BASE` in `gpu_graphics_memory.c` | "the pinned native heap profile" | Hardware (firmware profile); virtual only, so it does not bound physical memory |
| G9 | Excluded physical range | `0x1fc0_0000`..`0x1fc0_4000` | `pvr_mips.c`, `gpu_memory.c`, `gpu_vm_memory.c`, `gpu_backend.c` | "the full sensitive 16 KiB range described by the reference" | Hardware, below DRAM; no DRAM buffer meets it |

### DMA and IOMMU masters

| # | Bound | Value | Set in | Stated reason | Kind and source |
| --- | --- | --- | --- | --- | --- |
| D1 | DMA0 address width | data below 2^35; descriptors below `0xffff_f800` | `A733_DMA0_DATA_LIMIT`, `A733_DMA0_DESCRIPTOR_LIMIT` in `include/dma_address_width.h` | Descriptor parameter bits 11-13 and 15-17 carry address bits 34:32; the link word ends a chain at `0xffff_f800` | Hardware (A733 manual and vendor `sunxi-dma.c`); the high-address encoding is unverified on the board |
| D2 | DMA0 job shape | 64 segments, 1 MiB per segment, 16 windows, 64 ports | `A733_DMA_MAX_SEGMENTS`, `A733_DMA_MAX_SEGMENT_BYTES` in `include/a733_dma.h`; `DMA_MEMORY_MAX_WINDOWS`, `DMA_MEMORY_MAX_PORTS` in `include/dma_memory.h` | "Software policy pending physical length qualification; not a silicon limit" (segment size) | Software |
| D3 | IOMMU geometry | two banks, one directory each (4,096 entries, 16 KiB), 1,024-entry leaf tables, 4 KiB pages only, IOVA 34 bits, physical bits 33:32 in entry bits 9:8 | `drivers/iommu/a733_iommu.c`; `A733_IOMMU_PHYSICAL_LIMIT` in `include/dma_address_width.h` | Vendor `sunxi-iommu-pgtable.c`, `sunxi-iommu-v2.c`; A733 manual p. 406 (two banks, 16 GiB address spaces) | Hardware |
| D4 | IOMMU builder shape | 8 spans; 4 windows of 4 MiB in 32 KiB (5 in 36 KiB) | `A733_IOMMU_MAX_SPANS`, `A733_IOMMU_L2_TABLES`, `A733_IOMMU_MAX_L2_TABLES`, `A733_IOMMU_TABLE_BYTES` in `include/a733_iommu.h` | Fixed offline mappings; no live table changes | Software |
| D5 | IOVA policy | below 4 GiB | `A733_IOMMU_IOVA_LIMIT` | Camera route parameters hold the address shifted right by 2, below 4 GiB | Software for most masters; hardware (vendor `vin` source) for the camera route |
| D6 | Camera buffers | fixed IOVA `0x1000_0000`; 663,552-byte allocation; 4 KiB guard | `A733_CAMERA_NATIVE_IOVA`, `A733_CAMERA_NATIVE_*` in `include/a733_camera_native.h`, `include/a733_camera_profile.h` | Fixed capture profile | Software |
| D7 | xHCI | 131,072 bytes below 4 GiB unless `HCCPARAMS1.AC64` is 1 | `A733_XHCI_MEMORY_BYTES` in `include/a733_xhci.h` | AC64 unread on this part | Hardware (xHCI 1.2, 5.3.6); value unread |
| D8 | PCIe endpoint DMA | below 4 GiB; NVMe pool of six pages | `DMA_ADDRESS_32BIT_LIMIT`; `a733_pcie_nvme_init` | The root port's inbound width is unpublished | Unknown hardware fact |
| D9 | Crypto engine | below 2^40 | `drivers/crypto/a733_ce.c:19` | Vendor driver | Hardware |
| D10 | Codec and display profiles | H.264 input 8 KiB, 512 KiB or 4 MiB; slot and motion-vector sizes; G2D result 192 bytes | `A733_H264_*` in `include/a733_video_h264.h`, `A733_G2D_*` | Fixed service profiles | Software |

### Which bounds are hardware facts

The inventory lists 24 CPU and boot rows, 11 NPU rows, 9 GPU rows and 10 DMA and IOMMU rows, 54 in all.

- **Hardware facts or hardware-derived, 14:** C9, C12, C19, C22, C24, N4, N8, G7, G8, G9, D1, D3, D7 and D9.
- **Hardware facts not yet known, 2:** C11 and D8. The open questions below turn them into measurements.
- **A hardware bound mixed with a software choice, 6:** C17, N2, N5, N6, G1 and D5.
- **Software choices, 32:** the remaining rows.

## The allocator

### One ledger covers all DRAM above the OS image

The ledger owns every byte from `0x4220_0000` (the end of the OS image, monitor and tables) to the end of installed memory, except ranges that a boot check excludes. The usable ranges are the ones the decode check passes, extended down to `0x4220_0000`: `0x4220_0000`..`0x7900_0000` and `0x8000_0000`..`0x3_4000_0000`. The 112 MiB hole stays excluded until a board measurement locates the secure region (open question 9). Below `0x4220_0000` the linker owns the OS image.

The region ledger of `kernel/appmem/app_region.c` is the starting point. It keeps the properties that the board qualified: extents of one owner, the 1 GiB placement preference for large regions, release with refusal for non-owners, and `app_region_owner_of()`. It gains these properties:

- **Page granularity.** An extent is a multiple of 4 KiB, the smallest unit that every consumer maps (CPU, GPU, NPU and IOMMU all use 4 KiB pages). The 2 MiB block stays the CPU mapping unit for extents that are 2 MiB aligned and 2 MiB long; smaller extents get level-3 tables.
- **Run-time growth.** Entry storage grows by carving 4 KiB pages from the ledger itself, owned by the kernel and mapped in no application space. The entry count is bounded by memory: at 88 bytes an entry, 3,145,728 buffers of 4 KiB would use 264 MiB, and measured use with 40,000 live buffers of up to 128 KiB is 5.4 MiB. No entry table is sized from manifests.
- **Ordered index.** Extents sit in one treap ordered by address, and free extents also in a list per policy and size class. Lookup, insertion and removal take a logarithmic number of steps. A request with no constraint takes the first extent of the next larger size class, so it visits few extents: with 40,000 live buffers the 99th percentile is 79 steps and the worst 178. A request with an alignment, window, physical limit or non-default policy visits every free extent of the classes it can fit, so its worst case grows with fragmentation (5,036 steps measured). A release costs 87 steps on average and 272 at worst.
- **Constraints per request.** A request names a size, an alignment, a physical window (`phys_max`, the exclusive upper bound, and optionally a naturally aligned window that the extent may not cross) and a cache policy. Placement is first fit among the extents of the next larger size class (the request's own class last, or first with `UMEM_REUSE`), and aligned to the largest device page of the devices in the request. A request of 1 GiB or more is placed on a 1 GiB boundary when one is free.
- **Owners.** An owner is an application space (ASID), the kernel, or a service. The kernel owner keeps its extents out of every application space, as the NPU pool does today.

### A buffer record

| Field | Content |
| --- | --- |
| `base`, `bytes` | Physical extent; the CPU address of every mapping equals the physical address, as in the region ledger |
| `owner` | The application, kernel or service that holds the buffer |
| `policy` | `WB` (Normal write-back, inner shareable, MAIR index 1) or `NC` (Normal non-cacheable, MAIR index 2, which gathers writes) |
| `state` | `CPU`, `DEVICE` or `RETAINED`, with the holder rules of [memory-architecture.md](memory-architecture.md) |
| `dirty` | The extent may hold dirty cache lines |
| `generation` | Published with release order at allocation; a lock-free describe reads it before and after copying the record, as `device_memory_describe()` does |
| `maps` | The mapping set: for each space, its address, protection and generation |

Free extents carry `dirty` as well. Adjacent free extents merge and OR their flags.

### Cache policy applies to every mapping of a buffer

A buffer has one policy, and every CPU mapping of it, kernel and EL0, uses that policy's attribute. Mixed-attribute aliases of one physical page do not arise.

The kernel's identity map of DRAM is write-back. An `NC` buffer needs the entry for its 2 MiB chunk to be non-cacheable in every table that maps the chunk, and that includes the EL1-only entries that each application root carries for memory the application does not own: kernel code that runs in an application's task uses that root. The ledger keeps one policy byte per 2 MiB chunk (6,144 bytes for 12 GiB) and allocates `NC` buffers only from `NC` chunks. A free chunk converts when a request needs the other policy (`umem_policy_convert`):

1. The ledger takes the chunk out of the free lists under its lock; no allocation uses it.
2. The conversion clears the valid bit of the chunk's entry in every table that maps it: the kernel's level-2 table, each application's private level-2 table and each split gigabyte's table. It executes `DSB ISH`, `TLBI VAAE1IS` (all ASIDs) and `DSB ISH`, and `ISB`. No entry maps the chunk now, in a table or in a TLB.
3. It writes the same descriptors with the new attribute index, executes `DSB ISH` and `ISB`.
4. It runs `DC IVAC` over the chunk through the new mapping. The chunk is free, so its contents are dead and the invalidate discards nothing of value, and it removes any line that an access before step 2 allocated under the old attribute.
5. The chunk joins the free space of its new policy.

Between steps 2 and 3 no mapping exists, so write-back and non-cacheable translations of one page never coexist. A host model with a TLB and speculative accesses between every instruction checks this, and a variant without the TLBI is flagged.

A gigabyte of upper memory is one level-1 block until it is split into a level-2 table of 2 MiB blocks. The split is a boot step, taken before any application root copies the level-1 table; the policy layer refuses a split, and so a conversion in an unsplit gigabyte, once an application root exists. An `NC` request after admission therefore draws on chunks that boot converted, on slot 1, or on a gigabyte that boot split. Making application spaces share the split tables, so that any gigabyte converts at run time, belongs to step 3, where the device pools first need `NC` memory. The conversion runs at allocation time, never on a job path, and an `NC` chunk stays `NC` while it is free until a request needs `WB` memory and no `WB` chunk fits.

Policy follows use. CPU-written, device-read data that the CPU rereads (weights, tensors, command buffers) is `WB` with a clean at hand-off. Descriptor rings, firmware queues, scanout buffers and any buffer that a device writes while the CPU polls are `NC`. A write-combining CPU mapping is the `NC` attribute on this core family; no further attribute is used.

### Allocation returns zeroed, clean memory

Every allocation of a `WB` extent returns memory that holds no dirty line, and every allocation returns zeroed memory when the owner changes. The ledger zeroes with `DC ZVA` (or stores on an `NC` extent), then cleans the extent when `dirty` was set or the zeroing dirtied it. The work runs on an EL1 memory worker in steps of at most 80 µs, the step size the NPU runtime already uses for pool zeroing. `ss_mem_alloc` returns a handle at once; the buffer becomes ready when the worker finishes. A same-owner reallocation skips zeroing and still cleans a `dirty` extent. This is the zeroed acquire that [memory-architecture.md](memory-architecture.md) lists as open work.

### Free is deferred until every device has drained

`umem_free` refuses while any device mapping is live. A device mapping ends only after its service has drained the device (the service's existing stop or completion protocol) and the space's translation cache is invalidated. CPU mappings end with break-before-make and `TLBI` by ASID and address. Only then does the extent become free, with `dirty` set if it had a writable CPU mapping.

### Concurrency

Acquire, release and the policy conversion take one ledger lock, the kernel critical section of [memory-architecture.md](memory-architecture.md). Table edits take a per-space lock and run outside the ledger lock. No interrupt handler calls the ledger. The ledger's metadata lives in kernel-owned memory that no application table maps, so EL0 code cannot corrupt ownership; the lock words in the image's `.bss` stay EL0-writable, as the memory architecture records.

## Mappings

Each address-space kind implements one interface: `map`, `unmap`, `flush` and `quiesced`. `map` takes a buffer extent, a protection and an optional address and returns the address in that space. `flush` invalidates the space's translations for a range. `unmap` is legal only after `quiesced` returns true for the space.

### CPU: the EL0 address space of one or more applications

`app_space_build_l2()` and the ASID-tagged, non-global tables stay. They change in four ways: the floor drops from `0x4800_0000` to the end of the OS image (`0x4220_0000`); a buffer smaller than 2 MiB or not 2 MiB aligned maps with level-3 pages; a space accepts mappings after the application starts; and a buffer may be mapped in several spaces. A mapping added after start uses the break-before-make sequence that [large-app-regions.md](large-app-regions.md) specifies: write invalid, `DSB ISH`, `TLBI ASIDE1IS` (or `VAE1IS` for one address), `DSB ISH`, write the entry, `DSB ISH`, `ISB`. A mapping that adds an entry to an invalid slot needs only the final barriers. The table arena grows from the ledger: a space needs one root page, one level-2 page per touched gigabyte and one level-3 page per touched 2 MiB, all taken as kernel-owned buffers. The cost is at most 4 KiB per 2 MiB of small-buffer mappings and nothing for mappings of 2 MiB blocks or 1 GiB blocks.

### NPU: one directory, 32-bit device addresses

The NPU MMU has one directory, a 32-bit device address space of 256 sixteen-MiB ranges, and 40-bit physical addresses. `a733_npu_mmu_build()` builds the directory and leaf tables from a list of regions. In this design the NPU service owns one **device space** per application, built from the mappings the ledger records for that application.

- **Device addresses.** A device-address allocator hands out ranges from `0x10_0000` (above the unmapped low range, skipping the SRAM window at `0x40_0000`..`0x48_0000`) to the end of the 32-bit span, with a guard page after each buffer. A 16 MiB range holds one page size. The allocator opens a range on the first buffer that needs a page size and fills it with buffers of that size. A buffer whose physical start is 16 MiB aligned and whose length is a multiple of 16 MiB uses 16 MiB pages; 1 MiB alignment uses 1 MiB pages; 64 KiB alignment uses 64 KiB pages; anything else uses 4 KiB pages.
- **Placement help.** The ledger aligns an NPU-flagged request of at least 16 MiB to 16 MiB, of at least 1 MiB to 1 MiB, of at least 64 KiB to 64 KiB, and otherwise to 4 KiB, which keeps table memory small. A 1 GiB tensor then needs 64 directory entries with a 64-byte leaf table each (4 KiB of leaf tables), where 4 KiB pages need 64 leaf tables of 16 KiB (1 MiB).
- **Table memory.** The directory and every leaf table must share physical bits 39:32. The service requests its table extent with a 4 GiB window constraint and the size that the mapping list computes (`a733_npu_mmu_build` reports it). The page-directory register takes the table address shifted right by 12, shifted left by 4, with bit 0 set.
- **Containment.** The directory that the NPU runs holds only the buffers mapped for the application whose job runs. When consecutive jobs belong to different applications, the service writes the other application's directory into the page-directory register (`+0x3b4`) between jobs, with the NPU idle, and flushes the MMU. Jobs of one application's queue never change the directory. Open question 6 measures the switch.
- **Shared tensors.** A producer's output and a consumer's input that are the same buffer map in the same space at one device address.

### GPU: per-context page tables

`pvr_gpu_vm_build()` builds a catalogue (root), directories and leaves from sorted, physically contiguous spans; the firmware context record names the root. Each GPU context (session) has its own tables. The GPU data heap is 128 GiB of virtual space at `0x80_0000_0000`, which exceeds installed memory, so no GPU virtual allocator bound reaches the memory limit. Buffers are mapped with the largest page that their physical and virtual alignment admits, up to 256 KiB. The ledger aligns a GPU-flagged request of at least 256 KiB to 256 KiB. Table memory comes from the ledger with `phys_max` 2^36. The `PVR_GPU_VM_COHERENT` flag stays clear, because no measurement shows that it establishes coherence. A change to a live context follows the translation-cache invalidation protocol of [gpu-flush.md](../gpu-flush.md), which has host evidence only.

Firmware objects (code, data, MIPS tables, shared records) keep their own rules: MIPS tables map the firmware's pages by the remap registers, queues and records are `NC`, and code and data are `WB`. Their sizes come from `pvr_fw_requirements` of the loaded firmware image.

### DMA: the shared IOMMU tables and DMA0

Display, G2D, the deinterlacer, the video decoders and encoder, the camera and the ISP translate through the system IOMMU. The IOMMU has two banks with one directory each. The design gives each bank one device space: bank 0 holds USB, CSI, ISP and the encoder; bank 1 holds the decoders, display, deinterlacer, G2D and the e-paper engine. Masters in one bank share a space, so containment between masters of one bank is by mapping lifetime only: a master sees a buffer from `map` until `unmap`, and services unmap after the job completes. An IOVA allocator hands out ranges from `0x1000` up to the master's measured IOVA width. The table builder takes a variable number of spans and 4 MiB windows; its tables come from the ledger with `phys_max` 2^34 and 16 KiB alignment. The camera's fixed IOVA becomes an allocator result.

DMA0 addresses memory directly. Its descriptor builder takes the physical extents of the buffers and the descriptor memory, which the ledger places below `0xffff_f800`. Segment count and segment length derive from the descriptor memory and the controller's measured limits instead of the constants 64 and 1 MiB.

The USB host, PCIe and SD paths keep the width rules of D7, D8 and C23 until their measurements pass; their buffers carry `phys_max` 4 GiB in the request.

### Each device has one measured width

One table replaces the scattered width constants. Each row has a device, the physical width with its source, a flag that says whether the board has run a transfer above 4 GiB, and the page sizes. The allocator computes a request's `phys_max` as the minimum over the devices in the request's device mask. A row that no board run has confirmed above 4 GiB carries 4 GiB as its working limit, and its source width as the hardware limit; a confirming run raises the working limit to the source width. DRAM ends at `0x3_4000_0000`, below 2^34, so every device whose width is at least 34 bits (GPU, NPU, IOMMU masters, DMA0, crypto) is unrestricted by width once confirmed.

| Device | Source width | Working limit today |
| --- | --- | --- |
| NPU | 2^40 | 4 GiB |
| GPU data | 2^36 | 4 GiB |
| GPU firmware objects | 2^36 | 4 GiB |
| IOMMU masters | 2^34 | 4 GiB |
| DMA0 data | 2^35 | 4 GiB |
| DMA0 descriptors | `0xffff_f800` | `0xffff_f800` |
| Crypto engine | 2^40 | 4 GiB |
| xHCI | 2^34 if AC64, else 4 GiB | 4 GiB |
| PCIe endpoint DMA | unpublished | 4 GiB |
| SD DMA | unpublished | 4 GiB |

## Containment

**Devices.** A device sees only the buffers mapped into its tables. Every leaf entry of an NPU, GPU or IOMMU table derives from a mapping record whose buffer belongs to the space's owner, and a table builder refuses a span that no record names. The check that matters is the table walk: a host check interprets each generated table with the existing walkers (`checks/npu-mmu.c`, the GPU table walker, `a733_iommu_translate`) and compares the set of reachable physical pages with the union of the space's mapping records. The tables, command streams and descriptors of a space are also buffers in the ledger, owned by the kernel, mapped in no device space except where the device must read them (the command fetch), and read-only there.

**Graphs and command streams run as compiled.** The services validate structure and addresses at admission, as the NPU loader's relocation and the GPU service do today, and they never rewrite a stream's access pattern. Out-of-range device accesses fault in the MMU; the NPU fault path (`ss_npu_last_fault`) and the GPU fault capture report the address.

**EL0.** EL0 reaches only what its tables map. The shared boot table shrinks to the OS image, `0x4000_0000`..`0x4220_0000`: OS code, data, heap and the session record, which the SDK calls reach as plain function calls. Every other byte of DRAM is a ledger buffer, and a buffer is mapped at EL0 in the spaces of its owner and of the applications the owner grants. This replaces the cached-profile rule "DRAM above `0x4800_0000` is closed to EL0". The old rule exists because one table serves every EL0 task; per-application tables already map upper memory, so the rule becomes a property of the tables and needs no address threshold. A byte outside the application's mappings, including another application's buffer, a device buffer and the ledger's metadata, takes a permission or translation fault at EL0. The OS image stays EL0-readable and writable by design: SDK calls are plain function calls, and no system-call gate is planned (see Limitations).

**Checks stay at EL1.** The ledger, the table builders, cache maintenance and every device register write run at EL1. EL0 holds handles and mapped pointers only. Handles carry a generation, so a stale handle is refused after free.

**Sharing.** An application shares a buffer with another by naming the peer; the OS maps it into the peer's space only when the peer's manifest declares the same channel. A refused share changes nothing.

## Cache coherence and maintenance

The drivers treat every device as non-coherent: the CPU cleans what a device reads and cleans and invalidates what a device wrote. No board measurement shows that any master snoops the CPU caches (open question 7). All CPU cores are inner-shareable coherent with each other, so a buffer shared between cores needs no maintenance.

| Device | Reads | Writes | Maintenance in the existing driver |
| --- | --- | --- | --- |
| CPU, all cores | coherent | coherent | None between cores. `aarch64_device_cache()` runs only at EL1 and ends with `DSB SY` |
| NPU | MMU-translated, direct DRAM | MMU-translated, direct DRAM | Publication cleans the tables and every device-readable allocation, and cleans and invalidates every device-written allocation, then a barrier before submit. Completion invalidates the written allocations of the finished command's group only, so one session's completion cannot discard another client's dirty lines ([npu-memory.md](../npu-memory.md)) |
| GPU | MMU-translated | MMU-translated | Class (a) input: clean written bytes at submit. Class (b) output: invalidate the bytes the client reads at completion; clean and invalidate at submit only when the client wrote it. Class (c) device-only: clean and invalidate once at open. Firmware queues in `NC`: barriers only. Page tables: cleaned once after construction ([gpu-runtime.md](gpu-runtime.md)) |
| DMA0 | direct | direct | Clean before the device reads; clean and invalidate over the written range after completion; the callback table maps `enum a733_dma_cache` onto `enum aarch64_cache_op` |
| IOMMU masters | translated | translated | The pool rule: clean before a device read, `DMB OSHLD` then clean and invalidate before a CPU read of device output; the IOMMU's own tables are cleaned before the directory base is written |
| `NC` buffers | any | any | `DMB OSHST` before a doorbell, `DMB OSHLD` after completion; no cache operation |

The ledger generalizes the existing hand-off rule to every device. `ss_mem_sync` and the services call one routine with a buffer, a range set and a transition:

- CPU to device: clean the write set (`WB` only).
- Device to CPU: `DMB OSHLD`, then clean and invalidate the write set (`WB` only).
- Device to device: no maintenance, because the CPU holds no dirty line while the buffer is in `DEVICE` state.

A hand-off that moves a `WB` buffer from device to CPU while a mapping in another EL0 space is writable is refused; one space writes at a time. The clean granule is the 64-byte line, and every extent starts on a 4 KiB boundary, so two buffers never share a line.

Hand-offs on the submit and completion paths of the NPU, GPU and DMA run in the service (an EL1 worker or the completion interrupt), driven by the ranges that the ring entry names, so the application enters the kernel for none of them. `ss_mem_sync` serves buffers that no ring carries; it enters EL1, because `DC IVAC` is EL1-only. Whether EL0 may issue `DC CVAC` and `DC CIVAC` directly (`SCTLR_EL1.UCI`) is a source check that decides whether a clean needs the call (open question 10).

Large `WB` buffers cost a clean per line (a 1 GiB clean is predicted at no more than 80 M A76 cycles in [memory-architecture.md](memory-architecture.md); the board run that fixes the figure is queued under open work there). A producer that writes a buffer once and a device that reads it many times pays the clean once. A producer that streams a buffer every frame and reads none of it takes `NC` instead.

## The SDK interface

The SDK offers a POSIX-like allocate, map and free. All calls are plain functions safe at EL0. The `ss_mem_*` calls may enter EL1, because they build tables, zero memory and maintain caches. The hot paths enter no kernel code: submit and complete for the NPU, GPU and DMA go through rings in shared memory, and the kernel is entered only to sleep when a ring is empty or full and to wake the other side. Names follow `ss_*`; each returns zero or a positive value on success and a negative `errno` value otherwise: `-ENOMEM` (no extent), `-EINVAL`, `-EBUSY` (a device mapping or a pending operation), `-EPERM` (not the owner), `-ENODEV` (no such device).

```c
typedef struct { uint32_t index, generation; } ss_mem;

enum {
    SS_MEM_WRITE_COMBINE = 1u << 0,   /* policy NC; default is write-back */
    SS_MEM_SHARED_CPU    = 1u << 1,   /* will be mapped by more than one application */
    SS_MEM_REUSE         = 1u << 2,   /* same-owner reuse: skip zeroing; cleaning still applies */
    SS_MEM_DEV_NPU       = 1u << 8,   /* devices that will map the buffer: width, alignment and policy follow */
    SS_MEM_DEV_GPU       = 1u << 9,
    SS_MEM_DEV_DMA       = 1u << 10,
    SS_MEM_DEV_DISPLAY   = 1u << 11,
    SS_MEM_DEV_CAMERA    = 1u << 12,
    SS_MEM_DEV_CODEC     = 1u << 13,
    SS_MEM_DEV_USB       = 1u << 14,
    SS_MEM_DEV_PCIE      = 1u << 15
};
enum { SS_MEM_READ = 1, SS_MEM_WRITE = 2 };
enum ss_mem_direction { SS_MEM_TO_DEVICE, SS_MEM_FROM_DEVICE };

/* Reserve bytes (rounded to 4 KiB). Returns a handle at once; the buffer is ready when ss_mem_wait returns 1. */
int   ss_mem_alloc(size_t bytes, uint32_t flags, ss_mem *out);
int   ss_mem_wait(ss_mem, uint64_t timeout_us);       /* 1 ready, 0 timeout, negative failure */
void *ss_mem_map(ss_mem, unsigned prot);               /* CPU address in the caller's space, NULL on failure */
int   ss_mem_unmap(ss_mem);
int   ss_mem_sync(ss_mem, size_t offset, size_t bytes, enum ss_mem_direction);
int   ss_mem_grant(ss_mem, const char *peer_channel, unsigned prot);
int   ss_mem_free(ss_mem);                             /* -EBUSY while a device mapping lives */
int   ss_mem_info(ss_mem, struct ss_mem_info *);       /* size, policy, state, device mappings */
```

A device flag is a constraint and a promise: the ledger applies the device's width, alignment and policy rules, and `ss_mem_alloc` refuses a combination that no device can honor (for example, `SS_MEM_DEV_NPU` with `SS_MEM_DEV_USB` and a size that does not fit below 4 GiB). Mapping into a device is not an application call. The services map a buffer when the application binds it:

- **NPU queue.** `ss_npu_queue_bind(queue, handle, kind, index, ss_mem, offset, bytes)` binds a buffer to a tensor of a prepared graph, at open and not on the submit path. `ss_npu_queue_tensor()` returns a pointer into a buffer that the queue allocated from the ledger for that application, so unbound tensors still live in application-owned memory mapped at EL0. The first submit maps the buffer in the application's NPU space and transfers the buffer's device state; connected handles share one buffer. The `ss_npu_run` copy path and its 1 MiB tensor limit go away.
- **GPU service.** A client registers a buffer handle at open instead of a pointer. The worker maps the buffer into the client's GPU context and no longer copies it, because the buffer is mapped at EL0 in the client's space already.
- **DMA.** `ss_dma_submit(src, dst, bytes)` takes two handles and publishes a ring entry. The service builds DMA0 descriptors from their physical extents, or maps them into a bank's IOMMU space for a master that translates. The fixed 4 KiB catalogue of `ss_dma_open` goes away.

The kernel interface that services call, and that `work/npu-nocaps-1103` takes its tables from, is the same ledger with owner, space and device arguments:

```c
struct umem_request {
    uint64_t bytes, align;
    uint64_t phys_max;          /* exclusive; 0 = installed memory */
    uint64_t window;            /* extent must not cross a window-aligned boundary; 0 = none */
    uint32_t policy;            /* UMEM_WB or UMEM_NC */
    uint32_t device_mask;       /* widths, page-size alignment and policy derive from the mask */
    uint16_t owner;
    uint16_t flags;             /* UMEM_AT (use base), UMEM_REUSE, UMEM_NO_CONVERT, UMEM_RAW (ready at once, contents kept) */
    uint64_t base;              /* with UMEM_AT: pinned address */
};
struct umem_buf { uint64_t base, bytes; uint32_t index, generation; };

int  umem_alloc(struct umem *, const struct umem_request *, struct umem_buf *out);   /* async: ready callback or poll */
int  umem_free(struct umem *, struct umem_buf);                                       /* refuses live device mappings */
int  umem_describe(const struct umem *, struct umem_buf, struct umem_extent *, int *state);   /* lock-free */
int  umem_handoff(struct umem *, struct umem_buf, int from, int to, const struct umem_range *set, unsigned count);

struct umem_space_ops {          /* one per address-space kind: CPU, GPU context, NPU directory, IOMMU bank */
    int (*map)(void *space, struct umem_buf, uint64_t offset, uint64_t bytes, unsigned prot, uint64_t *address);
    int (*unmap)(void *space, uint64_t address, uint64_t bytes);
    int (*flush)(void *space, uint64_t address, uint64_t bytes);
    int (*quiesced)(void *space);
};
int umem_map(struct umem *, struct umem_buf, const struct umem_space_ops *, void *space, unsigned prot, uint64_t *address);
int umem_unmap(struct umem *, struct umem_buf, const struct umem_space_ops *, void *space, uint64_t address);
```

The NPU stream takes four things from it:

- The pool becomes a set of `umem_alloc` calls at admission, sized by `npu_runtime_reservation`.
- The table arena becomes one `umem_alloc` with `window` 4 GiB and the computed size.
- Each region and tensor becomes a `umem_map` into the application's NPU space.
- The slot, region and buffer counts size their arrays from the declaration at start, and the arrays come from the ledger.

 No array bound in `A733_NPU_MMU_MAX_REGIONS` or `A733_NPU_MEMORY_MAX_BUFFERS` remains; `a733_npu_mmu_build` takes the count from the caller and returns the table bytes it needs.

## Migration order

Each step removes a family of bounds, ends with host checks that prove the step and one board run that confirms it, and leaves the tree building and every earlier check passing. The predictions below are the pass conditions. Before a run is queued, its evidence record under `docs/evidence/` (`tools/evidence.py add`) copies the prediction; the record also holds the queue entry and the result.

A run that returns a different result changes the device-width table of "Each device has one measured width" to the measured value and changes nothing else; the step's host checks still stand.

### Step 1: the NPU pool

**Scope.** The ledger gains page granularity, run-time growth, the ordered index, constraints and the `umem_*` interface (the ledger core, shared by all steps). The NPU service takes its pool, tables and tensors from it. `OS_NPU_POOL_BYTES`, `OS_NPU_POOL_BASE`, `SUNSTONE_OS_NPU_POOL_*` and the `npu_pool` record are removed, together with the 128 MiB EL0 window as the pool's reason: EL0 tensor pointers are ledger buffers mapped in the application's space. Rows N1, N2 (the window half), N3 (with the other stream), N4, N5 and C13 (for NPU buffers) fall.

**Host checks.**

- `checks/umem-ledger.c`: randomized allocate, free and convert sequences against a model, checking that extents never overlap, every extent meets its alignment, window and `phys_max`, free bytes plus allocated bytes equal usable bytes, and generations are never reused. It counts the steps of acquire and release with 40,000 live buffers against a budget: the 99th percentile of an unconstrained acquire at most 400 steps, its worst at most 800, a release at most 400.
- `checks/umem-policy.c`: the conversion sequence against a translation model with a TLB and speculative accesses between instructions, which flags any moment at which two attributes reach one chunk and any maintenance through an unmapped address.
- The NPU table walker (`checks/npu-mmu.c`) runs over the tables the NPU service builds from ledger mappings and proves that the reachable physical pages equal the mapping records of the space, including a mapping above 4 GiB, a 16 MiB page and a mixed-size 16 MiB range refusal.
- `checks/region-el0.c` on QEMU: an application space maps a ledger buffer in upper memory and another in slot 1; the other space and the ledger's own metadata page take a permission fault, the ledger grows on the real tables, and a converted chunk is non-cacheable in every root. The old window `0x4400_0000`..`0x4700_0000` joins this check when step 4 moves it into the ledger.
- `checks/os-npu-pool-bind.py` and `checks/npu-pool-size.py` are replaced by a check that an image links no pool constant, and `checks/memory-layout.py` drops the pool rows.
- The QEMU OS boot (`checks/qemu-os-boot.py`) runs with a 1 GiB allocation at admission.

**Board run.** One OS image with an application that declares an NPU session and allocates three NPU buffer sets with `UMEM_AT` at `0x1_0000_0000` (physical bit 32 only), `0x2_0000_0000` (bit 33 only) and `0x3_0000_0000` (both), each holding a tensor set, the command buffer and a table extent for the matching directory. The run submits the qualified add graph 1,000 times per placement and compares every output with the host reference, then runs a second application space that reads the first one's tensor and the NPU job whose command stream addresses an unmapped device page.

**Prediction.** All 3,000 jobs return the reference output with the guard words intact. The second space takes a permission fault at the tensor's address (data fault status `0x0d` for a 1 GiB block, `0x0e` for a 2 MiB block, `0x0f` for a 4 KiB page, by the level of the kernel's entry). The unmapped-page job ends in an NPU MMU exception (`ss_npu_last_fault` returns the faulting device address) and the session stays usable. Allocation and zeroing of a 1 GiB buffer on one A55 core completes in no more than 0.5 s (one A55 core streams 5.5 GB/s in the memory records, plus a clean pass and a factor of two). If the NPU ignores physical bits 39:32 or the directory window, a job in the second or third placement reads or writes low memory, and the guard check catches it.

### Step 2: the GPU reservations

**Scope.** The GPU service takes firmware objects, compute payloads, graphics buffers and every page table from the ledger. `A733_GPU_RESERVED_*`, `A733_GPU_COMPUTE_PAYLOAD_PAGES`, `PVR_COMPUTE_SERVICE_TABLE_PAGES`, `A733_GPU_GRAPHICS_*` and the `private_image()` range checks are replaced by sizes computed from the firmware image and the request (rows G1 to G6). EL0 clients' buffers are mapped at EL0 and in the GPU context, and the worker's per-buffer `memcpy` and the 64-page client buffer limit are gone. This step also corrects [memory-architecture.md](memory-architecture.md) and [gpu-runtime.md](gpu-runtime.md), which name 2 MiB GPU pages: the GPU's largest data page on this part is 256 KiB, and the 2 MiB alignment of large `dma32` extents serves the CPU block mapping.

**Host checks.**

- The GPU table walker (`checks/a733-gpu-vm-memory.c`, `checks/gpu-graphics-memory.c`) over contexts built from ledger buffers at addresses above 4 GiB, with the 256 KiB page selection and a refusal of 1 MiB and 2 MiB pages.
- A replay of the admission order of every GPU image through the ledger in `checks/umem-ledger.c`, comparing table bytes and free extents against the fixed reservations they replace (the `checks/device-memory-capacity.c` replay shape).
- `checks/gpu-session-isolation.c`, the check [gpu-runtime.md](gpu-runtime.md) plans: every catalogue entry, free-list entry and command address of a session lies in a buffer mapped to that session.
- The cache hand-off model of `checks/memory-reservations.c` over the generalized `umem_handoff`.

**Board run.** The GPU compute image runs the four changed-input local-memory GEMMs of the qualified records in [gpu.jsonl](../evidence/gpu.jsonl). A, B and C come from the ledger in three placements: below 4 GiB, bit 32 only, and bits 33 and 32. Firmware objects stay below 4 GiB, their working limit. A second GPU context runs a job that reads an address that only the first context maps.

**Prediction.** Every GEMM output digest equals the qualified digest in all three placements; firmware start and stop return normally; per-job time is within 3 percent of the qualified record. The second context takes a GPU MMU page fault and the first context's data is unchanged. If a placement above 4 GiB fails, the digest differs and the GPU data limit stays at 4 GiB.

### Step 3: the device and uncached pools

**Scope.** `device_memory_pool` becomes a thin adapter over the ledger, then each consumer moves to `umem_alloc`: display, camera, DMA services, crypto, the USB host, xHCI, NVMe, G2D, deinterlacer, the video codecs, ISP and the audio services. Rows C4 to C8, C10 (for pool consumers), D2, D4, D6 and D10 fall. `__device_pool_size`, `__uncached_pool_size`, `a733_memory_boot_uncached()`, the 2 MiB block at `0x41c0_0000` and the extended pools go away; `NC` chunks come from the policy conversion. The IOMMU builder takes the span count from its caller, and an IOVA allocator replaces the fixed camera IOVA. The adapter is deleted when its last user moves, and a check forbids new users.

**Host checks.**

- `checks/device-memory-capacity.c` and `checks/memory-reservations.c` run against the adapter, then are replaced by `checks/umem-ledger.c` cases for pool-shaped use (the same admission orders).
- The IOMMU table walker (`a733_iommu_translate`) over tables built for 100 spans in 40 windows, including a span at physical `0x1_8000_0000`, which the existing check does not cover ([dma-high-memory.md](dma-high-memory.md)).
- The DMA0 model of `checks/dma-address-width.c` extended with descriptors above 4 GiB and with the derived segment limits.
- The extended-layout boot comparison in `checks/device-memory-capacity.py` becomes a comparison of the boot table with the ledger's initial state (all `WB`, 1 GiB blocks).
- A grep check that no image defines `__device_pool_size`.

**Board run.** One image runs four tests with ledger buffers above 4 GiB:

- A DMA0 memory-to-memory copy of 4 MiB with source at `0x1_0000_0000` (bit 32 only) and destination at `0x2_0000_0000` (bit 33 only), with descriptors below the descriptor limit.
- A G2D fill and copy through the IOMMU into a buffer at `0x3_0000_0000`.
- A coherence probe: a CPU write through an `NC` mapping to a 4 KiB buffer, read by DMA0 without maintenance.
- The same probe through a `WB` mapping, read by DMA0 without a clean.

**Prediction.** Both copy digests match; the G2D output matches the host reference and the IOMMU fault registers stay clear. The `NC` probe matches. The `WB` probe returns the old contents for every line (a 4 KiB write stays in the cache, so the device reads memory that the CPU has not updated), which shows that the policy and the maintenance rule do what the model says. If the `WB` probe matches, a master snoops the CPU caches and the maintenance table changes for that master (open question 7).

### Step 4: the application window

**Scope.** The application window, the slot 1 area, the boot assets range and the USB arena become ledger buffers. A manifest region with address 0 is placed by the ledger at admission; a region with a pinned address (runner uploads) uses `UMEM_AT`. The shared EL0 table shrinks to `0x4000_0000`..`0x4220_0000`. `window`, `el0`, `slot1_area`, `upper_area` and `boot_slot_load` leave `tools/memory_layout.py`, `APP_REGION_FLOOR` becomes the OS image end, the manifest tool places regions with the ledger's rules, and the heap threshold `heap_link_max` goes. Rows C13 to C16, C17 (granule), C20 and C21 fall. Rows C1 to C3 stay.

**Host checks.**

- `checks/memory-layout.py` compares the linker scripts and the table builder with a layout record that holds only the image extents and the DRAM end.
- `checks/app-region.c` over the 4 KiB granule, level-3 mappings, mappings added after start, and a buffer shared between two spaces; the TLB model of the existing check shows both outcomes of a global entry.
- The golden boot-table comparison (`checks/device-memory-capacity.py`) for the shrunken EL0 map: EL0 access exists for the OS image only.
- `checks/region-el0.py` on QEMU: every address of the old window faults for a space that does not own it.
- Application manifests built with the pinned addresses of the existing records produce the same admission result as before.

**Board run.** The qualified probe image that runs EL0 routines in three address spaces. Each space owns one 8 MiB buffer placed by the ledger inside the old window range (`0x4400_0000`..`0x4700_0000`), one 3 MiB buffer in the slot 1 area and one 1 GiB region in upper memory, and each routine reads and writes its own buffers and then touches the other two spaces' buffers.

**Prediction.**

- Every access to an owned buffer succeeds.
- Every access to another space's buffer takes a permission fault with the faulting address equal to the access address. The status is `0x0d` for the 1 GiB region, `0x0e` for a 2 MiB block and `0x0f` for a 4 KiB page, as in records `1793638933466934204-4dc4152d` and `1793638990638144080-9a9aad2d`.
- The OS image stays readable and writable in every space.
- The TTBR0 switch costs 4.4 µs per round trip, within 5 percent, as in record `1793632829779717086-f7d0d1cf`.

## What stays fixed

These bounds remain after the migration. Each has a hardware source; none is a build parameter.

| Bound | Value | Source |
| --- | --- | --- |
| DRAM base and size | `0x4000_0000`, 12 GiB | Boot0 and training report 12,288 MiB; address test over the map; `SUNXI_DRAM_BASE` in the pinned TF-A |
| OS image, monitor and tables | `0x4000_0000`..`0x4220_0000` | Boot chain placement (launcher, entry `0x4100_0000`, monitor interval); fixed by the loader formats |
| Excluded hole | `0x7900_0000`..`0x8000_0000` until measured | No pinned source for the boot0 secure region |
| CPU translation | 4 KiB granule, 39-bit input, 64-byte line, ASID width of the part | Arm ARM D8, `CTR_EL0`, `ID_AA64MMFR0_EL1` |
| NPU MMU | 32-bit device address, 40-bit physical, 256 directory entries of 16 MiB, one page size per 16 MiB, tables in one 4 GiB physical window, low 1 MiB and SRAM window unmapped, command fetch of at most 524,280 bytes | Vendor driver `vip_drv_mmu.c`, `vipdMAX_CMD_SIZE` |
| GPU MMU | 40-bit virtual, 36-bit physical, pages of 4, 16, 64 and 256 KiB, one page size per 2 MiB region | Firmware feature record, erratum BRN 71317 |
| GPU virtual heap layout | PDS and USC heaps of 4 GiB, general heap of 128 GiB | Pinned native heap profile |
| IOMMU | 34-bit IOVA and physical, 4 KiB pages, two banks of one directory each, 4,096 directory entries | Vendor `sunxi-iommu-*`, A733 manual p. 406 |
| DMA0 | data 2^35, descriptors below `0xffff_f800` | A733 manual, vendor `sunxi-dma.c` |
| Width of xHCI, PCIe, SD DMA, crypto | per the table in "Each device has one measured width" | Open questions 5 and the vendor driver |
| Page granule of the ledger | 4 KiB | Smallest page of every mapping hardware |

## Open hardware questions

Each needs a board measurement unless it says otherwise.

1. **NPU above 4 GiB.** Whether the NPU honors leaf bits 11:4 (physical bits 39:32) for tensors and command buffers, and whether the page-directory register and the table window work with tables above 4 GiB. Step 1 measures bit 32, bit 33 and both.
2. **GPU above 4 GiB.** Whether data above 4 GiB works through the GPU MMU, and separately whether the MIPS firmware's remap registers and page tables accept firmware objects above 4 GiB. Step 2 covers data; the firmware objects need a second run with its own recovery plan, because a failed firmware start returns only through the watchdog.
3. **IOMMU above 4 GiB.** Leaf bits 9:8 and a directory above 4 GiB for G2D, display and the codecs. Step 3 covers G2D; the camera needs a connected module (the matrix in [dma-address-width.md](../dma-address-width.md)).
4. **DMA0 high addresses.** The encoding of address bits 34:32 in descriptor parameters and of bits 33:32 in the link word, which is unverified on hardware.
5. **xHCI, PCIe and SD.** `HCCPARAMS1.AC64`, the PCIe root port's inbound width (needs an NVMe read), and the SD controller's behavior above 4 GiB with a descriptor address shifted right by 2.
6. **Run-time change of live device tables.** The NPU page-directory switch and MMU flush between jobs of different applications (cost and correctness with the NPU idle), the GPU translation-cache invalidation protocol on a live context (host evidence only), and IOMMU range invalidation while other masters run. Each measurement reports the per-switch time against the job time and runs under the NPU and GPU clocks of the approved top operating points.
7. **Coherence of any master.** Whether the GPU (with `PVR_GPU_VM_COHERENT`), the NPU or an IOMMU master snoops the CPU caches. Step 3's `WB` probe answers it for DMA0; the same probe for the NPU and GPU follows. A snooping master removes maintenance for that master.
8. **Maintenance and zeroing cost.** The clean cost per line for 64 KiB and 1 GiB on A55 and A76 cores, the `DC ZVA` block size and rate at EL1, and the policy conversion time of one 2 MiB chunk (invalidate, break-before-make, barriers). The first two are queued in [memory-architecture.md](memory-architecture.md); the conversion needs its own run.
9. **The hole and launch evidence.** The location of boot0's secure region, which decides whether `0x7900_0000`..`0x8000_0000` joins the ledger, and launch evidence (no remote firmware or bus master from an earlier stage uses `0x4800_0000`..`0x3_4000_0000`) before the ledger owns that range ([memory-map-12g.md](memory-map-12g.md), "Launch evidence the gate does not cover").
10. **EL0 maintenance instructions** (a source check, no board): whether `SCTLR_EL1.UCI` is set, which decides if `ss_mem_sync` for a clean needs the call, and whether `DC ZVA` is permitted at EL0.
11. **Mismatched-attribute risk in conversion.** Whether the A55 and A76 hold a line of a chunk after `DC IVAC` and the invalidation of its mapping when the chunk is then read through the `NC` attribute. The conversion avoids aliases by construction, and the board run of question 8 reads a converted chunk after a poisoned write to check it.

## Limitations

- A buffer is one physically contiguous extent. Fragmentation after long runs can refuse a large request while memory is free. A buffer of several extents is a later extension; it needs EL0 addresses that differ from physical addresses, which the identity mapping excludes today.
- Masters of one IOMMU bank share an address space, so containment between those masters is by mapping lifetime.
- The OS image stays EL0-readable and writable. SDK calls are plain function calls and no system-call gate is planned: per-application page tables and the device MMUs isolate buffers at no per-access cost, and application performance takes priority over containing the OS image.
- The design adds no quota. An application can exhaust memory; a manifest-declared reservation that admission guarantees is the extension if a deployment needs it.
