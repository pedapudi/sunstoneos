# Design: large application regions in upper memory

Status: the region ledger and the application page-table builder are
**implemented** and checked on the host (`checks/app-region.py`). The
non-global kernel entries and the address-test decode check are implemented
and host-checked, and the decode result is board-recorded. The loader, the
context-switch hook and the SDK calls are **design**. No part is qualified on the board. Readers are assumed to know
AArch64 stage-1 translation (Arm ARM DDI 0487, D8), the FreeRTOS SMP port and
[memory-architecture.md](memory-architecture.md).

An application needs memory for data sets that are far larger than the 48 MiB
application window: a 730 MB speech model, hundreds of megabytes of weights
streamed per chunk. The board has 12,288 MiB, and the DRAM above `0x8000_0000`
(11 GiB) has no owner and no EL0 access today. This design lets an
application declare a region of up to several GiB in that memory. The OS
reserves it, maps it at EL0 for that application only, and fills it from the
microSD card.

## Decisions

| Question | Decision |
| --- | --- |
| Who owns upper memory | The region ledger in `kernel/appmem/app_region.c`. It holds the usable ranges and at most 16 regions, each owned by one application. |
| Isolation between applications | Each application has its own address space: a copy of the kernel's level-1 table plus the application's regions, selected by TTBR0 with the application's ASID. |
| Isolation from DMA masters | A region is reachable by a master only through an explicit device grant (interface below). No master is granted any region at load or at start. |
| Mapping | 1 GiB blocks for every 1 GiB slot that a region covers whole; 2 MiB blocks for the rest. Regions of 1 GiB or more are placed on a 1 GiB boundary. |
| Memory type | Normal write-back, write-allocate, inner shareable (MAIR index 1), never executable. |
| Access | Read-only or read-write at EL0, as declared. Only the OS writes the region during load, through the kernel's table. |
| Source | A file on the FAT or exFAT volume, or a raw extent of the card, read through the existing card driver and file system port. A SHA-256 digest of the content is required. |

## What exists

- The boot tables map `0x8000_0000`–`0x3_4000_0000` with eleven 1 GiB blocks
  at EL1 only, when `a733_dram_decode_check()` passes
  ([memory-map-12g.md](memory-map-12g.md)). Every image built today links the
  definition that returns 0, so no image maps this memory yet.
- The tables are one set, built at EL3 with the MMU off and installed in
  TTBR0 on every core. Every EL0 task shares them, so an EL0 task can reach
  any address that has an EL0 mapping. The 48 MiB application window
  `0x4400_0000`–`0x4700_0000` is therefore shared by all EL0 code
  ([memory-protection.md](../memory-protection.md)).
- TCR_EL1 has a 39-bit input range, a 4 KiB granule and `A1` clear, so the
  ASID comes from TTBR0 (8 bits, 255 applications). MAIR index 1 is
  write-back, index 2 non-cacheable (`include/a733_cpu_coherence.h`).
- The SD driver reads with the controller's internal DMA engine in requests of
  up to 128 KiB below 4 GiB ([sd-dma.md](sd-dma.md)). The file system port
  reads a FAT or exFAT file read-only ([filesystem.md](../filesystem.md)).
- The board runner uploads at about 0.55 MB/s, so 730 MB takes about 22
  minutes. No microSD read rate over more than 64 KiB is recorded.

## Ownership and isolation

The ledger is the only allocator of upper memory. `app_region_ledger_init()`
receives the ranges that the decode check passed. With no ranges every request
fails with `APP_REGION_NO_SPACE`, so an image whose decode check fails cannot
hand out unmapped memory. `app_region_acquire()` rounds the size up to 2 MiB,
places it, and records the owner. It refuses a zero or overflowing size, a
write-only access, an owner above the quota of four regions, a full ledger and
a request that no free gap holds. A refusal changes nothing.

Isolation rests on the tables, not on a software check:

1. `app_space_build()` copies the kernel's level-1 table into the
   application's root. Slots 0 and 1 (MMIO and the first gigabyte of DRAM)
   stay byte-identical, so every application keeps the kernel's mappings.
2. In slots 2 to 12 (upper memory) the copy keeps the kernel's EL1-only block
   except where the application owns bytes. There the block, or a new level-2
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
switch between spaces, one system-register write and one `ISB`; the board
measures it (see Measurements).

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
first image reserves 16 pages (64 KiB), which holds four applications with
two level-2 pages each.

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

