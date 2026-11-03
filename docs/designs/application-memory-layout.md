# Design: application memory sized from the build and the manifest

Status: **qualified on the board** for the heap from the ledger, the main stack in the ledger, manifest regions in the 48 MiB window, the slot 1 area and the upper memory. Also qualified: preserving runs of those regions, and an NPU pool in the slot 1 area. **Implemented**, host and emulator checked: the layout record, scratch regions, the preservation estimate and budget, and constructors that run after the heap is attached. **Open**: the application image area above `0x4800_0000`, a layout record that writes the linker script, run-time memory requests, a stack pool with a guard block per worker, and removal of the heap threshold `heap_link_max`.

Readers are assumed to know AArch64 stage-1 translation (Arm ARM DDI 0487, D8), the FreeRTOS SMP port, [memory-architecture.md](memory-architecture.md) and [large-app-regions.md](large-app-regions.md).

An application on SunstoneOS meets memory limits that come from constants in the build, the tools and the boot tables. None comes from the hardware. This design places every kind of application memory so that the build and the manifest size it, and the only bounds left are the installed memory and the address widths of the bus masters. Board runs of this design are evidence records in [the evidence file](../evidence/application-memory-layout.jsonl).

## Limits

| # | Limit | Where it is set | State |
| --- | --- | --- | --- |
| 1 | Code and read-only data: 2 MiB | `CODE ... LENGTH = 2M` in `platform/a733-smp-cached/link.ld`, `apps/board-single-core/link.ld`, `apps/os/usb-memory-link.patch`; `IMAGE_RAM` in `apps/os/cpu-return-host.py` | Open. The OS and the application share the 2 MiB; an OS image has 0.75 MB of code and read-only data. |
| 2 | Data, bss, static stacks and heap: about 10 MiB | `RAM ... LENGTH = 14M` and `ASSERT(__bss_end <= 0x42000000)` in `link.ld`; `__heap_end <= 0x41c00000` in `cpu-return-host.py` | A heap above `heap_link_max` (4 MiB) comes from the ledger, so the manifest accepts up to 11 GiB of `heap_bytes`; linked data, bss and static stacks stay in the 10 MiB. |
| 3 | Input, work and output regions, GPU buffers, NPU pool: one 48 MiB window | `window` in `tools/memory_layout.py` | Lifted. Regions continue into the slot 1 area (`0x4800_0000..0x7900_0000`) and the upper memory (`0x8000_0000` to the installed memory). |
| 4 | EL0 reaches only `0x4000_0000..0x4800_0000` through the shared table | `platform/a733/platform.c` maps 64 blocks for EL0 | Lifted for regions the application is granted: its own tables map them (see "What EL0 reaches"). |
| 5 | Large regions exist only at build time | [status/large-regions.md](../status/large-regions.md) | Open: run-time requests. |

Other constants: the board runner uploads and backs up the FEL reach, `0x4000_0000..0x1_0000_0000` (`tools/board_images.py`); the boot-slot planner loads below `0x4700_0000` (`tools/boot-slots.py`). The ledger's entry storage is sized at build time from the regions the manifests declare (no fixed count). A space needs at most `APP_SPACE_PAGES` (13) table pages, and `SS_REGIONS` is 8 in the manifest format.

## Decisions

| Question | Decision |
| --- | --- |
| What sets an address | One record, `tools/memory_layout.py`, holds the addresses and bounds of the images. The manifest tool, the CPU-return host, `tools/board_images.py` and `tools/boot-slots.py` import it; `checks/memory-layout.py` compares it with the linker scripts, `apps/os/identity.c`, `platform/a733/platform.c` and the importing tools, and with `--image` places a linked image against it. A planner that writes the linker script from the record is open. |
| What stays fixed | Addresses that the boot ROM, the monitor or the card format name: boot0 and launcher, the entry at `0x41000000`, the session at `0x41f00000`, the monitor interval `0x42000000..0x42200000`, the uncached block at `0x41c00000` and the device pools. They belong to the OS and do not grow with an application. |
| Where the application lives | Regions, heap and stacks in the slot 1 area and the upper memory, by the ledger. The image area (text, read-only data, data) above the OS core is open. |
| How the application is mapped | By its own page tables at EL0, as [large-app-regions.md](large-app-regions.md) designs for upper memory. |
| How the image is loaded | The FEL upload and the stage-2 loader write each ELF load segment to the address where it links, anywhere below 4 GiB. A first-stage loader that places data above 4 GiB is not built until a consumer needs it; loading an application as a code region (card speed, relocation, instruction-cache maintenance) waits for a plug-in consumer. |
| Heap, stacks and large regions | Obtained from the region ledger when the application is admitted; run-time acquisition is open. |
| Hardware bounds | Checked against `include/dma_address_width.h` for each region from the devices the manifest names. |

