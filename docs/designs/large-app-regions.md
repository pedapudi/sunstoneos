# Design: large application regions in upper memory

Status: **implemented** in the OS image and checked on the host and in QEMU: the region ledger, the application page-table builder, non-global kernel entries, the TTBR0 task-switch hook, the loader and the `ss_large_*` calls. **Qualified on the board** with a probe image (isolation, 1 GiB and 2 MiB block walks, address-test decode result, SD read rate), with an OS image at the CPU-return top points (512 MiB file load, task switch, read bandwidth) and with a 6 GiB region and its isolation faults. The device grant is an **interface only** and run-time requests are open ([application-memory-layout.md](application-memory-layout.md)). Readers are assumed to know AArch64 stage-1 translation (Arm ARM DDI 0487, D8), the FreeRTOS SMP port and [memory-architecture.md](memory-architecture.md).

An application needs memory for data sets that are far larger than the 48 MiB application window: a 730 MB speech model, hundreds of megabytes of weights streamed per chunk. The board has 12,288 MiB, and the DRAM above `0x8000_0000` (11 GiB) is reachable at EL0 only through a region. An application declares a region of up to several GiB in that memory. The OS reserves it, maps it at EL0 for that application only, and fills it from the microSD card. [The status page](../status/large-regions.md) lists open work; the board runs are evidence records in [the memory-dma](../evidence/memory-dma.jsonl) and [storage](../evidence/storage.jsonl) files, indexed under "Qualified figures".

## Decisions

| Question | Decision |
| --- | --- |
| Who owns upper memory | The unified memory ledger in `kernel/umem/umem.c` ([unified-memory.md](unified-memory.md)). The OS image admits every region, the NPU pool, the heap and the main stack through it. `kernel/appmem/app_region.c` keeps the space builder and the region records it reads, and its own ledger serves the host checks and the probe images. |
| Isolation between applications | Each application has its own address space: a copy of the kernel's level-1 table plus the application's regions, selected by TTBR0 with the application's ASID. |
| Isolation from DMA masters | A region is reachable by a master only through an explicit device grant (interface below). No master is granted any region at load or at start. |
| Mapping | 1 GiB blocks for every 1 GiB slot that a region covers whole; 2 MiB blocks for the rest. Regions of 1 GiB or more are placed on a 1 GiB boundary. |
| Memory type | Normal write-back, write-allocate, inner shareable (MAIR index 1), never executable. |
| Access | Read-only or read-write at EL0, as declared. Only the OS writes the region during load, through the kernel's table. |
| Source | A file on the FAT or exFAT volume, or a raw extent of the card, read through the existing card driver and file system port. A SHA-256 digest of the content is required. |


## What exists

- The boot tables map `0x8000_0000`–`0x3_4000_0000` with eleven 1 GiB blocks at EL1 only, when `a733_dram_decode_check()` passes ([memory-map-12g.md](memory-map-12g.md)). An image that links the address-test definition (CMake option `SUNSTONE_A733_DRAM_DECODE_PROBE`) maps it; the default definition returns 0 and maps nothing, so the ledger has no usable ranges.
- The boot tables are one set, built at EL3 with the MMU off and installed in TTBR0 on every core. Every EL0 task shares them, so an EL0 task can reach any address that has an EL0 mapping; the 48 MiB application window `0x4400_0000`–`0x4700_0000` is shared by all EL0 code ([memory-protection.md](../memory-protection.md)). Per-application root tables add the regions.
- TCR_EL1 has a 39-bit input range, a 4 KiB granule and `A1` clear, so the ASID comes from TTBR0. MAIR index 1 is write-back, index 2 non-cacheable (`include/a733_cpu_coherence.h`).
- The SD driver reads with the controller's internal DMA engine in requests of up to 128 KiB below 4 GiB ([the SD card reference](../sd-card.md)). The file system port reads a FAT or exFAT file read-only ([filesystem.md](../filesystem.md)).
- The board runner uploads at about 0.55 MB/s, so 730 MB takes about 22 minutes by upload.

