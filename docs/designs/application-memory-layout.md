# Design: application memory sized from the build and the manifest

Status: **design**, with two steps of the migration implemented and host and
emulator checked: the layout record (step 2) and the heap from the ledger
(step 1). No part is board-qualified. See "Implemented so far" below.
Readers are assumed to know AArch64 stage-1 translation (Arm ARM DDI 0487,
D8), the FreeRTOS SMP port, [memory-architecture.md](memory-architecture.md)
and [large-app-regions.md](large-app-regions.md).

An application on SunstoneOS meets five memory limits that come from constants
in the build, the tools and the boot tables. None comes from the hardware. This
note places every kind of application memory so that the build and the manifest
size it, and the only bounds left are the installed memory and the address
widths of the bus masters.

## Where the limits are today

Each limit below was checked in the tree.

| # | Limit | Where it is set | Effect |
| --- | --- | --- | --- |
| 1 | Code and read-only data: 2 MiB | `CODE ... LENGTH = 2M` in `platform/a733-smp-cached/link.ld:4`, `apps/board-single-core/link.ld`, `apps/os/usb-memory-link.patch`; `IMAGE_RAM = (0x41000000, 0x1200000)` in `apps/os/cpu-return-host.py:38` | The OS and the application share the 2 MiB. An OS image built for the board in `~/.cache/sunstoneos/build` has 0.75 MB of code and read-only data. |
| 2 | Data, bss, static stacks and heap: about 10 MiB | `RAM ... LENGTH = 14M`, `ASSERT(__bss_end <= 0x42000000)` in `link.ld`; `__heap_end <= 0x41c00000` in `cpu-return-host.py:87`; heap size is a link-time symbol | The same OS image has 6.7 MB of data and bss, of which 2 MiB is heap, 2 MiB the device pool and 2 MiB the uncached pool. The manifest accepts `heap_bytes` up to 64 MiB (`tools/sunstone-manifest.py:101`), which cannot link. |
| 3 | Input, work and output regions, GPU buffers, NPU pool: one 48 MiB window | `WINDOW = (0x44000000, 0x47000000)` in `tools/sunstone-manifest.py:40`, `apps/os/identity.c:47`, `apps/cpu-runtime-qualification/host.py:129`; NPU pool top `0x47000000` in `cpu-return-host.py` | A region, a GPU buffer set or an NPU pool larger than 48 MiB in total is refused. |
| 4 | EL0 reaches only `0x40000000..0x48000000` | `platform/a733/platform.c:152` maps 64 blocks for EL0 and every other block for EL1 only; [npu-resources.md](npu-resources.md) states the limit under Backlog | Everything an EL0 task touches must lie in 128 MiB. |
| 5 | Large regions exist only at build time | [status/large-regions.md](../status/large-regions.md), open item 6 | The ledger and the table arena are sized from the manifest, so an application cannot ask for memory while it runs. |

Other constants follow from these: the board runner uploads and backs up only
`0x40000000..0x80000000` (`tools/board_images.py:482`), the boot-slot planner
loads below `0x47000000` (`tools/boot-slots.py:23`), the ledger holds 16
regions with a quota of four per application, and `SS_REGIONS` is 8 in the
manifest format.

## Decisions

| Question | Decision |
| --- | --- |
| What sets an address | One planner, `tools/memory_layout.py`, reads the link sizes and the manifest and writes a layout record. The linker script, the boot-table builder, the manifest tool, the runner and the stage-2 loader read the record. No other file holds an address constant for application memory. |
| What stays fixed | Addresses that the boot ROM, the monitor or the card format name: boot0 and launcher, the entry at `0x41000000`, the session at `0x41f00000`, the monitor interval `0x42000000..0x42200000`, the uncached block at `0x41c00000` and the device pools. They belong to the OS and do not grow with an application. |
| Where the application lives | In an application area that starts above the OS core and above the image's device zone, and that the planner extends upward as far as the memory goes. |
| How the application is mapped | By its own page tables at EL0, as [large-app-regions.md](large-app-regions.md) already designs for upper memory, extended to the part of the first 4 GiB that lies above `0x48000000`. |
| How the image is loaded | The FEL upload and the stage-2 loader write each ELF load segment to the address where it links, anywhere below 4 GiB. A first-stage loader that places data above 4 GiB is not built until a consumer needs it. |
| Heap, stacks and large regions | Obtained from the region ledger when the application is admitted. After this note they can also be obtained while the application runs. |
| Hardware bounds | Checked by the planner against `include/dma_address_width.h` for each region from the devices the manifest names. |

## The memory map

Physical DRAM is `0x4000_0000..0x3_4000_0000` (12 GiB). Application memory is
identity mapped: a virtual address equals its physical address, so DMA
addresses and cache maintenance need no translation table.

| Range | Contents | Sized by |
| --- | --- | --- |
| `0x4000_0000..0x4100_0000` | boot0 and launcher | fixed |
| `0x4100_0000..0x4200_0000` | OS core: entry and vectors, the OS code and data, OS heap, device pool, uncached block at `0x41c0_0000`, session at `0x41f0_0000` | the OS build; fixed addresses |
| `0x4200_0000..0x4220_0000` | monitor, result, contract, boot tables | fixed |
| `0x4220_0000..0x4800_0000` | USB arena, boot assets and the pinned-address window that existing manifests use | existing manifests; shrinks as manifests move to placed regions |
| `0x4800_0000..D` | the image's device zone (extended layout only), where D is the end of the largest linked device pool | the image link |
| `D..` | the application area, in the order below | the planner |

The application area holds, each starting on a 2 MiB boundary:

1. **Application text**: read-only, executable at EL0. Sized from the link.
2. **Application read-only data**: read-only, not executable.
3. **Application data and bss**: read-write, not executable.
4. **Stack pool**: the main stack and each worker stack, `stack_bytes` each, with an unmapped guard block below every stack so that an overflow faults.
5. **Heap**: `heap_bytes` from the manifest.
6. **Regions** in manifest order: INPUT, WORK and OUTPUT, GPU buffers (WORK or OUTPUT with a GPU access flag), and the NPU pool.
7. **Large regions**: from the ledger, each region of 1 GiB or more on a 1 GiB boundary, as designed today.