The file system port gains `fatfs_port_read(port, path, offset, buffer,
bytes)`, which reads a byte range of an open file so that a file of more
than 4 GiB (exFAT) loads in 128 KiB steps. Its argument checks mirror
`fatfs_port_load`.

## SDK interface

An application declares large regions in an optional section,
`sunstone_large_regions`, aligned to 8 bytes, beside the manifest. The manifest
layout of 552 bytes does not change; `sunstone_region_initializers` set the
precedent for an optional section.

| Field | Bytes | Meaning |
| --- | --- | --- |
| `index` | 4 | Large region number, 0 to 3 |
| `flags` | 4 | `SS_LARGE_WRITE`: the application may write the region. `SS_LARGE_EXTENT`: the source is a raw extent, otherwise a file |
| `bytes` | 8 | Capacity; at least `source_bytes` |
| `source_bytes` | 8 | Exact length of the source |
| `first_sector` | 8 | Raw extent only |
| `sha256` | 32 | Digest of the source bytes |
| `path` | 48 | File path, NUL-padded |

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

/* Request a region after start with the same record. Returns 0 on success
 * or a negative app_region_status; the call returns after the load begins. */
int ss_large_request(const struct ss_large_region *record);
```

At admission the OS acquires every declared region before the main task
exists, builds the application's space, starts the loader and runs the main
task with that space. The main task waits for `ss_large_wait` as it waits for
`ss_live` today. `ss_progress()` calls during a long load keep the
supervision window satisfied through the loader's own progress reports.

## Measurements the design fixes

The implementation reports these on the board:

- Load time and bytes per second, from the loader's progress record.
- Read bandwidth from the region by one A76 core and by several cores,
  against the `apps/dram-bandwidth` single-core and multi-core figures
  (12.69 GB/s one A76 core on a 40 MiB buffer).
- TLB refill counts per pass (PMU events L1D_TLB_REFILL 0x05, L2D_TLB_REFILL
  0x2D, and DTLB_WALK 0x34 where the core counts it), with 1 GiB and with
  2 MiB blocks.
- Cost of the TTBR0 switch at a context switch between spaces.
- A task of a second application that reads and writes the region faults with
  a permission fault, recorded by the OS fault capture.

## MicroSD read rate (prediction before any measurement)

The card runs in profile 2: four data lines at 50 MHz, 3.3 V, high speed.
Condition: back-to-back CMD18 reads of 128 KiB with the descriptor-DMA path
and the card's sequential mode.

- Bus time per 512-byte block is 1,042 clocks, 20.84 µs
  ([sd-dma.md](sd-dma.md)), a ceiling of 24.6 MB/s.
- The one recorded DMA transfer, 64 KiB in 3,332 µs, shows 665 µs beyond its
  bus time of 2,667 µs for command, the card's first-block latency and
  completion. That equals 19.7 MB/s for a single request.
- A 128 KiB request spreads the same overhead over twice the data: 5,334 µs
  of bus time plus about 665 µs, 6.0 ms, 21.8 MB/s. A card that streams
  without a per-request gap approaches the ceiling.

Prediction: 20 to 23 MB/s for 128 KiB requests, so 512 MB loads in 22 to 26
seconds and 730 MB in 32 to 37 seconds, against 22 minutes by upload. The
copy and the SHA-256 overlap the next request; hashing at 100 MB/s or more on
one core keeps up with this rate. If hashing ran after each read instead,
the rate would fall to about 17 MB/s. SDR104 (profile 3) is predicted at 50
to 60 MB/s and is not qualified.

## Predicted upper-memory decode result

Condition: the memory qualification image maps 12,224 MiB from `0x4400_0000`
and runs its window scan, data-line test, address-line test and page
signature over every 2 MiB block of that map, in development DRAM mode on
port 3-1, with a work budget of 18 seconds
([memory-qualification.md](../memory-qualification.md)). The page signature
stores each page's address, combined with a seed, in the first line of the
page and then reads every page back, so an alias of any two pages in the
whole map, including every 1 GiB slot of upper memory, is a failure.

Evidence for the prediction:

- DRAM training on both boards reports 12,288 MiB and passes two 4 KiB page
  comparisons ([memory and DMA status](../status/memory-dma.md)). That shows
  the training sees the capacity; it does not show linear decode at every
  address.
- [memory-map-12g.md](memory-map-12g.md) maps the range as 1 GiB blocks and
  lists the image's address test as the evidence that linear decode needs.
- The measured cost estimates for the scan and signature are 1.5 s for the
  signature and a similar time for the scan
  ([memory-qualification.md](../memory-qualification.md), time budget).

Prediction: the run returns normally with application status 0, the window
scan finds no beacon value, the page signature finds no page that holds another
page's signature, and the scan and signature complete within 5 seconds. A pass
shows linear decode over `0x4400_0000`–`0x3_4000_0000` for this boot. It does
not show that no boot stage left a bus master writing there; the launch
evidence prerequisite in [memory-map-12g.md](memory-map-12g.md) stays. A
failure appears as a recorded alias (an address decode fault), as a bus error
that resets the board through the watchdog (a firewall-protected range), or as
a run that exhausts its budget.

## Predicted region test on the board

Condition: the read-only SD probe takes a 1 GiB read-write region from the ledger (first fit at a
1 GiB boundary, `0x8000_0000`), builds three address spaces (the owner, a second application with no
region, and a view of the owner's region with 2 MiB blocks only), runs EL0 routines in them on the boot
Cortex-A55, and counts TLB events with the PMU. The routines are position-independent code in a read-only
executable window; faults return to the EL1 caller through its own vectors. Port 3-1, development DRAM mode.

Isolation prediction (from the translation rules in this design and the QEMU fixture `checks/region-el0.py`):

- The owner reads and writes its first word, a middle word and its last word at EL0 and reads a 4 MiB strided
  fold that equals the EL1 fold of the same bytes. Each ends in an SVC (exception class 0x15).
- The owner's access to the first byte after the region faults with exception class 0x24 and a level 1
  permission fault (fault status 0x0d), because the next gigabyte is a kernel block that allows EL1 only. The
  byte before the region lies in a 2 MiB kernel block and faults with a level 2 permission fault (0x0e).
- The second application's read and write of the region fault the same way; its write sets the write bit of the
  syndrome; its access to its own stack window succeeds.
- The 2 MiB-block view faults on the byte after the region exactly as the owner does, with a level 1 permission
  fault (0x0d): the region ends on a gigabyte boundary, so the next byte is in a whole-gigabyte kernel block. A
  region that ends inside a gigabyte faults there at level 2 (0x0e).

Read-bandwidth prediction. The control pass reads a 40 MiB window of the region ten times (two warm-up
passes) with the 128-byte vector loads of `apps/dram-bandwidth/kernel.c`. One Cortex-A55 core reads 5.36 GB/s
in that application at CPU-L 1794 MHz on a 40 MiB buffer (the
[memory-dma evidence](../evidence/memory-dma.jsonl), `read_one_a55`). This probe leaves the CPU at the clock
that the runner and the ROM left, so the pass records cycles; the prediction is 4.0 to 5.5 GB/s, a rate
bounded by the single core's outstanding requests and the DRAM latency, not by the clock. A pass over 1 GiB
with the 1 GiB block reads within 5 percent of the control. The view with 2 MiB blocks reads within 8 percent.

TLB prediction. A level-1 block entry covers the whole gigabyte, so the L1D TLB refill count of the 1 GiB pass
is below 100 (the entry, the routine's stack and code windows, and the kernel's entries). The 2 MiB view needs
one entry per block: 512 for the pass, so 512 to 700 refills, which is about 0.5 to 0.7 refills per MiB, and none
of them are 4 KiB-page misses. A DTLB walk count of the same order follows when the core counts that event.

Loader prediction (the file on the card). Streaming `sunstone-region-test.bin` (512 MiB) through two 128 KiB
buffers reads at the measured 21.9 MB/s per request; copy and SHA-256 overlap the next request and the chain
lookups run between requests, so the load takes 24 to 27 seconds (19 to 22 MB/s) and the SHA-256 equals
`a0ab21dca50740b257c495faa888058cb757ec5165d27afb0c13cc5bef48eabd`. Without the file the probe reports it absent,
skips the load and returns normally in under 2 seconds.

## Measured on the board with the file absent

The probe ran on port 3-1 at its boot clocks (the core ran at 494 MHz, from cycles over time), so it reports
functional results and per-cycle figures; performance figures belong to the OS image at the approved top points.
The isolation cases behaved as predicted above, with the level of each fault as corrected there. A 1 GiB pass
read 3.79 bytes per cycle in every mapping (the 40 MiB control, 1 GiB blocks, 2 MiB blocks): the block size did
not change the read rate. The L2 TLB refill count was 3 per pass with 1 GiB blocks and 513 with 2 MiB blocks, and
the DTLB walk count 2 and 512. The L1D TLB refill count was 262,314 per pass (one per 4 KiB page) in every
mapping, so only the L2 refill and walk events show the block size. The card read rate of 21.9 MB/s per 128 KiB
request, reported earlier, is also a boot-clock figure; the OS image re-measures it, where the 0.65 ms of
overhead per request may shrink.

## Predicted FAT32 volume inspection

Condition: the read-only SD probe mounts the card's only partition (FAT32, first sector
32768, 122,105,856 sectors, from the layout read) with the read-only FAT32 reader, counts the
free clusters by reading the whole FAT, and lists the root directory, in profile 2 on port 3-1.

Prediction: the mount succeeds with 512-byte sectors and a power-of-two cluster size (a
partition of 58 GiB formatted by a PC has 32 KiB clusters, so about 1.9 million clusters and
a FAT of about 15,000 sectors); the free-cluster count is within the volume and agrees with
the FSInfo hint when the PC left the hint current; the root directory lists no more than 64
entries, each with a size below 4 GiB; the FAT scan and listing finish within one second of
identification at the measured 21.9 MB/s; and no restoration word changes.

## Host checks

`checks/app-region.py` compiles the ledger and the table builder with the
sanitizers and walks the tables with a separate model of AArch64 stage-1
translation (4 KiB granule, 39-bit input, levels 1 and 2, access flag, AP and
execute-never bits). The model aborts on a descriptor bit it does not model
(contiguous hint, table attributes, non-secure bit). The check covers:

- an application reaches every sampled byte of its region at EL0, including
  the first and last byte, and a second application and the kernel's
  EL0 view fault on the same bytes with permission faults;
- the bytes below and above each region fault in every space, including slots
  that two applications share;
- a read-only region faults on an EL0 write, and the kernel's table gives EL1
  write access to it for the loader;
- block counts: three 1 GiB blocks for 3 GiB, one 1 GiB block plus 256 2 MiB
  blocks for 1.5 GiB, 51 2 MiB blocks for 102 MiB, and page counts of 1, 2 and 2;
- kernel mappings are identical in every space for sampled MMIO, text, data
  and assets addresses at both exception levels;
- a request larger than the free memory, a zero size, a write-only access,
  an overflowing size, a fifth region of one owner and a seventeenth region
  are refused with the ledger unchanged; a request for exactly the free
  memory succeeds and the next one is refused; release restores the memory;
  fragmented usable ranges refuse a request that fits their sum;
- an empty usable list refuses every request;
- a table arena that is too small leaves the space unbuilt and the arena
  unchanged;
- a TLB model with global and ASID-tagged entries shows no conflict and no
  stale permission across kernel, application A and application B with no
  invalidation when every upper entry is non-global, and shows both failures
  with global kernel entries;
- a mutation that gives read-only regions write permission fails the check.

## Prerequisites and changes outside this design's files

1. **Upper memory must be mapped.** The memory qualification image's run on
   port 3-1 passed the window scan and the page signature over
   `0x4400_0000`–`0x3_4000_0000` (decode result above). The address-test
   definition of `a733_dram_decode_check()` is built by the CMake option
   `SUNSTONE_A733_DRAM_DECODE_PROBE`; without it no image maps upper memory,
   and the ledger has no usable ranges.
2. **`nG` on upper-memory entries.** `A733_EL1_DRAM_BLOCK` sets `nG` for the
   entries of slots 2 to 12, with ASID 0 in the kernel's TTBR0, and the golden
   comparison writes the fields out so that its native-publication case fails
   without the bit.
3. **The TTBR0 hook.** The image defines `portTASK_SWITCH_HOOK` and tags the
   tasks of each application with its space.
4. **Second application for the isolation test.** The OS admits one primary
   application and one GPU peer. The test image adds a second EL0 task with its
   own space and no region.
5. **Loader and SDK.** `fatfs_port_read`, the loader task, the section reader
   in `tools/sunstone-manifest.py` and the four `ss_large_*` calls.

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