## Ownership and isolation

The ledger is the only allocator of upper memory. `app_region_ledger_init()`
receives the ranges that the decode check passed. With no ranges every request
fails with `APP_REGION_NO_SPACE`, so an image whose decode check fails cannot
hand out unmapped memory. `app_region_acquire()` rounds the size up to 2 MiB,
places it, and records the owner. It refuses a zero or overflowing size, a
write-only access, a full ledger (its storage holds every declared region) and
a request that no free gap holds. A refusal changes nothing.

Isolation rests on the tables, not on a software check:

1. `app_space_build()` copies the kernel's level-1 table into the
   application's root. Slots 0 and 1 (MMIO and the first gigabyte of DRAM)
   stay byte-identical, so every application keeps the kernel's mappings.
2. In slots 2 to 12 (upper memory) the copy keeps the kernel's EL1-only block
   except where the application owns bytes. There the block, or a level-2
   table, gives EL0 read access (AP[2:1] = 0b11) or read-write access
   (0b01).
3. A task of application A runs with A's TTBR0. It faults on every byte of
   another application's region, on every byte of upper memory it does not
   own, and on the byte after its own region (permission fault), because the
   kernel's EL1-only entry is the only mapping there.
4. EL1 code (drivers and the loader) keeps read access to all upper memory
   under every space, and write access under every space except one whose
   region is read-only: stage-1 permissions have no encoding for EL1 write
   with EL0 read-only. The loader therefore writes a region before the
   application's first task runs, while the kernel's table (ASID 0) is
   installed on the loading core.

**Non-global entries.** Every upper-memory entry carries `nG` and therefore an
ASID. The kernel's own entries use ASID 0. If the kernel's entries were
global, a TLB filled by a kernel access would hold a global entry that
matches the application's addresses under every ASID, and the application
would receive the kernel's EL1-only permission instead of its own mapping.
The reverse order creates two matching entries, a TLB conflict
(Arm ARM D8, "TLB conflict abort"). The host check shows both outcomes with a
TLB model, and `app_space_build()` refuses a kernel table whose
upper-memory entries are global or are not the expected blocks. The cached
table builder sets `nG` through `A733_EL1_DRAM_BLOCK`. With `nG` set no TLB maintenance is
needed at a context switch.

**Context switch.** The scheduler adapter already calls
`portTASK_SWITCH_HOOK(task)` after it selects the next task
(`kernel/scheduler/freertos.c`). The image defines the hook to read the
application space of the incoming task from its application task tag
(`configUSE_APPLICATION_TASK_TAG`) and, when it differs from the core's
current space, write TTBR0_EL1 and execute `ISB` before the exception return.
No scheduler file changes. The cost is one compare per switch and, for a
switch between spaces, one system-register write and one `ISB`; the board figure is under "Qualified figures".

**Release and later requests.** Releasing a region or granting one after the
application started rewrites a level-1 entry in the application's root. The
OS writes the entry invalid, executes `DSB ISH` and `TLBI ASIDE1IS`, `DSB ISH`,
writes the new entry, and executes `DSB ISH` and `ISB`, as the Arm ARM
requires for a break-before-make change of a block descriptor. The static
path, where every region is granted before the main task starts, needs none
of this.

## Page tables and TLB reach

A region is mapped with the largest blocks its position allows:

| Region | Leaf entries |
| --- | --- |
| 3 GiB at a 1 GiB boundary | three 1 GiB blocks, no extra table page |
| 1.5 GiB at a 1 GiB boundary | one 1 GiB block and 256 2 MiB blocks, one level-2 page |
| 102 MiB inside a slot | 51 2 MiB blocks, one level-2 page |