A region with an address of 0 in the manifest is placed by the planner. A
region with a nonzero address stays where the manifest puts it, which keeps
existing manifests valid, and the planner checks that it lies in installed
memory and does not overlap another extent.

The first-fit rule of `app_region_acquire()` places everything from item 4
onward. Items 1 to 3 are placed by the linker from the layout record because
their sizes come from the link.

### Hardware bounds the planner enforces

Each bound comes from [dma-address-width.md](../dma-address-width.md) or a
driver check. The planner takes, for each region, the smallest limit over the
devices its access flags and the manifest's device list name, and places the
region below it.

| Consumer | Bound on a buffer's physical address | Other bound |
| --- | --- | --- |
| CPU at EL0 and EL1 | the installed memory; the 39-bit input range of TCR_EL1 covers 512 GiB | none |
| FEL upload and stage-2 loader | below 4 GiB. The FEL request record has a 32-bit address and a 32-bit length (`struct aw_fel_request` in `fel_lib.c` of the pinned sunxi-tools revision `b92f4ed3`, whose 16-byte request has no room for more), and `fel.c` takes `uint32_t` offsets. Upper memory is reachable only by code that runs on the board | none |
| GPU | below 2^36 (`platform/a733/gpu_vm_memory.c:7`) | 40-bit GPU virtual addresses |
| NPU | below 2^40 | device addresses are 32 bits, so the tensors of one graph lie in one 4 GiB device span; the MMU image does not cross a 4 GiB boundary |
| IOMMU masters (display, G2D, codecs, camera) | below 2^34 | IOVA below 2^32 |
| DMA0 | below 2^35 | descriptors below `0xfffff800` |
| xHCI | below 2^34 with 64-bit addressing, else below 4 GiB | none |
| PCIe endpoint DMA (NVMe) | below 4 GiB | none |
| SD and MMC, USB0 | no DMA | none |

With 12 GiB installed, every application region fits under the GPU, NPU,
IOMMU and DMA0 limits, so those limits constrain placement only for the
masters below 4 GiB (xHCI without 64-bit addressing and PCIe). Buffers of those
masters stay in the OS device pools, which are below 4 GiB, unless the manifest
grants the device a region, in which case the planner places that region below
4 GiB. A region the planner cannot place fails the build or the admission with
a message that names the region and the limit.

## The layout planner

The planner replaces the constants listed under "Where the limits are today".
Its inputs are the sizes of the linked sections of the OS core and of the
application, the manifest, the devices and device pools the image links, and
the DRAM ranges that the decode check can pass. Its output is a layout record
that lists every extent with its address, size, kind, access, the master limit
that applies, and whether the runner preserves it.

The build runs in three steps. A first link measures section sizes. The planner
writes `layout.ld`, which defines `__app_text_base`, `__app_data_base` and the
other bases as symbols. A second link places the sections. A host check
compares the two links' section sizes so that a placement change cannot move a
size.

Consumers of the record:

- the linker script, in place of `LENGTH = 2M` and `LENGTH = 14M` for the application's sections (the OS core keeps its own limit, reported with the OS core's size when it overflows);
- `tools/sunstone-manifest.py`, in place of `WINDOW`;
- `apps/os/identity.c`, which validates the region records against the record;
- `platform/a733/platform.c`, which builds the boot tables from the extents;
- `apps/os/cpu-return-host.py` and `tools/board_images.py`, in place of `IMAGE_RAM`, `0x41c00000`, `0x42000000` and `0x80000000`;
- `tools/boot-slots.py`, in place of `LOAD_LIMIT`.

For an unchanged manifest and image the record is identical to today's
constants. A host check shows this by linking the current images with the
planner and comparing the section addresses with the current build.

## How the image gets there

The application is linked into the OS image, which is one ELF with several
load segments. The runner uploads each segment to its link address, and the
stage-2 loader of the card boot copies each extent of a slot to its load
address.

| Option | What it needs | Cost |
| --- | --- | --- |
| **A. Link each segment where it belongs, upload or copy it there.** | The runner and the slot planner accept segments at any address below 4 GiB that the layout record lists. The entry stays at `0x41000000`. | No new code runs on the board. The FEL upload runs at about 0.55 MB/s ([board-runner.md](../board-runner.md)), so 64 MiB of initialized image takes two minutes. Bss, heap, stacks and regions are not uploaded. |
| **B. A first stage in image RAM that places the rest.** | A loader in the OS core that reads the remaining segments from a staging area (uploaded or on the card) and copies them to their addresses, then jumps to the entry. | It reaches addresses above 4 GiB. It uploads the image at the same rate as A and adds a copy and a second verification of the digest. The loader is another program to qualify, and it runs before the OS has set up its tables. |
| **C. Load the application from the card or from runner inputs as a region.** | A code region type that is executable at EL0, an instruction-cache maintenance step after the load, a relocation story (the image is linked at fixed addresses), and the large-region loader. | It loads at card speed (about 22 MB/s predicted) instead of 0.55 MB/s. It makes the application a loadable module, which the SDK does not have. |

**Recommendation: A for the image, and the card loader of
[large-app-regions.md](large-app-regions.md) for data.** The code, read-only
data and initialized data that an application links stay moderate because
anything large and read-only (model weights, tables) already belongs in an
INPUT or large region that the card loader fills at card speed. Option A needs
no new board code and loses nothing that the existing flow provides. Option B
is added only when an image segment must lie above 4 GiB, which needs an
initialized image of more than about 3 GiB and has no consumer. Option C is
the right tool for plug-in code and waits for a plug-in consumer.

Details for A:

- **Segment table.** The runner reads the load segments from the ELF and refuses any that lies outside the extents of the layout record or at or above 4 GiB. The stage-2 loader receives the same list in the slot header.
- **Zeroing.** The OS zeroes bss, the heap, the stack pool and every region it hands out before the application runs. After a reset DRAM contains the previous contents, and a region must not expose them.
- **Branch range.** A `BL` reaches 128 MiB. With the OS core at `0x4100_0000` and a large application text far above, calls between them may exceed that. `ld.lld` inserts range-extension thunks for AArch64, and a host check links an image with a 200 MiB gap to show the thunks appear and the image runs in QEMU.
- **Data addressing.** `ADRP` reaches 4 GiB from the instruction. Data and bss stay below `0x1_0000_0000`. The heap and large regions are reached through a pointer the OS stores at admission, so they can lie anywhere.

## What EL0 reaches and what changes in the tables

**Per-application tables.** `app_space_build()` copies the kernel's level-1
table and gives the application's regions EL0 access. The application area
starts at `0x4800_0000`, which is inside level-1 slot 1 (`0x4000_0000..0x8000_0000`).
That slot is a shared level-2 table today. The builder therefore also gives
each application its own copy of slot 1's level-2 page (4 KiB), copies the
kernel's 64 EL0 blocks below `0x4800_0000` unchanged, and rewrites the blocks
at and above `0x4800_0000` from the application's extents. Blocks below
`0x4800_0000` stay byte-identical to the kernel's, so the uncached block at
`0x41c0_0000`, the device pools and the monitor interval keep their attributes.
A host check compares them entry by entry. The ledger's lowest slot changes
from 2 to 1, and its usable range starts at `D`.

**Permissions.** Text is EL0 read-only and executable (UXN clear, PXN set), read-only data
is read-only and non-executable, and data, heap, stacks and regions are
read-write and non-executable. Every entry stays non-global, as in
[large-app-regions.md](large-app-regions.md). The context switch hook already
writes TTBR0 when the incoming task belongs to another application.

**Boot tables.** `platform_init()` maps `0x4800_0000..` and above for EL1 only,
from the extents of the layout record, when the decode check passes. Today it
does this only for images built with `SUNSTONE_PLATFORM_UPPER_MAP` or
`SUNSTONE_MEMTEST`, and no OS image links the check. Two additions:

- **Text at EL1.** Some application functions run at EL1 (`dram_nsi_worker` and `dram_qos_worker` in `apps/os/application.c` are the existing cases). The kernel table therefore maps the application text read-only and executable at EL1 as well.
- **Table size.** The kernel's slot 1 level-2 page already covers `0x4800_0000..0x8000_0000`, and slots 2 to 12 are 1 GiB blocks, so the boot tables gain no page and stay within `0x421f_0000..0x4220_0000`.

**DRAM decode check.** The check runs at EL3 with the MMU off and covers the
ranges it is given. The planner passes all of the installed memory above
`0x4800_0000` (the ranges `platform.c` already lists), so every address the
application area or a run-time request can use has passed. The check never
runs after boot, so a run-time request draws only from checked memory. If a
range fails, the ledger omits it and the planner's build-time placement for an
image that needs it fails at admission with a message that names the range.

**CPU-return backup and restore.** `apps/os/cpu-return-host.py` and
`tools/board_images.py` preserve only extents inside `0x40000000..0x80000000`,
and a preserving run reads and writes each byte at about 0.55 MB/s. Both
change:

- The extents come from the layout record. The `0x80000000` ceiling becomes the FEL reach of 4 GiB, since the runner can address nothing higher.
- Each region carries a `preserve` attribute in the record, true by default. A region the manifest marks as scratch is excluded. A run that preserves memory above 4 GiB, or any extent the runner cannot address, is refused with the reason. The runner prints the estimated transfer time of the plan before it starts, and refuses a plan that exceeds the time budget the submission names.
- A development run keeps its present rule: it makes no preservation claim and transfers nothing.

**Uncached and device pools.** They stay in the OS core below 4 GiB, outside
the application area, because their masters have 32-bit limits and the
uncached pool must be a whole 2 MiB block with no cached alias
([memory-architecture.md](memory-architecture.md)). The extended layout's
`dma32` and `uncached` pools sit at `0x4800_0000..0x7900_0000`; the planner
starts the application area above the end of any such pool the image links.

## Memory requests while the application runs

**Call.**

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

Like the other `ss_*` calls, these are plain calls at EL0. Each posts a
request to an EL1 service task and blocks the caller on a task notification.
They are not for time-critical paths: a request waits for the service task and
for zeroing.

**Ledger.** The ledger replaces its fixed 16-entry array and per-application
quota by entries kept in pages of 170 entries (24 bytes each). When a page
fills, the ledger takes a new 4 KiB page from the metadata arena. A request
fails with `NO_SPACE` only when no free extent of the size exists, or with
`NO_TABLES` when no table page is available. The manifest's `SS_REGIONS` is the
count of regions declared at admission and stays 8; run-time requests do not
use it.

**Table pages.** Instead of the static `APP_SPACE_PAGES` array, the arena
takes a 2 MiB extent from the ledger as kernel-owned, EL1-only metadata when
it is empty, giving 512 pages. A request costs at most two pages (one for each
slot a region partly covers), so one extent serves 256 requests. The walker
reads tables through the data cache (TCR_EL1 IRGN0 and ORGN0 are 1), and the
service task writes them with cacheable stores followed by `DSB ISH`.

**Update protocol.** The service task works on the application's root and
level-2 pages while its tasks run on other cores.

- **Acquire.** The entries covered are invalid, so no earlier translation can be in a TLB. The task writes the new level-2 page contents, then the level-1 entry that points to it, cleans and invalidates the lines of every page it wrote (`DC CIVAC` and `DSB SY`), and replies. The cleaning is required: the eight-core images walk their tables without the data caches, so a descriptor that is only in the writer's cache is invisible to the walker. The first board run of the heap faulted at the first store for this reason, because the tables were used a few milliseconds after they were built; the admission now cleans them. The requesting core executes an `ISB` before it first touches the memory. Another core that faults on an address that was invalid retries and finds the entry.
- **Release.** Break before make: write the entry invalid and `DSB ISH`. The extent stays reserved in the ledger until the invalidation described under "Real-time cost of the broadcast invalidation" has completed for every core that could hold an entry, which for the preferred form is until teardown. A release of a region with a device grant is refused.
- **Change of mapping other than acquire.** None exists. Resizing in place and changing permissions are not offered.