## The memory map

Physical DRAM is `0x4000_0000..0x3_4000_0000` (12 GiB). Application memory is identity mapped: a virtual address equals its physical address, so DMA addresses and cache maintenance need no translation table.

| Range | Contents | Sized by |
| --- | --- | --- |
| `0x4000_0000..0x4100_0000` | boot0 and launcher | fixed |
| `0x4100_0000..0x4200_0000` | OS core: entry and vectors, the OS code and data, OS heap, device pool, uncached block at `0x41c0_0000`, session at `0x41f0_0000` | the OS build; fixed addresses |
| `0x4200_0000..0x4220_0000` | monitor, result, contract, boot tables | fixed |
| `0x4220_0000..0x4800_0000` | USB arena, boot assets and the pinned-address window | existing manifests |
| `0x4800_0000..0x7900_0000` | slot 1 area: the extended layout's `dma32` and `uncached` pools when the image links them, manifest regions, NPU pool, stacks, heap | the ledger and the manifest tool |
| `0x8000_0000..0x3_4000_0000` | upper memory: manifest regions, heap, large regions | the ledger and the manifest tool |

The planned application image area follows the image's device zone and holds, each on a 2 MiB boundary: text (read-only, executable at EL0), read-only data, data and bss, and the stack pool (the main stack and each worker stack, `stack_bytes` each, with an unmapped guard block below every stack). After those come the heap, the regions in manifest order (INPUT, WORK and OUTPUT, GPU buffers, the NPU pool), and large regions (each of 1 GiB or more on a 1 GiB boundary).

A region with an address of 0 in the manifest is placed by the manifest tool: first fit in the window, then the slot 1 area, then the upper memory (`upper_area` of the record, `0x8000_0000` to the installed 12 GiB). A region with a nonzero address stays where the manifest puts it, and the tool checks that it lies in installed memory, does not cross the unmapped hole and overlaps no other extent. The ledger holds every placed region from `0x4800_0000` up by its address (`apps/os/large.c`), so the heap, the large records, the NPU pool and the stack take what is left.

### Hardware bounds

Each bound comes from [dma-address-width.md](../dma-address-width.md) or a driver check. A region is placed below the smallest limit over the devices its access flags and the manifest's device list name.

| Consumer | Bound on a buffer's physical address | Other bound |
| --- | --- | --- |
| CPU at EL0 and EL1 | the installed memory; the 39-bit input range of TCR_EL1 covers 512 GiB | none |
| FEL upload and stage-2 loader | below 4 GiB. The FEL request record has a 32-bit address and a 32-bit length (`struct aw_fel_request` in `fel_lib.c` of the pinned sunxi-tools revision `b92f4ed3`), and `fel.c` takes `uint32_t` offsets. Upper memory is reachable only by code that runs on the board | none |
| GPU | below 2^36 (`platform/a733/gpu_vm_memory.c:7`) | 40-bit GPU virtual addresses |
| NPU | below 2^40 | device addresses are 32 bits, so the tensors of one graph lie in one 4 GiB device span |
| IOMMU masters (display, G2D, codecs, camera) | below 2^34 | IOVA below 2^32 |
| DMA0 | below 2^35 | descriptors below `0xfffff800` |
| xHCI | below 2^34 with 64-bit addressing, else below 4 GiB | none |
| PCIe endpoint DMA (NVMe) | below 4 GiB | none |
| SD and MMC, USB0 | no DMA | none |

With 12 GiB installed, every region fits under the GPU, NPU, IOMMU and DMA0 limits, so those limits constrain placement only for masters below 4 GiB (xHCI without 64-bit addressing, PCIe). Their buffers stay in the OS device pools, which are below 4 GiB, unless the manifest grants the device a region, which the tool then places below 4 GiB.