`app_region_acquire()` places a region of 1 GiB or more on a 1 GiB boundary
when a free one exists, because every whole slot then costs one level-1
entry. Otherwise it uses the smallest free gap, which keeps large gaps
whole. A space needs one root page plus one level-2 page per partly covered
slot: at most two pages per region, 12 KiB for a region of any size.

A weight-streaming pass over hundreds of megabytes touches one 2 MiB block
per 2 MiB read at worst, and one TLB entry per gigabyte in the aligned case.
The descriptors do not set the contiguous bit: the Arm ARM allows a TLB to
coalesce sixteen contiguous level-2 entries, but no pinned source states that
the Cortex-A76 does, so the design claims no benefit from it.

Table memory comes from one page arena of 4 KiB pages. A build that does not
fit leaves the arena and the space unchanged (`APP_REGION_NO_TABLES`). The
arena reserves 16 pages (64 KiB); a space needs at most `APP_SPACE_PAGES` (13).


## Cache attributes

Regions are Normal write-back write-allocate, inner shareable, execute-never
at both levels: the descriptor of `A733_EL1_DRAM_BLOCK` with the
application's access bits. Inner shareable matches
every other DRAM entry of the kernel's tables, so the cores of both clusters
see one coherent copy. The loader writes through the cacheable
mapping and cleans nothing, because no master other than the CPU reads the
region during load. A master that is granted the region later follows the
pool rule of [memory-architecture.md](memory-architecture.md): clean before
the device reads, invalidate before the CPU reads what the device wrote.

## Device access (interface only)

The NPU and other masters reach a region only through a grant. The interface
is a pair of calls that no driver uses yet:

- `app_region_device_grant(ledger, device_class, owner, base, bytes,
  direction)` returns a grant record (physical base, bytes, direction,
  generation) when the extent lies inside a region owned by `owner`, inside
  the master's address limit from
  [dma-address-width.md](../dma-address-width.md) (NPU: physical below 2^40
  through the NPU MMU), and not already granted to a device in the opposite
  direction. It returns a refusal otherwise.
- `app_region_device_revoke(ledger, grant)` ends the grant after the driver
  has drained the master.

The ledger keeps one grant table per region. Region release is refused while
a grant exists. `app_region_owner_of()` already answers which application
owns a byte, so a driver can refuse a buffer inside a region without a
grant. The device grant adds a cache duty and an address-width check; it
adds no mapping, because the masters that matter (NPU, GPU) translate through
their own MMUs.


## File format and access

The loader runs in an OS task at EL1 on the core that owns the card. Its
source is one of two kinds, chosen in the declaration:

- **File.** A path on the FAT or exFAT volume that `fatfs_port_mount()`
  mounts (an unpartitioned volume, or the first MBR partition that holds
  one). The file is read-only; FatFs write functions are not linked.
- **Raw extent.** A first sector and a byte length on the card, outside every
  partition and the boot area, read with the same driver. A raw extent
  needs no file system and no write to one.

Both kinds require a SHA-256 of the whole source in the declaration, and the
loader refuses an all-zero digest. The loader reads in requests of 128 KiB
into two bounce buffers below 4 GiB, so the controller fills one while the
CPU copies and hashes the other (a single copy per byte). It stores into the
region through the kernel's table. It reports progress as bytes loaded after
every request and sets the region ready only when the digest matches. A
digest mismatch, a short file, a read fault or an expired deadline marks the
region failed, leaves its contents unspecified and fails the application's
start the way an authentication failure of an input region does.

The controller's descriptor holds the buffer address shifted right by 2, which
would reach `2^34`, but no board result shows a transfer into memory above
4 GiB and [dma-address-width.md](../dma-address-width.md) lists no address
limit for SD, so direct transfer into the region is not used. The bounce copy runs at memory
bandwidth (above 5 GB/s on one A55 core, measured in
[dram-bandwidth.md](dram-bandwidth.md)) and costs under 3% of a read.