### Real-time cost of the broadcast invalidation

`TLBI ...IS` is broadcast to every core of the inner shareable domain, and the
receiving cores stall their translations until it completes. The robots run
control loops above 1 kHz with a highest-priority wake latency target of 10 µs
or less and a control-path jitter target of 5% of the period, so the design
limits when one can be issued:

- **When one can happen.** Only inside `ss_memory_release()` and
  `ss_memory_acquire()` calls of the application itself, executed by the service task. No interrupt handler, context switch, driver, fault handler or timer issues one. A context switch writes TTBR0 and needs no invalidation because every entry is non-global and carries an ASID. The static path (regions granted before the main task starts) issues none after boot.
- **Never on a hot path.** The SDK documents both calls as setup and teardown operations. An application's control loop acquires its memory before it enters the loop and releases it after it leaves.
- **Acquire needs none.** The entries are invalid before the call, and Armv8 does not cache invalid entries, so acquire needs `DSB ISH` only.
- **Release prefers the weaker forms**, in this order:
  1. The kernel does not need the invalidation until the extent is reused, because the extent stays reserved in the ledger. The service task therefore keeps a released extent reserved, with its entries invalid, until the application's teardown or until another request needs the space. The application's own tasks have stopped using it by the release contract, and a stale entry can only be reached by a bug of the application, which then reads memory that still belongs to it.
  2. When the extent must be reused before teardown, the task invalidates locally with `TLBI ASIDE1` (not broadcast) on each core that runs a task of the application, by an inter-processor request to exactly those cores, and skips the cores that run other applications. The cores are known from the application's affinity masks in the manifest.
  3. The broadcast form `TLBI ASIDE1IS` or `TLBI VAE1IS` is the fallback if the local form cannot be shown correct.
- **Measurement before it is relied on.** The board step that first issues a broadcast invalidation (step 7 below) measures its effect on the control path with the existing scheduler latency harness (`apps/latency`, metrics `wake_idle` and `wake_busy`, and the workload of eight pinned tasks released by the CPU0 timer). The run uses a 10 kHz release period, issues a release every 100 ms from the A55 cluster while the harness runs on the A76 cores, and reports the worst case and the 99.9th percentile of both wake metrics against a run without releases. The result decides between the three forms above; a measurable cost (more than 5% of the 100 µs period at the 99.9th percentile, or any worst case above 10 µs) selects the local form or the deferred reuse.

**Fragmentation.** The mapping stays identity for now, because device DMA uses
physical addresses and a CPU-visible address must equal the address a master
sees. A request therefore needs contiguous physical memory. First fit with
1 GiB alignment for large requests keeps large gaps whole. A request that
fails for fragmentation reports the largest free extent so that the
application can retry with less. The remedy, when fragmentation shows in
practice, is to separate virtual from physical addresses for CPU-only memory:
the tables then map discontiguous 2 MiB extents at consecutive virtual
addresses. Memory that a master reads or writes stays identity mapped, or is
reached by the masters that sit behind the IOMMU (display, G2D, codecs and the
camera) through IOVA mappings, which already give them a contiguous view of
scattered pages. Masters without an IOMMU (the NPU through its own MMU aside, DMA0, xHCI
and the PCIe endpoint) need physical contiguity or a per-transfer scatter list.
This step is not scheduled.

**Zeroing.** The service task zeroes a granted extent in 2 MiB steps at a
priority below the application's and before it signals the reply. At 5 GB/s,
the single-core read rate in [dram-bandwidth.md](dram-bandwidth.md), 1 GiB
takes about 0.2 s. The write rate is not measured and is a board
measurement of the step that adds the call.

## Checks and evidence for each change

Each row gives the checks that change, and how the change is shown: on the
host, then in QEMU, then once on the board.

| Change | Host check | QEMU | One board run |
| --- | --- | --- | --- |
| Planner and layout record | New `checks/memory-layout.py`: record equals the current constants for every current manifest and image; section sizes equal between the two links; every placement obeys the hardware bounds; overlaps and out-of-memory placements are refused | `checks/qemu-os-boot.py` boots the image linked with the record | None. The linked images are byte-identical, which the check shows by hash |
| Heap and stacks from the ledger | `checks/app-region.py` gains heap-sized and stack-sized acquisitions; newlib `sbrk` against a runtime base and limit; guard block faults | Boot with a 1 GiB heap, touch every 2 MiB, free and reallocate, fault below a stack | Large-region OS image with a 1 GiB heap: a pattern written and read back over every 2 MiB, and the first board run of the per-application tables (open items 1, 3 and 5 of the large-regions status) |
| Manifest regions anywhere, window lifted | `checks/sunstone-app-abi.py` with regions above `0x4800_0000` and at address 0; the manifest tool, `identity.c` and `host.py` read the record | An EL0 task reads and writes each region and faults one byte outside it | A run with an INPUT of 256 MiB at `0x5000_0000` uploaded by FEL, EL0 checksum equals the host's, and a neighbouring byte faults |
| Slot 1 per-application page | `checks/app-region.py` model walks slot 1; blocks below `0x4800_0000` are byte-identical to the kernel's; the `device-memory-capacity` golden still matches the kernel table | `checks/region-el0.py` in two spaces | Covered by the run above |
| Runner and backup/restore | `checks/os-cpu-return.py` and `tools/board_images.py` checks with extents above `0x80000000`; a plan above 4 GiB is refused; the time estimate | none | A preserving run of the image above with an 8 MiB OUTPUT and a 16 MiB WORK at `0x8800_0000`: the restored bytes hash equal to the backup |
| NPU pool from the ledger, sized by declaration | `checks/npu-session-runtime.c` and `checks/cpu-npu-geometry.py` with a pool above 48 MiB; the 4 GiB span rule | The existing NPU fixtures with the pool at the new address | One NPU graph run from a 96 MiB pool above `0x4800_0000`, output equal to the 48 MiB pool run |
| Application image area | Link checks: text above `0x4800_0000`, thunks present; boot-table builder golden for the extents; `checks/standalone-boot-loader.py` and `checks/microsd-boot-package.py` with the new extents | `qemu-os-boot.py` with a 100 MiB text | A returning image whose text is 40 MiB, run to a normal return; an EL1 call into the text |
| Run-time requests | `checks/app-region.py` with a TLB model for the acquire and release order (a stale entry is detected); the ledger page chaining; `NO_TABLES` leaves state unchanged | A task acquires, uses, releases and re-acquires from two cores; a use after release faults | One run acquiring and releasing 256 MiB and 1 GiB regions from the A76 and A55 clusters, with the release latency and zeroing rate recorded |