## Layout record and image loading

The layout record is the single source of the constants in the table above. A host check links the current images with it and compares the section addresses with the build; the linked hello, hello (CPU return) and large-region images are byte-identical to the images built from the earlier constants, including the generated manifest tables.

The application is linked into the OS image, which is one ELF with several load segments. The runner uploads each segment to its link address, and the stage-2 loader of the card boot copies each extent of a slot to its load address. The FEL upload runs at about 0.55 MB/s ([board-runner.md](../board-runner.md)), so 64 MiB of initialized image takes two minutes. Bss, heap, stacks and regions are not uploaded, and data of that size belongs in INPUT or large regions that the card loader fills at card speed ([large-app-regions.md](large-app-regions.md)).

For the open image area:

- **Segment table.** The runner reads the load segments from the ELF and refuses any that lies outside the extents of the layout record or at or above 4 GiB. The stage-2 loader receives the same list in the slot header.
- **Zeroing.** The OS zeroes bss, the heap, the stack pool and every region it hands out before the application runs, because DRAM keeps the previous contents after a reset.
- **Branch range.** A `BL` reaches 128 MiB. `ld.lld` inserts range-extension thunks for AArch64; a host check links an image with a 200 MiB gap and runs it in QEMU.
- **Data addressing.** `ADRP` reaches 4 GiB from the instruction, so data and bss stay below `0x1_0000_0000`. The heap and large regions are reached through a pointer the OS stores at admission and can lie anywhere.

## Heap, stacks and constructors

A manifest whose `heap_bytes` exceeds `heap_link_max` in the record (4 MiB) gets its heap from the ledger. The threshold keeps the image of mimus, whose manifest asks for exactly 4 MiB, and every image without the upper memory map on their linked heap until a board run of mimus qualifies the ledger heap. It is not a limit, and removing it is open work. The ledger heap works like this:

- The manifest tool appends one more large record, the last, with the flag `SS_LARGE_HEAP`: read-write, no source, no digest, a size of `heap_bytes` plus the OS's 1 MiB share rounded up to 2 MiB. The manifest cap on `heap_bytes` is the 11 GiB of upper memory.
- The image links only the OS's 1 MiB share of the heap, and the build refuses such a manifest unless the image maps upper memory (`-DSUNSTONE_A733_DRAM_DECODE_PROBE=ON`).
- At admission the ledger takes the region, the loader skips it (a heap alone opens no card and authorizes no card pins), and `runtime_heap_attach()` makes it the C library's heap before the C library initializes. Attachment fails, and with it the admission, when the linked heap has already handed out a byte.
- `_sbrk` zeroes each range it hands out of an attached heap, so memory that an earlier run left in DRAM never reaches the application.
- The static constructors of the application and the C library run once, in `runtime_init`, which `os_application_prepare` calls after `os_large_prepare` has attached the heap; no boot code runs the `.init_array`. A constructor that takes memory from `sbrk` takes it from the ledger's region, zeroed.
- Every task of the application starts in its space with no call: a new task takes the address-space tag of the task that creates it (`traceTASK_CREATE` in `FreeRTOSConfig.h` with `os_space_inherit()` in `include/os_space.h`; both the stock SMP scheduler and the partitioned scheduler call it before the task becomes ready). The main task is tagged at admission. A task of the OS stays in the kernel's table. `ss_large_attach` remains for a task that other code created.
- An application with upper memory also takes its main stack from the ledger: a 2 MiB block that the guard owner holds below it, so that an overflow is an EL0 fault in memory no space maps. Where no extent is free the static stack stays. Worker stacks and guard blocks per worker are open.

A manifest with `heap_bytes` of 4 MiB or less links its heap as before.

## What EL0 reaches and what changes in the tables

**Per-application tables.** `app_space_build()` copies the kernel's level-1 table and gives the application's regions EL0 access. Slots 2 and above map as 1 GiB blocks where a slot is one region's, or as 2 MiB blocks where a gigabyte is shared. Slot 1 (`0x4000_0000..0x8000_0000`) is built from `base_l2`, the kernel's 512 level-2 entries (`include/app_region.h`). Each application gets its own copy of slot 1's level-2 page (4 KiB) that keeps the kernel's 64 EL0 blocks below `0x4800_0000` unchanged and rewrites the blocks at and above from the application's extents, so the uncached block at `0x41c0_0000`, the device pools and the monitor interval keep their attributes; a host check compares them entry by entry.