The file system port provides `fatfs_port_read(port, path, offset, buffer,
bytes)`, which reads a byte range of an open file so that a file of more
than 4 GiB (exFAT) loads in 128 KiB steps. Its argument checks mirror
`fatfs_port_load`.


## SDK interface

An application declares large regions in an optional section,
`sunstone_large_regions`, aligned to 8 bytes, beside the manifest. The manifest
layout of 552 bytes does not change.

| Field | Bytes | Meaning |
| --- | --- | --- |
| `index` | 4 | Large region number |
| `flags` | 4 | `SS_LARGE_WRITE`: the application may write the region. `SS_LARGE_EXTENT`: the source is a raw extent, otherwise a file |
| `bytes` | 8 | Capacity; at least `source_bytes` |
| `source_bytes` | 8 | Exact length of the source |
| `first_sector` | 8 | Raw extent only |
| `sha256` | 32 | Digest of the source bytes |
| `path` | 256 | File path of at most 255 characters (the FAT32 long-name limit, `FAT32_NAME_MAX`), NUL-padded |

Calls, all plain function calls that are safe at EL0 like the other `ss_*`
calls (the EL1 loader task does the privileged work):

```c
/* Base of large region index, or NULL until it is ready. *bytes receives its capacity. */
void *ss_large_base(unsigned index, size_t *bytes);

/* state: SS_LARGE_PENDING, SS_LARGE_LOADING, SS_LARGE_READY or SS_LARGE_FAILED. */
struct ss_large_status {
    uint32_t state, error;
    uint64_t loaded, total, elapsed_us;
};
int ss_large_status(unsigned index, struct ss_large_status *out);

/* Block until the region is ready or failed, or timeout_us passes.
 * Returns 1 ready, -1 failed, 0 timeout. Woken by task notification. */
int ss_large_wait(unsigned index, uint64_t timeout_us);

/* Put a task of the application, such as a worker it created, in the application's address space. */
void ss_large_attach(void *task);
```

At admission the OS acquires every declared region before the main task
exists, builds the application's space, starts the loader and runs the main
task with that space. The main task waits for `ss_large_wait` as it waits for
`ss_live`. `ss_progress()` calls during a long load keep the
supervision window satisfied through the loader's own progress reports.


## OS integration

The pieces below are implemented in the OS image and checked on the host and in QEMU; [the status page](../status/large-regions.md) lists what is open.

- **Counts.** No tuning constant bounds a count. The usable upper memory is 11 level-1 slots, which bounds the
  usable ranges and the pages of a space (`APP_SPACE_PAGES` = the root and one level-2 table per slot = 12). The
  number of regions is bounded by the memory (each takes a 2 MiB block) and their total by 11 GiB. The ledger
  takes caller storage for as many regions as the manifest declares; the generated tables size it, the loaded
  slots and the waiter list (one entry per task of the application, which is its workers plus the main task).
- **Admission** (`apps/os/large.c`) runs in `os_application_prepare()`. The usable memory is read from the
  installed kernel table, so an image whose decode check failed has none and the application does not start.
- **Task switch.** `traceTASK_SWITCHED_IN()`, defined in the OS `FreeRTOSConfig.h` files only, calls
  `os_space_switch_in()` (`include/os_space.h`). A task carries its space's TTBR0 value in its application tag
  (the OS tags the main task; `ss_large_attach` tags a worker). The hook reads the core's TTBR0, compares it with
  the task's and writes it when they differ. It issues no barrier and no TLB invalidate: the kernel's mappings
  are the same in every space, every upper-memory entry carries an ASID, and the exception return that resumes
  the task is a context synchronization event. Neither kernel needs a change: the default `tasks.c` and the
  partitioned `freertos.c` both expand the macro with names they already have (`pxCurrentTCBs`,
  `portGET_CORE_ID`, the task tag), and both build and boot in the QEMU check.