All of these checks and runs use the existing board runner and queue
([board-runner.md](../board-runner.md)). Performance figures come from OS images at the approved top
operating points, not from probe images.

## Migration in small landable steps

1. **Heap from the ledger.** The heap becomes an application region obtained at admission when `heap_bytes` exceeds what is linked, the manifest's cap becomes the ledger's free space, and `runtime/newlib.c` takes the heap base and limit from the region. This has the most application impact for the least risk: heap size is the limit that today fails at link time, the change reuses the large-region ledger and tables that already pass host and QEMU checks, and an image that asks for no heap above the linked size behaves as before. The step also supplies the first board run of the per-application tables.
2. **Planner and layout record**, producing today's constants. Host only, with no image change. (Implemented before step 1.)
2b. **Every application heap from the ledger.** Remove `heap_link_max`: the heap of any manifest is a ledger region when the image maps upper memory, and an image whose decode check fails falls back to the linked heap. This changes the mimus image, so it waits for the qualified board run of step 1 and for a board run of mimus.
3. **Manifest regions placed by the planner, and the 48 MiB window lifted**, with the slot 1 per-application page and the ledger's lowest slot at 1. Stack pool with guard blocks in the same step.
4. **Runner and backup/restore from the record**, with the `preserve` attribute and the time estimate.
5. **NPU pool and GPU buffers from the ledger**, using the hardware bounds table.
6. **Application image area**: renamed application sections, `layout.ld`, boot-table text mapping at EL1 and EL0, stage-2 loader extents.
7. **Run-time requests**: SDK call, ledger pages, table-page extents and the release protocol.

Steps 3 to 5 each depend on step 2, and step 6 on step 3. Step 7 depends on
step 1 and on the tables of step 3.

## Implemented so far

**Layout record (step 2).** `tools/memory_layout.py` holds the addresses and
bounds of the images built today. The manifest tool, the CPU-return host,
`tools/board_images.py` and `tools/boot-slots.py` import them from it, and
`checks/memory-layout.py` compares the record with the numbers of the earlier
images, the linker scripts, `apps/os/identity.c`, `platform/a733/platform.c`
and the importing tools, and with `--image` places a linked image against it.
The linked hello, hello (CPU return) and large-region images are byte-identical
before and after, including the generated manifest tables.

**Heap from the ledger (step 1).** A manifest whose `heap_bytes` exceeds
`heap_link_max` in the record (4 MiB) gets its heap from the ledger. The
threshold exists only so that the image of mimus, whose manifest asks for exactly
4 MiB, and every image without the upper memory map keep their current heap while
step 1 is qualified on a board. It is not a limit and a later step removes it
(migration step 2 below): every application heap then comes from the ledger
whenever the decode check passes, and the linked heap remains only as the
fallback of an image whose decode check fails. The heap from the ledger works
like this:

- The manifest tool appends one more large record, the last, with the flag `SS_LARGE_HEAP`: read-write, no source, no digest, a size of `heap_bytes` plus the OS's 1 MiB share rounded up to 2 MiB. The manifest cap on `heap_bytes` is the 11 GiB of upper memory.
- The image links only the OS's 1 MiB share of the heap, and the build refuses such a manifest unless the image maps upper memory (`-DSUNSTONE_A733_DRAM_DECODE_PROBE=ON`).
- At admission the ledger takes the region, the loader skips it (a heap alone opens no card and authorizes no card pins), and `runtime_heap_attach()` makes it the C library's heap before the C library initializes. Attachment fails, and with it the admission, when the linked heap has already handed out a byte.
- `_sbrk` zeroes each range it hands out of an attached heap, so memory that an earlier run left in DRAM never reaches the application. The cost is one pass over the memory the application takes, and none for memory it never takes.
- Every task of the application starts in its space with no call: a new task takes the address-space tag of the task that creates it (`traceTASK_CREATE` in `FreeRTOSConfig.h` with `os_space_inherit()` in `include/os_space.h`; both the stock SMP scheduler and the partitioned scheduler call it before the task becomes ready). The main task is tagged at admission. A task of the OS is created from boot code or from an OS task, neither of which has a tag, so it stays in the kernel's table. `ss_large_attach` remains for a task that other code created. Neither scheduler prevents this.

A manifest with `heap_bytes` of 4 MiB or less links its heap as before. The
OS core code differs in every image (the admission, the loader loop and
`_sbrk` changed), so those images are not byte-identical; their generated
tables are.