**Permissions.** Text is EL0 read-only and executable (UXN clear, PXN set), read-only data is read-only and non-executable, and data, heap, stacks and regions are read-write and non-executable. Every entry is non-global, as in [large-app-regions.md](large-app-regions.md). The context switch hook writes TTBR0 when the incoming task belongs to another application.

**Boot tables.** `platform_init()` maps `0x4800_0000..` and above for EL1 only, from the `dram_map` list of `platform/a733-smp-cached/memory.c`, when the decode check passes: level-2 blocks below `0x8000_0000`, 1 GiB blocks from there. Application functions that run at EL1 (`dram_nsi_worker` and `dram_qos_worker` in `apps/os/application.c`) need the application text mapped read-only and executable at EL1 as well; that belongs to the open image area. The kernel's slot 1 level-2 page covers `0x4800_0000..0x8000_0000` and slots 2 to 12 are 1 GiB blocks, so the boot tables gain no page and stay within `0x421f_0000..0x4220_0000`.

**EL3 tables for the normal-return terminal.** The EL3 level-2 table covers `0x4000_0000..0x8000_0000`. `platform/a733-smp-cached/memory.c` gives the EL3 level-1 table a 1 GiB block for each gigabyte that holds a work or output region, only where the boot's decode check passed. The terminal (`apps/os/cpu-return/terminal.patch`) reads that block for an address from `0x8000_0000` and applies the test it applies to a level-2 block (NS, write-back, inner-shareable, access flag), cleans the region and parks on a block the tables do not map that way. The EL3 publication of the identity step (`platform/a733/cpu_coherence.S`) accepts the installed memory.

**DRAM decode check.** The check runs at EL3 with the MMU off over the ranges `platform.c` lists, so every address the application can use has passed. It never runs after boot, so a run-time request draws only from checked memory. If a range fails, the ledger omits it and an image that needs it fails at admission with a message that names the range.

**CPU-return backup and restore.** Extents come from the layout record, and the runner's reach is the FEL reach of 4 GiB. A manifest region of kind work or output may take the access name `scratch` (bit 4 of the region's access word, which no OS code reads): `apps/os/host.py` leaves it out of the backup list, while the CPU-return terminal still cleans it. An input cannot be scratch, since the runner uploads it. An extent that reaches or passes 4 GiB is refused for a preserving run, with a message naming the extent and the reach. `tools/board_images.py` estimates the transfer of a preserving plan as two passes of the preserved bytes at the recorded FEL rate (550 kB/s); the runner prints the estimate before the backup, and `tools/board.py submit --preserve-budget-s N` refuses a plan whose estimate exceeds N seconds. A development run makes no preservation claim and transfers nothing. `checks/runner-preservation.py` covers the reach, the scratch attribute, the estimate and the budget; six mutations of the runner each fail it.

**Uncached and device pools** stay in the OS core below 4 GiB, outside the application area, because their masters have 32-bit limits and the uncached pool must be a whole 2 MiB block with no cached alias ([memory-architecture.md](memory-architecture.md)).

## NPU pool and GPU buffers