- **Cost of the hook.** Compiled, the hook is 6 instructions while no space is installed, about 20 when a space is
  installed and the table does not change, and one more (the `MSR`) when it changes. QEMU's instruction time
  (`-icount shift=0`) for 20,000 notification round trips between two tasks of one core is 289.16 ticks per
  round trip in one space and 289.34 with a TTBR0 write at each switch. The difference is 0.18 ticks (7.5 ns) per round trip, 3.8 ns per
  switch, four instructions' time. This is not hardware time. The OS test application runs the same benchmark, so
  the figure on the board is one run away.
- **SDK.** `ss_large_wait`, `ss_large_base`, `ss_large_status` and `ss_large_attach`. There is no call to take a
  region after start: the ledger and the tables are sized from the manifest at build time, so run-time requests
  are backlog (see the status page for what they need).
- **ASIDs.** An ASID comes from `app_asid_alloc()` (`kernel/appmem/app_region.c`), which gives each number out once and
  never takes one back. The bound comes from the hardware: `app_asid_pool_init()` reads `ID_AA64MMFR0_EL1.ASIDBits`
  (bits 7:4; 0 is 8 bits and 2 is 16 bits) and `TCR_EL1.AS` (bit 36). The pool hands out 1 to 65535 only when the
  hardware has 16 bits and AS is set, and 1 to 255 otherwise. The reason is that with AS clear the hardware ignores the upper
  byte of the TTBR0 ASID field and two spaces would share a number. The Cortex-A55 and A76 support 16 bits, but the
  boot code installs TCR_EL1 with AS clear (`A733_CPU_COHERENCE_TCR_*`, pinned by the boot-chain checks), so the OS
  hands out 1 to 255. Setting AS there is a change to those boot constants, and the pool then widens by itself.
  The OS has one application and nothing tears a space down, so there is no reuse path; the pool refuses
  (returns 0) when it is exhausted. A teardown that wanted to reuse an ASID would first clear the space's tables and
  execute `DSB ISH; TLBI ASIDE1IS, (asid << 48); DSB ISH`; without that the old space's translations would stay
  live in the TLBs under the number. `checks/app-region.py` covers both widths, the cases where the register and
  TCR disagree, and exhaustion.
- **Kernel mappings.** Every per-application root is a copy of the boot root: slot 0 (MMIO) is copied and slot 1
  points to the same level-2 table. The code, data, stacks and devices that EL1 runs on between the `MSR` and the `ERET` are therefore the same global (nG = 0) entries in every table. The upper-memory slots carry nG = 1 and an ASID
  and are touched by EL1 only in the loader and drivers, never in the scheduler.
- **Reading the boot root.** Admission copies the boot root from the platform (`os_kernel_l1_head`, `include/os_space.h`)
  and never loads from the table's pages. The A733 tables sit in the monitor's 2 MiB block at 0x42000000-0x421fffff,
  which the EL1 table leaves unmapped (`platform/a733-smp-cached/memory.c`: "Secure monitor and tables remain
  inaccessible at EL1"). The hardware walks them, but a load through `TTBR0_EL1` faults (level-2 translation fault,
  ESR 0x96000006, seen on a board in the CPU-return profile at `os_large_prepare`). The A733 platform copies slots 0 to
  15 into ordinary memory when the tables are complete and refuses to finish if any slot from 16 is nonzero. The
  QEMU OS platform places its tables alone in an unmapped 2 MiB block at 0x41c00000 the same way, so every QEMU case
  that admits a region fails with the same ESR if admission loads from the table.
- **Loader** (`apps/os/large_load.c`): one EL1 task reads each region's FAT32 file (or raw extent) in 128 KiB
  requests through two buffers, copies into the region through the kernel table, hashes, and sets READY only when
  the digest matches. The application's waiters are woken by task notification. A missing file, a failing device
  and a digest mismatch set FAILED with the loader's status, and the application returns normally.