**Runner preservation (step 4).** The runner's DRAM range in the record is now
the FEL reach, `0x40000000` to `0x100000000`, instead of the first 2 GiB, and a
preserving run of an application with a WORK or OUTPUT region above
`0x80000000` is accepted. An extent that reaches or passes 4 GiB is refused, and
the refusal names the extent and the reach. A manifest region of kind work or
output takes the access name `scratch` (bit 4 of the region's access word, which
no OS code reads): `apps/os/host.py` leaves it out of the backup list, while the
CPU-return terminal still cleans it, because the terminal does not depend on what
the runner saves. An input cannot be scratch, since the runner uploads it.
`tools/board_images.py` estimates the transfer of a preserving plan as two passes
of the preserved bytes at the recorded FEL rate (550 kB/s), the runner prints the
estimate before the backup, and `tools/board.py submit --preserve-budget-s N`
refuses a plan whose estimate exceeds N seconds. `checks/runner-preservation.py`
covers the reach, the scratch attribute, the estimate and the budget; six
mutations (the old ceiling, no scratch filter, no budget test, one pass in the
estimate, scratch allowed on an input, no upper bound) each fail it. The board
run listed for this step (an 8 MiB OUTPUT and a 16 MiB WORK at `0x88000000`,
restored bytes equal to the backup) is not yet done.

**NPU pool and GPU buffers (step 5).** What each reaches today, before the change:

| | Memory | Who maps it | What contains the device |
| --- | --- | --- | --- |
| NPU pool | 8 MiB at `0x47400000`, or 10 to 48 MiB of whole blocks ending at `0x47000000` (`OS_NPU_POOL_BYTES`, linker symbols `__os_npu_pool_start` and `__os_npu_pool_end`, bound into the session record by `apps/os/host.py` and compared by `apps/os/admission.c`) | The CPU: EL1 read-write write-back blocks of the kernel table, none in an application's space. EL3: cleaned by the normal-return terminal, which parks on an extent it does not know (`apps/os/cpu-return/terminal.patch`). The NPU: only through its own page tables, built once at admission in an arena inside the pool (`drivers/npu/a733_npu_runtime.c`, `place`; `a733_npu_memory.c`; `a733_npu_mmu.c`) | The NPU MMU. The tables map the bootstrap stream and each slot's command, constant, read and write buffers at 4 KiB granularity, with an unmapped guard page after each; they do not map the arena, the runtime's own tables, the CPU-only staging area or any byte of the pool the layout leaves unused. The write bit follows the buffer's role. The graphs run as compiled |
| GPU buffers | Manifest regions with a GPU access bit, 2 MiB aligned (`tools/sunstone-manifest.py`), in the window or, since step 3, in the slot 1 area | The CPU: a GPU-written region has no writable alias (EL1 read-only blocks, `platform/a733-smp-cached/memory.c`) and an application sees it through its own space. The GPU: only through its own MMU, one mapping per lease (`platform/a733/gpu_queue.c`) | The GPU MMU maps the leases the application asked for, rounded to 2 MiB; the admission refuses a region that overlaps the image, the device pools or another lease and one that is not identity mapped write-back |

The GPU buffers already come from the ledger's placement (step 3: the manifest
tool places them, the ledger holds them for the application at admission), and
the GPU admission accepts any 2 MiB aligned region below 2^36, so no GPU change
was needed.

The NPU pool changes only where its memory comes from. An image built with
`SUNSTONE_OS_NPU_POOL_BASE` places the pool in the slot 1 area instead of below
`0x47000000`: any whole-block size from 10 to 768 MiB, below 4 GiB so that the
device memory class (DMA32) and the one-span rule of the table builder stay
true. The linker symbols still carry the address, so the session record, the
runner's preservation list (the runner reaches 4 GiB since step 4) and the
terminal need no second source. At admission `os_large_prepare` holds the pool
in the ledger for the kernel (`APP_REGION_KERNEL_OWNER`) before any region of the
application, so a manifest region, the heap or a stack cannot overlap it and no
application space maps it; `npu.c` refuses to start the NPU unless the ledger
holds exactly that extent. The platform maps the pool's blocks at EL3 for the
terminal's clean, as it does for manifest regions in slot 1, and the terminal
accepts a pool in the slot 1 area (block aligned, 10 to 768 MiB). Nothing about
the table builder, the loader, the graph formats or the buffers' layout inside
the pool changes, and an image without the option is unchanged.

Size and place are not choices. `tools/npu-pool-plan.py` takes the slot count
and the bounds for graphs registered later from the manifest's npu section (as
`apps/os/npu.c` does at admission), runs the production runtime's own reservation
for the linked models (`checks/npu-pool-size.py`, which builds the runtime for the
host), rounds up to whole 2 MiB blocks (at least 10 MiB), and places the pool at
the top of the slot 1 area, refusing a layout in which a region the manifest tool
placed there would overlap it. It prints the two cmake arguments. For the default
`npu-graphs` manifest and the `models-a1.json` list it gives 10 MiB at
`0x78600000`. `checks/npu-pool-plan.py` covers the rules with a stand-in for the
measurement and fails under five mutations of the tool.

Evidence: `checks/npu-pool-isolation.py` builds the tables with the production
memory layer at the legacy pool, a declared-size pool, two pools in slot 1 and
one above 4 GiB, walks them with an independent walker over the whole 32-bit
device space, and requires exactly the declared pages (addresses, physical
pages, write permission by role), no page of the arena, the staging area, the
unused bytes or the 2 MiB beside the pool, and no page after a buffer. Four
mutations of the builder (a page past each buffer, every page writable, the
high physical bits dropped, a buffer allowed over the tables) each fail it.
`checks/app-region.py` shows the kernel's hold: no request overlaps the pool,
the application's space faults at its first and last byte, EL1 keeps access, and
dropping that rule fails the check. The linked terminal model
(`checks/os-cpu-return.py`) runs 16 cases on images with the pool at 8 MiB, 96 MiB
at `0x60000000`, the 768 MiB maximum and the last 10 MiB of the area, and the old
terminal parks on the ledger image. Not covered on the host: the EL3 table
entries that `memory.c` writes for the pool at boot (the model does not execute
that code) and the NPU itself with a pool in slot 1; both need the board.
`checks/memory-layout.py` ties the bounds in the build, the header, the terminal and the platform to the record.