| | Memory | Who maps it | What contains the device |
| --- | --- | --- | --- |
| NPU pool | 8 MiB at `0x47400000`, or 10 to 48 MiB of whole blocks ending at `0x47000000` (`OS_NPU_POOL_BYTES`, linker symbols `__os_npu_pool_start` and `__os_npu_pool_end`, bound into the session record by `apps/os/host.py` and compared by `apps/os/admission.c`); or, when the image sets `SUNSTONE_OS_NPU_POOL_BASE`, 10 to 768 MiB in the slot 1 area | The CPU: EL1 read-write write-back blocks of the kernel table, none in an application's space. EL3: cleaned by the terminal, which parks on an extent it does not know. The NPU: only through its own page tables, built once at admission in an arena inside the pool (`drivers/npu/a733_npu_runtime.c`, `a733_npu_memory.c`, `a733_npu_mmu.c`) | The NPU MMU. The tables map the bootstrap stream and each slot's command, constant, read and write buffers at 4 KiB granularity, with an unmapped guard page after each; they map neither the arena, the runtime's own tables, the CPU-only staging area nor any unused byte of the pool. The write bit follows the buffer's role. The graphs run as compiled |
| GPU buffers | Manifest regions with a GPU access bit, 2 MiB aligned, in the window, the slot 1 area or the upper memory | The CPU: a GPU-written region has no writable alias (EL1 read-only blocks) and an application sees it through its own space. The GPU: only through its own MMU, one mapping per lease (`platform/a733/gpu_queue.c`) | The GPU MMU maps the leases the application asked for, rounded to 2 MiB; the admission refuses a region that overlaps the image, the device pools or another lease and one that is not identity mapped write-back |

The GPU admission accepts any 2 MiB aligned region below 2^36, so GPU buffers take their placement from the manifest tool and the ledger.

An image built with `SUNSTONE_OS_NPU_POOL_BASE` places the NPU pool in the slot 1 area: any whole-block size from 10 to 768 MiB, below 4 GiB so that the device memory class (DMA32) and the one-span rule of the table builder hold. The linker symbols still carry the address, so the session record, the runner's preservation list and the terminal need no second source. At admission `os_large_prepare` holds the pool in the ledger for the kernel (`APP_REGION_KERNEL_OWNER`) before any region of the application, so a manifest region, the heap or a stack cannot overlap it and no application space maps it; `npu.c` refuses to start the NPU unless the ledger holds exactly that extent. The platform maps the pool's blocks at EL3 for the terminal's clean, and the terminal accepts a pool in the slot 1 area (block aligned, 10 to 768 MiB).

`tools/npu-pool-plan.py` takes the slot count and the bounds for graphs registered later from the manifest's npu section, runs the production runtime's own reservation for the linked models (`checks/npu-pool-size.py`), and rounds up to whole 2 MiB blocks (at least 10 MiB). It places the pool at the top of the slot 1 area, refuses a layout in which a region the manifest tool placed there would overlap it, and prints the two cmake arguments. The default `npu-graphs` manifest with `models-a1.json` gives 10 MiB at `0x78600000`.

`checks/npu-pool-isolation.py` builds the tables with the production memory layer at the legacy pool, a declared-size pool, two pools in slot 1 and one above 4 GiB. It walks them with an independent walker over the whole 32-bit device space, and requires exactly the declared pages and no page of the arena, the staging area, the unused bytes or the 2 MiB beside the pool. `checks/app-region.py` shows the kernel's hold of the pool, and `checks/os-cpu-return.py` runs the terminal on images with the pool at 8 MiB, 96 MiB at `0x60000000`, the 768 MiB maximum and the last 10 MiB of the area. The EL3 table entries that `memory.c` writes for the pool and the NPU itself with a pool in slot 1 are covered by the board run only.

## Memory requests while the application runs (open)

No SDK call exists yet; this is the design.

```c
struct ss_memory_request {
    uint64_t bytes;       /* rounded up to 2 MiB */
    uint64_t align;       /* 0: 2 MiB; at least 1 GiB for requests of 1 GiB or more */
    uint32_t access;      /* SS_ACCESS_READ, SS_ACCESS_WRITE, SS_ACCESS_GPU_*, SS_ACCESS_NPU */
    uint32_t flags;       /* SS_MEMORY_BELOW_4G; contents are always zeroed */
};
int ss_memory_acquire(const struct ss_memory_request *, void **base, uint64_t *bytes);
int ss_memory_release(void *base);
```

Like the other `ss_*` calls, these are plain calls at EL0. Each posts a request to an EL1 service task and blocks the caller on a task notification. They are setup and teardown operations, not for time-critical paths.

**Ledger.** The ledger's entry storage is caller storage sized at build time from the manifests, so it cannot grow while the system runs. Run-time requests need entries kept in pages of 170 entries (24 bytes each) that the ledger extends from the metadata arena when a page fills. A request fails with `NO_SPACE` only when no free extent of the size exists, or with `NO_TABLES` when no table page is available. `SS_REGIONS` (8) counts regions declared at admission; run-time requests do not use it.