- **Device.** The board supplies `os_large_device()`, which runs in the loader task. On the A733 it is
  `apps/os/large_sd.c`. That file holds the OS's shared providers (`providers.shared`: counter, locks, clock leases, pins, rails and
  the PMIC that the board-pmic task serves), the read-only SD core (`SUNXI_MMC_READ_ONLY`, so no write or erase command
  is reachable; `checks/sd-read-only.py --elf` passes on the image) and the descriptor DMA of `a733_mmc_platform`
  with its 33 descriptors in a lease of the uncached pool. `os_large_sd_pins()` authorizes PF0 to PF6 at boot when the
  image declares regions. The loader task owns the platform, identifies the card in profile 2, and
  `os_large_device_close()` restores the clock, the pins and the rail lease after the last region. A failure at any step
  retains the platform and returns no device. The QEMU board supplies a RAM disk and four device faults.
- **Checks.** `checks/qemu-os-boot.py` cases: loaded (the region reads back at EL0 as the file), file absent
  (reported, normal return), one flipped bit (digest mismatch, normal return), a task outside the space faults at the
  region base with a level 1 permission fault, a device that is present and fails (metadata, request start, mid-stream,
  absent), switch cost, and regions beyond the board's memory (no start).
- **Test application** `apps/large-region-test` (`SUNSTONE_APPLICATION=large-region-test`, needs
  `-DSUNSTONE_A733_DRAM_DECODE_PROBE=ON`): a 1 GiB region loaded from `/sunstone-test/sunstone-region-test.bin`, the
  switch benchmark, and read passes with the vector loads of `apps/dram-bandwidth/kernel.c` (a 40 MiB window ten times and
  the full gigabyte three times), timed with the 24 MHz counter. Without the file it reports why and returns.


## Host checks

`checks/app-region.py` compiles the ledger and the table builder with the sanitizers and walks the tables with a separate model of AArch64 stage-1 translation (4 KiB granule, 39-bit input, levels 1 and 2, access flag, AP and execute-never bits; the model aborts on a descriptor bit it does not model).

It covers:

- an application reaches every sampled byte of its region at EL0 and a second application and the kernel's EL0 view fault on the same bytes;
- bytes below and above each region fault in every space;
- a read-only region faults on an EL0 write while the kernel's table gives EL1 write access;
- block counts and page counts (three 1 GiB blocks for 3 GiB, one 1 GiB block plus 256 2 MiB blocks for 1.5 GiB, 51 2 MiB blocks for 102 MiB, with 1, 2 and 2 pages);
- kernel mappings identical in every space;
- refusals (too large, zero, write-only, overflow, full ledger) that leave the ledger unchanged, exact-fit success and release restoring memory;
- an empty usable list;
- an undersized table arena leaving the space unbuilt;
- a TLB model with global and ASID-tagged entries that shows no conflict and no stale permission with non-global upper entries and both failures with global kernel entries.

A mutation that gives read-only regions write permission fails the check.

## Qualified figures

Each row points to an evidence record by id; the records hold the conditions and the full measurement sets.