### Predicted board run of the NPU pool in the slot 1 area

Hypothesis: an `npu-graphs` image built from the recipe of the passing run `1793599845892192862-30dc9323` (seven
single-operation graphs, `apps/npu-graphs/models-a2.json`, 852 MHz at 800 mV, kind `npu`), with the pool that
`tools/npu-pool-plan.py` places (10 MiB at `0x78600000`, the top of the slot 1 area, where that run's pool was 8 MiB
at `0x47400000`), runs every graph through that pool and returns as that run did. Nothing else differs: the session
record names the placed pool, the OS holds it in the ledger for the kernel, the platform maps its blocks at EL3, and
the terminal cleans it.

Condition: one run on port 3-1, no card, development DRAM mode.

- **Expected.** Normal return, the receipt of that run's `expect.json`: seven records each with status `match` and no
  differing byte, `completed` and `matched` equal to 7, the SDK closed. The per-graph times equal the earlier run's within
  its spread, because the pool is write-back memory in both places and the maintenance is the same.
- **A different result and what it means.**
  - No receipt and no `RETURN_ROM` breadcrumb after the graphs ran: the terminal parked, so the EL3 entries of the pool
    (`platform/a733-smp-cached/memory.c`) are missing or wrong. The host model does not run that code.
  - The OS refuses the NPU at its start (the failure record names the NPU site): the ledger did not hold the extent
    (`os_large_kernel_reserved`), which is a decode-check or capacity failure, and no job ran.
  - A job faults or the NPU hangs: the NPU's reach of memory above `0x48000000` differs from what its page tables say.
    The host check shows the tables are right, so this would be a property of the interconnect and would be new.
  - Every graph `mismatch` with the same differing count: the device read or wrote another copy than the CPU, a cache
    maintenance or attribute difference of the slot 1 blocks.

### Predicted board run of the NPU pool below 0x48000000 with the upper memory mapped

The first run of the pool in the slot 1 area (entry `1793652334060640370-11f14671`, the image built from commit `8c4508c3` with the
pool at `0x78600000`) ended at the watchdog. The only failure it recorded is the supervisor's cutoff test in
`os_cpu_return_supervise` (`apps/os/cpu-return.c`, detail `0x73c`): the application had not asked for the return by the session
deadline minus the 5 s return reserve, about 55 s after launch. No NPU, board or application failure came first, and the board
live test never failed, so no service's progress window expired. The passing run that this one repeated took 5 s. That run was built without the DRAM decode check, so the new image differs in two ways, not one: the pool
address, and the check, which maps the upper memory at EL1, gives the application an address space and takes its main stack from
the ledger. The prediction of the first run named only the pool.

Hypothesis: the pool address is the cause. A pool at `0x78600000` leaves the NPU service waiting for a job that does not complete,
or for a stop that does not finish, while the same image with the pool below `0x48000000` completes. The control is the same
source tree and recipe with one change, `SUNSTONE_OS_NPU_POOL_BASE` empty: a 10 MiB pool ending at `0x47000000`, which is
`0x46600000..0x47000000`. The decode check, the application space, the main stack in the ledger and the A76 main core stay as in the
failed image. A build of the failed recipe from the same tree reproduces the failed image byte for byte, so the control differs
from it in the pool symbols and the code they select (the ledger reservation, the start test of the reservation, the EL3 pool
entries and the terminal's clean of a sized pool).

Condition: one run on port 3-1, no card, development DRAM mode, 852 MHz at 800 mV as before.

- **Hypothesis true.** Normal return with the receipt of `expect.json`: seven records with status `match` and no differing byte,
  `completed` and `matched` equal to 7, the SDK closed, `RETURN_ROM` as the last breadcrumb, about 10 s of launch.
- **Hypothesis false.** The same watchdog end: last stage `EL3_STOP`, first failure `0x73c`, about 55 s of launch. The cause is then
  in the composition of the decode check with the NPU service (the application space and TTBR0 switching, the ledger main stack, or an
  application space on core 6 beside the NPU service on core 1), and the next image must name what the application waits for.

Host evidence before this run. The image's own start code (`checks/os-npu-start.py`, with the kernel tables seeded as the decode check
leaves them) accepts the session of the failed run and starts the NPU service with the pool at `0x78600000`. An application with no
regions, a main stack in the ledger and an application space starts and finishes on core 6 and on core 2 in the emulated board.
No host model contains the NPU's reach of memory, so neither can show a pool-address failure.

**Result.** The control (entry `1793663710033568998-bec104fe`, pool at `0x46600000`) ended exactly as the failed image did: last stage
`EL3_STOP`, first failure `0x73c` at the supervisor's cutoff, no earlier failure. The hypothesis is false. The pool address is not
the cause, and the cause lies in running the NPU service in an image whose decode check is on.

### Predicted board run that names what the session waits on

A session that reaches the supervisor's cutoff now records what it was waiting on. The NPU retirement wait in `ss_finish`, the
only wait of the application that lasts until the cutoff, gives up one second earlier with failure `0x75f`, whose value is the set of
unmet retirement conditions (`OS_NPU_STATE_*` in `apps/os/npu.h`). The supervisor's cutoff failure `0x73c` carries the return
address of the application's latest `ss_progress` call as its value and the same conditions in the auxiliary field of its site.
`tools/a733-crumbs.py` names both.

Hypothesis: the session waits in `ss_finish` for the NPU to retire. The run uses the control image (pool at `0x46600000`, decode
check on) built from the tree with this change. Nothing else differs from the control.

Condition: one run on port 3-1, no card, development DRAM mode, 852 MHz at 800 mV.

- **Hypothesis true.** First failure `0x75f` in the application group, about 54 s after launch, with a value that lists the unmet
  conditions. Which of them are unmet says where retirement stops: the report not published points at the client task or the
  close, the report published but `os_npu_stopped` false points at the bootstrap's local stop or the interrupt state.