**Table pages.** In place of the arena sized at build time, the arena takes a 2 MiB extent from the ledger as kernel-owned, EL1-only metadata when it is empty, giving 512 pages. A request costs at most two pages, so one extent serves 256 requests. The walker reads tables through the data cache (TCR_EL1 IRGN0 and ORGN0 are 1), and the service task writes them with cacheable stores followed by `DSB ISH`.

**Update protocol.** The service task works on the application's root and level-2 pages while its tasks run on other cores.

- **Acquire.** The entries covered are invalid, so no earlier translation can be in a TLB. The task writes the new level-2 page contents, then the level-1 entry that points to it, cleans and invalidates the lines of every page it wrote (`DC CIVAC` and `DSB SY`), and replies. The cleaning is required: the eight-core images walk their tables without the data caches, so a descriptor only in the writer's cache is invisible to the walker. The requesting core executes an `ISB` before it first touches the memory. Another core that faults on an address that was invalid retries and finds the entry.
- **Release.** Break before make: write the entry invalid and `DSB ISH`. The extent stays reserved in the ledger until the invalidation described below has completed for every core that could hold an entry. A release of a region with a device grant is refused.
- Resizing in place and changing permissions are not offered.

### Real-time cost of the broadcast invalidation

`TLBI ...IS` is broadcast to every core of the inner shareable domain, and the receiving cores stall their translations until it completes. The robots run control loops above 1 kHz with a highest-priority wake latency target of 10 µs or less and a control-path jitter target of 5% of the period, so the design limits when one can be issued:

- **When one can happen.** Only inside `ss_memory_release()` and `ss_memory_acquire()` calls of the application itself, executed by the service task. No interrupt handler, context switch, driver, fault handler or timer issues one. A context switch writes TTBR0 and needs no invalidation because every entry is non-global and carries an ASID. The static path (regions granted before the main task starts) issues none after boot.
- **Never on a hot path.** An application's control loop acquires its memory before it enters the loop and releases it after it leaves.
- **Acquire needs none.** The entries are invalid before the call, and Armv8 does not cache invalid entries, so acquire needs `DSB ISH` only.
- **Release prefers the weaker forms**, in this order:
  1. The kernel does not need the invalidation until the extent is reused, because the extent stays reserved in the ledger. The service task keeps a released extent reserved, with its entries invalid, until the application's teardown or until another request needs the space. A stale entry can only be reached by a bug of the application, which then reads memory that still belongs to it.
  2. When the extent must be reused before teardown, the task invalidates locally with `TLBI ASIDE1` (not broadcast) on each core that runs a task of the application, by an inter-processor request to exactly those cores, known from the application's affinity masks in the manifest.
  3. The broadcast form `TLBI ASIDE1IS` or `TLBI VAE1IS` is the fallback if the local form cannot be shown correct.
- **Measurement before it is relied on.** The first board step that issues a broadcast invalidation measures its effect on the control path with the scheduler latency harness (`apps/latency`, metrics `wake_idle` and `wake_busy`, eight pinned tasks released by the CPU0 timer) at a 10 kHz release period, with a release every 100 ms from the A55 cluster while the harness runs on the A76 cores. A measurable cost (more than 5% of the 100 µs period at the 99.9th percentile, or any worst case above 10 µs) selects the local form or the deferred reuse. The prediction is the `application-memory-layout-tlb-release` record.

**Fragmentation.** The mapping stays identity, because device DMA uses physical addresses. A request therefore needs contiguous physical memory. First fit with 1 GiB alignment for large requests keeps large gaps whole. A request that fails for fragmentation reports the largest free extent so that the application can retry with less. If fragmentation shows in practice, the remedy is to separate virtual from physical addresses for CPU-only memory (discontiguous 2 MiB extents at consecutive virtual addresses). Memory that a master reads or writes stays identity mapped, or is reached through IOVA mappings by the masters behind the IOMMU (display, G2D, codecs, camera). Masters without an IOMMU (DMA0, xHCI, the PCIe endpoint, and the NPU aside from its own MMU) need physical contiguity or a per-transfer scatter list. This step is not scheduled.