| Run | Figure | Record (file) |
| --- | --- | --- |
| Memory qualification image, window scan and page signature over `0x4400_0000`–`0x3_4000_0000` | no alias, no beacon; scan 0.555 s, signature 0.975 s | `1793600664425885881-a871e54d` (memory-dma) |
| Probe image at boot clocks (494 MHz), 1 GiB region, three address spaces | isolation faults as designed; L2 TLB refills 3 per pass with 1 GiB blocks and 513 with 2 MiB blocks; 3.79 bytes per cycle in every mapping | `1793603354809199340-a4ed0f70` (memory-dma) |
| SD read, 160 requests of 128 KiB, profile 2 | 21.9 MB/s per request, 19.7 MB/s sustained | `1793602100319385552-a0551753` (storage) |
| FAT32 volume inspection | 1,907,437 clusters of 32 KiB, 14,912 FAT sectors, FSInfo hint equals the counted free clusters | `1793602559946667231-3a060254` (storage) |
| OS image, 512 MiB file load | 24.386 s, 22.0 MB/s, digest matched (reading 23.253 s, copy 0.154 s, hashing 0.979 s) | `1793633314510847134-f446c660` (memory-dma) |
| OS image, task switch and read bandwidth | 4.40 us per round trip in one space, 4.36 us with a TTBR0 write at each switch; one A76 core reads 12.79 GB/s (40 MiB window) and 12.66 GB/s (1 GiB) | `1793632829779717086-f7d0d1cf` (memory-dma) |
| 6 GiB region, one A76 core | read 12.66, write 12.16, copy 5.84 GB/s over the whole region; 805,306,368 words verified with 0 mismatches | `1793637177478128324-7b885910` (memory-dma) |
| 6 GiB region isolation | level 2 translation fault below the region, level 1 permission fault above it and from a task outside any application space | `1793638861895715015-a7176fec`, `1793638933466934204-4dc4152d`, `1793638990638144080-9a9aad2d` (memory-dma) |

The loader's load time is bounded by the card: 23.25 s of the 24.386 s is reading, within 12 percent of the 25 MB/s raw ceiling of the 4-bit 50 MHz high-speed bus mode (`SUNXI_MMC_SD_HIGH_SPEED4`, `apps/os/large_sd.c`). The loader reads the 24 MHz counter for its times, issues the next request before it hashes the current piece (`sd_submit` steps the controller until the command is issued), it hashes with the SHA-256 instructions (`drivers/storage/storage_digest_ce.S`) when `ID_AA64ISAR0_EL1` reports SHA2, with the C path otherwise. Copying by DMA straight into the region would save at most 0.15 s and hashing in parallel at most about 1 s; a faster bus mode (SDR104) is the storage task that would help. A region declared without a `sha256` is not hashed.

## Open work

1. Run the SD0 binding of an OS image through the standalone probe's scenarios on a board (`apps/os/large_sd.c`); the OS-image load run above covers the read path.
2. Count TLB refills in the OS image: EL0 cannot read the PMU, so the figure needs a counter grant or an EL1 sampler.
3. Run the partitioned scheduler's `freertos.c` with the hook on a board; it builds and boots in QEMU.
4. Implement the device grant (`app_region_device_grant`, `app_region_device_revoke`) and a driver that uses it.
5. Regions requested at run time, and tearing down a space with ASID reuse: [application-memory-layout.md](application-memory-layout.md).
6. Set `TCR_EL1.AS` (bit 36 of both `A733_CPU_COHERENCE_TCR_*` constants) with the checks and `platform/a733/cpu_continue.c` that pin them, and run the CPU-return images on both clusters, when application teardown or more than 255 applications per session is needed.

## Sources

- Descriptor fields, AP encodings, `nG` and ASID tagging, TLB conflict
  aborts, break-before-make and the PMU event numbers: Arm ARM (DDI 0487),
  which is not pinned in this tree.
- Block placement: the pinned Trusted Firmware-A translation library maps a
  region with a block descriptor when the region covers the whole block and
  its addresses are aligned, and with a next-level table otherwise
  (`lib/xlat_tables_v2/xlat_tables_core.c`, `xlat_tables_map_region_action`).
  The builder follows that rule and adds the 1 GiB placement preference.
- Per-address-space TTBR0 and non-global user mappings follow the convention
  of Linux arm64 (`arch/arm64/mm/context.c`), which is not pinned here.
- Memory attributes and ASID width: `include/a733_cpu_coherence.h`.


## Left out

- Direct SD transfer into the region, until a transfer above 4 GiB passes on
  the board.
- A region larger than the usable upper memory (11 GiB), regions shared by
  two applications, and demand paging.
- The device grant implementation and any driver use of it.
- Preserving a region across a returning run: the ROM return keeps DRAM, but
  the runner's development mode makes no claim about it.