- **Hypothesis false, application elsewhere.** First failure `0x73c` with a progress site inside the graph loop or the close loop of
  `npu_graphs_main`, which the decoder reports as an address to symbolize against the image.
- **Hypothesis false, application never ran.** First failure `0x73c` with progress site `0`, and the NPU conditions show whether
  the service started. The main task did not reach its first `ss_progress` call.
- **Normal return.** The change alters timing and the cause is a race. The receipt then decides.

**Result.** The run (entry `1793664765522129781-2eeb4b7e`) ended at the cutoff with first failure `0x73c`, not `0x75f`, so the
session was not waiting in `ss_finish`. The record names the wait. Its value is `0x41089608`, the return address of the
`ss_progress` call in the first loop of `main_entry` (`apps/os/application.c`), which waits until `ss_live()` is not zero. That
loop never ended, so the application's main entry never ran. `ss_live()` stays zero while `ss_grant()` is null or the USB service
task has not marked the transport ready (`os_application_usb_ready`, called first thing by `board_usb0_task` in this profile). The
NPU state in the site's auxiliary field, `0x41cb`, says the service started and became active, the SDK was never opened, and no
report was published. The next image must say which of the two terms of `ss_live()` is unmet.

**Cause.** `main_entry` tests `ss_live()`, finds the application not live, and waits for the transport with
`application_wait_until(deadline - 10 ms)`. A wake-up (`os_wake`) reaches only a task that has registered its wait. When the USB
service task marks the transport ready after the test and before the registration, the wake-up finds no waiter, and the main
task sleeps to the session deadline, which the supervisor's cutoff then ends. The wait also extended the application's progress
window to the deadline, which is why no service window expired. The emulated board reproduces it
(`usb-ready-between-test-and-wait` in `checks/qemu-os-boot.py`): on the earlier code the application never enters. The wait now
registers first and tests `ss_live()` again once registered, so the event is either seen by that test or delivered.

### Predicted board run of the NPU pool in the slot 1 area, repeated with the wait fixed

Hypothesis: the failed run of the pool at `0x78600000` (entry `1793652334060640370-11f14671`) was this lost wake-up, and the pool is
not at fault. The same recipe built from the tree with the wait fixed returns normally with the receipt of `expect.json`: seven
graphs `match`, `completed` and `matched` equal to 7, the SDK closed, `RETURN_ROM` last, about 10 s of launch. The image differs from
the failed one in the fix and in the cutoff diagnostics. If the run ends at `0x73c` again, the record's progress site and NPU state
say where the application waits, and the lost wake-up was not the whole cause.

### Predicted board run of the heap

Hypothesis: with the OS image built with the address-test decode check
(`sunstone-a733-os-heap-test`, four modes), upper memory at `0x8000_0000` is
mapped, the ledger grants the application a 1 GiB heap there, and its EL0 tasks
reach exactly that region through its own tables.

Condition: a CPU-return run on one board in development DRAM mode (no
preservation claim: the runner does not back up the heap), the image's default
CPU grant, no card.

- **Mode 0 (use the heap).** Returns normally. The result equals the file written by `apps/heap-test/host.py expected 0`: heap base `0x8000_0000`, size `0x4000_0000`, a 512 MiB block inside the region that read as zero in every word, a pattern that read back equal over all 64 Mi words, a failed request for the whole region, and a block that was freed and taken again. The decode check passing is a precondition; if it fails the image refuses the application before any task runs.
- **Mode 1 (a task in no application space reads the first heap byte).** The session ends with an application failure record whose fault is a level 1 permission fault on a read at `0x8000_0000`.
- **Mode 2 (the application reads the first byte after the region).** The session ends with a permission fault at `0xc000_0000`, a kernel-only 1 GiB block.
- **Mode 3 (an attached worker writes, the main task reads).** Returns normally with the result of `apps/heap-test/host.py expected 3`.

What the run does not show: the rate at which `_sbrk` zeroes memory (the result holds no time) and the cost of the task-switch hook with the heap's space. Both belong to the OS image at the approved top operating points and are measured by the large-region test and the dram-bandwidth application.

## Risks and open questions

- **Decode check coverage.** The check proves linear decode, not that the memory firewall passes Non-secure accesses or that no other master writes. Upper memory above `0x4800_0000` has the board-recorded result for the address test only; a bus error there resets the board through the watchdog. Step 1's board run is also the first OS-image run of it.
- **FEL address width.** The protocol limit is verified on the host from the pinned tool source (see the bounds table); whether this ROM implements a wider private extension is irrelevant, because the tool cannot send one. The runner check in step 4 refuses higher addresses.
- **Upload time.** An initialized image above tens of megabytes costs minutes per run at 0.55 MB/s. Data of that size belongs in INPUT or large regions.
- **Start-up order.** The heap of the ledger replaces the linked heap only if the linked heap has handed out no byte. A static constructor that calls `malloc` before admission therefore refuses the application at admission (the QEMU case `heap-after-early-sbrk` shows the refusal and that the application does not start). Step 2b removes the dependency by running the constructors after the heap is attached.
- **Stale memory.** Every extent handed out must be zeroed, and boot-time zeroing of the heap and bss has to be measured against the supervision window.
- **EL1 execution of application text.** The kernel tables must map it, and the application's EL1 worker functions have to be listed in the layout record. A function at an unmapped address faults at EL1.
- **Branch thunks and code model.** The thunk check must run for the application's real size. An application that places more than 4 GiB of bss in link-time symbols is outside the code model; it uses the heap or a region instead.
- **Fragmentation of an identity-mapped space.** No remapping exists, so a long-running application can fail a large request that enough free memory in total could serve.
- **ASIDs.** The pool hands out 255 and never returns one ([large-regions status](../status/large-regions.md), items 7 and 8). Release of an application space is out of scope here.
- **Real-time cost of invalidation.** The effect of a TLB invalidation on other cores' worst-case latency is unmeasured. Step 7 measures it at 10 kHz with the scheduler latency harness before any broadcast form is used (see the subsection on the real-time cost).