**Zeroing.** The service task zeroes a granted extent in 2 MiB steps at a priority below the application's and before it signals the reply. At 5 GB/s, the single-core read rate in [dram-bandwidth.md](dram-bandwidth.md), 1 GiB takes about 0.2 s. The write rate is a board measurement of the step that adds the call.

## Qualified runs

| Run | Figure | Record |
| --- | --- | --- |
| Heap from the ledger, 1 GiB heap at `0x8000_0000`, modes 0 to 3 | mode 3 returns the expected 128-byte result | `application-memory-layout-heap-modes`, `1793650900758260472-be096b75` |
| Preserving run, regions in the slot 1 area | 44,040,192 bytes backed up, estimate 160.1 s under a 240 s budget, scratch region left out | `1793652142808675717-ffd657dd` |
| Preserving run, regions above 2 GiB | same plan at `0x88000000`, `dram_restored` true | `1793670225972455043-c4f45b62` |
| NPU graphs with the pool at `0x78600000` | seven graphs `match` | `1793665683000181061-de31a526`, `1793668814201417903-92757416` |

The records are in [the evidence file](../evidence/application-memory-layout.jsonl).

## Limitations and open work

1. **Application image area** (text, read-only data, data above the OS core): renamed application sections, a `layout.ld` produced from the record by a two-link build, boot-table text mapping at EL1 and EL0, stage-2 loader extents and thunk checks for the real size. Board run: a returning image whose text is 40 MiB, run to a normal return, with an EL1 call into the text.
2. **Run-time requests** (`ss_memory_acquire`, `ss_memory_release`): ledger pages, table-page extents and the release protocol, with a board run that acquires and releases 256 MiB and 1 GiB regions from the A76 and A55 clusters and records the release latency and zeroing rate. It depends on the heap and on the slot 1 tables.
3. **Heap for every application**: remove `heap_link_max` so that every heap comes from the ledger when the decode check passes, with the linked heap as the fallback of an image whose decode check fails. This changes the mimus image, so it waits for a board run of mimus.
4. **Stack pool**: worker stacks from the ledger with a guard block below each.
5. **Planned checks** for the open items: `checks/memory-layout.py` equality of the record with the constants for every manifest and image and equal section sizes between the two links. Then `qemu-os-boot.py` with a 100 MiB text; a 256 MiB INPUT at `0x5000_0000` uploaded by FEL whose EL0 checksum equals the host's; one NPU graph from a 96 MiB pool above `0x4800_0000` with output equal to the 48 MiB pool run.
6. **Decode check coverage.** The check proves linear decode, not that the memory firewall passes Non-secure accesses or that no other master writes. A bus error resets the board through the watchdog.
7. **FEL address width.** The protocol limit is verified from the pinned tool source; the runner refuses higher addresses.
8. **Upload time.** An initialized image above tens of megabytes costs minutes per run at 0.55 MB/s.
9. **Start-up order.** A static constructor that calls `malloc` before admission refuses the application at admission (QEMU case `heap-after-early-sbrk`); the constructors run after the heap is attached, so only another caller of `sbrk` before admission still refuses it.
10. **Stale memory.** Every extent handed out must be zeroed, and boot-time zeroing of the heap and bss has to be measured against the supervision window.
11. **EL1 execution of application text.** The kernel tables must map it, and the application's EL1 worker functions have to be listed in the layout record.
12. **Fragmentation** of an identity-mapped space: a long-running application can fail a large request that enough free memory in total could serve.
13. **ASIDs.** The pool hands out 255 and never returns one ([large-regions status](../status/large-regions.md), items 7 and 8); release of an application space is out of scope.
14. **Real-time cost of invalidation:** on port 3-1 under the full memory load of the scheduler latency diagnostic, the issuing core waits at most 9.8 us (`TLBI VAE1IS`), 8.6 us (`ASIDE1IS`) and 6.8 us (`VMALLE1IS`) for a broadcast invalidation to complete. No wake or sweep count moves outside the spread of two control runs (records `1793671134053745086-10f06621` to `1793671172712527319-915ef621` in `docs/evidence/application-memory-layout.jsonl`). The 1 kHz harness cannot resolve a few microseconds, so the broadcast form is not cleared against the 5 us criterion: a measurement at 10 kHz on the control path remains before an application call uses it.
