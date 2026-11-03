# Native OV5640 image sizes and bounded memory

Status: implemented and host-tested. The consumer is the native still and
finite-stream camera application, on either camera connector. No camera is
attached; source and model checks cannot qualify physical images, receiver
timing, DRAM ownership, or recovery.

## One sensor mode per image

`drivers/camera/ov5640.c` derives ten UYVY modes, from 160 by 120 through 2592
by 1944. A build selection (`SUNSTONE_CAMERA_SIZE`, default VGA) picks one size.
The sensor mode is `ov5640_mode_get(size, OV5640_UYVY, 0)` with the source's
default frame length. Native admission compares its derived dimensions with the
selected allocation geometry before allocating or accessing hardware, and
receiver admission compares the complete selected mode, including link and
pixel rates. No runtime mode switch is admitted after construction.

An immutable, file-backed ELF profile records the selected size, width, height,
UYVY input and NV16 output, still or stream operation, plane and slot lengths,
table capacity, and linked memory reservation. The build identity includes these
values and every defining source. Host preparation reads the profile from a
read-only load segment, validates its arithmetic and the linker symbols, and
binds the session to that image; host observation uses the profile. The session
and USB wire layouts carry no additional fields: the image identity
authenticates the profile, and each USB response carries the total payload
length. Preserved VGA images form an explicit legacy profile.

## Guarded planes and one owned output copy

For width W and height H, payload P = W * H, page-rounded payload
Q = ceil(P / 4096) * 4096, and slot S = Q + 4096. Each slot holds P image bytes,
page-rounding padding, and one 4096-byte guard. Copy and USB lengths use P,
never Q or S, which handles sizes whose payload is not a multiple of a page.

The source allocation contains the IOMMU tables, 8192 parameter bytes, and two
guarded slots for still capture or four for the two-slot stream. The parameter
allocation keeps its parameter page and 4096-byte guard. A separate CPU-owned
allocation holds 2 * P output bytes, rounded to complete pages by the
allocator. The table arena is 32768 bytes except for the largest mode, whose
layout reserves 36864 bytes for five second-level tables.

| Size | Plane payload | Slot including padding and guard | Still total | Stream total |
| --- | ---: | ---: | ---: | ---: |
| 160 by 120 | 19,200 | 24,576 | 131,072 | 180,224 |
| 176 by 144 | 25,344 | 32,768 | 159,744 | 225,280 |
| 320 by 240 | 76,800 | 81,920 | 360,448 | 524,288 |
| 640 by 480 | 307,200 | 311,296 | 1,277,952 | 1,900,544 |
| 720 by 480 | 345,600 | 352,256 | 1,437,696 | 2,142,208 |
| 720 by 576 | 414,720 | 421,888 | 1,716,224 | 2,560,000 |
| 1024 by 768 | 786,432 | 790,528 | 3,194,880 | 4,775,936 |
| 1280 by 720 | 921,600 | 925,696 | 3,735,552 | 5,586,944 |
| 1920 by 1080 | 2,073,600 | 2,080,768 | 8,351,744 | 12,513,280 |
| 2592 by 1944 | 5,038,848 | 5,046,272 | 20,217,856 | 30,310,400 |

Totals include tables, parameters, source slots, and page-rounded output. A
compact pool may lose up to 12288 bytes more to the source allocation's 16 KiB
alignment; the maximum profile starts at an aligned fixed address and loses
none.

Images whose operation fits use the 2 MiB compact pool. Larger images use a
camera-only 32 MiB write-back pool at the half-open physical and identity CPU
range `0x48000000..0x4a000000` (`A733_CAMERA_POOL_START` and `_END` in
`include/a733_camera_profile.h`). Its allocator keeps page alignment, with
16 KiB requested for the source allocation, and uses the bounded ledger without
the general extended pool's coarse allocation class.

The largest stream has this layout:

| Object | Physical and CPU range | Bytes |
| --- | --- | ---: |
| Directory and five leaf tables | `0x48000000..0x48009000` | 36,864 |
| Parameters and guard | `0x48009000..0x4800b000` | 8,192 |
| Y slot 0 | `0x4800b000..0x484db000` | 5,046,272 |
| Y slot 1 | `0x484db000..0x489ab000` | 5,046,272 |
| UV slot 0 | `0x489ab000..0x48e7b000` | 5,046,272 |
| UV slot 1 | `0x48e7b000..0x4934b000` | 5,046,272 |
| Owned output copy | `0x4934b000..0x49ce8000` | 10,080,256 |
| Unallocated remainder | `0x49ce8000..0x4a000000` | 3,244,032 |

The source allocation is 20,230,144 bytes. The output contains 10,077,696 image
bytes and 2560 bytes of allocator padding. Each source slot has 3328 padding
bytes followed by the 4096-byte guard. Guard and prefetch evidence is a
separate admission requirement; padding does not establish it.

Device addresses start at `0x10000000` for parameters. Y starts at
`0x10002000`; UV starts after both Y slots. The final mapped byte is below
`0x11342000`, so five 4 MiB windows suffice. All camera IOVAs stay below 4 GiB;
all backing stays below 2 GiB and within the 34-bit physical bound. The
parameter span is read-only to the device, and the plane spans are write-only.
Tables and the output copy have no camera IOVA. CPU aliases resolve to the
allocated physical spans with write-back attributes.

## IOMMU capacity

`a733_iommu_build_sized` accepts only 32768 or 36864 bytes
(`A733_IOMMU_LARGE_TABLE_BYTES`); `a733_iommu_build` is the 32768-byte,
four-window wrapper. The arena length is recorded in the internal table
binding, and validation, overlap checks, publication, and controller admission
use that admitted length.

Five fixed scratch slots bound window collection. A fifth window is a refusal
through the wrapper; a sixth is a refusal through either API. Capacity, page
and 16 KiB directory alignment, physical and IOVA overflow, aliases, table
overlap, and output overlap are validated before the arena or binding changes.
Canonical validation checks every directory and leaf word, including unused
entries. The camera capture configuration carries the selected table capacity
(32768 bytes when none is given), and the native publication callback requires
that capacity and the allocated table address. Other IOMMU clients allocate and
publish 32768 bytes.

The reference is the Allwinner IOMMU v2 page-table implementation at Linux
revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`: 34-bit IOVAs, a 22-bit
directory shift, and 12-bit pages. The source and header SHA-256 values are
`c48d2f4c80f3d15455e811eaddbea0ab485af217654f9823e927fe299f329b76` and
`93ff578b4632306c9403f1edd9c5e1841558d465ecc112caf07346d47373eba3`; both cached
files match `sources.lock.json`. Larger tables extend software storage only;
the hardware page format and device page size are unchanged.

## Pool binding before CPU mappings

The camera linker patch reserves the external pool as a distinct NOLOAD
segment. Code, mutable application state, the session at `0x41f00000`, and the
monitor and CPU tables at `0x42000000..0x42200000` keep separate reservations.
The external segment is writable, non-executable, identity-addressed, and has
zero file bytes. The host uploads only initialized image segments and the
session; it neither uploads nor clears the 32 MiB reservation.

The camera boot hook retains and validates the session before CPU table
construction. A camera-only mapping admission hook requires that retained
session, the compiled image and profile identity, the exact linker pool
endpoints, and no non-cacheable pool, and it succeeds before any external
mapping is added. The host requires DDR ownership evidence for both
`0x41000000..0x42200000` and `0x48000000..0x4a000000`, bound to the image and
session memory digest. A compact reservation record cannot admit a large image.

Sixteen 2 MiB blocks are added in both EL3 and EL1, inner-shareable write-back
with matching physical addresses. EL3 uses the non-secure descriptor bits; EL1
denies EL0 access; both deny execution. The camera application rejects general
extended, NPU large-graph, and GPU read-only-pool build combinations, and its
external branch excludes the broad DRAM decode and mapping pass even when that
provider would return success. The compact and general extended branches keep
their own admission and maps.

Pre-scheduler pool preparation checks one write translation per 2 MiB block
and publishes the allocator only after every check succeeds; the camera check
requires the exact endpoints. There is no fallback to a smaller reservation or
to the general 768 MiB pool, and failed admission publishes no allocator. The
one-second boot deadline and sixty-second supervised session bound the run; no
physical startup time follows from the memory estimate.

## Bounded delivery and retained stop

Still capture copies each payload into its owned output allocation. Finite
streaming keeps one delivered slot HELD while copying at most 4096 bytes per
owner step, with capture polling before each copy. The maximum frame needs 2462
copy steps. DMA may drop frames into its active slot while the held slot stays
protected. Giving a slot back requires the slot lease; size selection creates
no post-stop reuse authority.

USB reads only the completed immutable output copy. The maximum frame needs
2461 requests of at most 4096 bytes, the last short when necessary. The host
bounds pending status requests separately from the geometry-derived payload
request count, under the absolute deadline, persists every byte and reports the
selected dimensions. Response framing uses the version's 72- or 80-byte header,
including short transfers.

Successful finite copying requests sensor stop and reports retained status
because packet drain is unavailable. Pre-admission refusal has no device effect.
Uncertain sensor, DMA, IOMMU, cache, cancellation, or deadline failure retains
the applicable allocations and leases, and the whole-board power-removal
condition governs recovery.

Receiver settle and deskew are explicit per-port admission inputs. A larger mode
has its source-derived lane rate and needs matching physical timing evidence,
which neither the cached reference nor modeled images provide.

## Cost

VGA keeps its arena, copies, register sequence, and payload length. The largest
table scan and publication grows from 8192 to 9216 words (12.5 percent) for the
fifth window. Large-pool preparation needs sixteen address translations and adds
sixteen descriptors per CPU translation regime, in existing table pages. A
session makes two allocations and none per frame. Native streaming makes one
copy into output with bounded 4096-byte work per owner step; USB keeps its
response copy and service loop. Admission fills the guarded source area, at
most 20,185,088 bytes. Cache maintenance covers the admitted source spans and
the delivered payloads. Physical cache-operation duration and the resulting DMA
status gap are qualification conditions; an exceeded interval faults.

## Checks and qualified figures

Compiled native still and stream paths run every size with the actual mode
derivation, plane and slot layout, table builder and provider callbacks. They
check complete output lengths and first and last payload bytes, including page
tails, guard exclusion, a held slot, and refusal after an overdue DMA interval.
The IOMMU fixture covers a fifth window, sixth-window refusal, table and span
aliases, corrupted metadata and leaf entries, exact publication length and no
output modification on refusal, and checks that representative camera, codec and
display callers keep 32 KiB. The CPU memory fixture compares every EL1 and EL3
entry, permissions and exact pool preparation, and refuses a missing session
admission or wrong endpoints. The default VGA, maximum still and maximum stream
ELFs run profile, image and session admission, DDR-range binding, loader
segment selection, USB output and full host capture, including wrong geometry,
truncated output, a short final packet, and a wrong header version.

| Figure | Value | Record |
| --- | --- | --- |
| Largest stream source allocation; output allocation | 20,230,144 B; 10,080,256 B | [camera-native-modes.jsonl](../evidence/camera-native-modes.jsonl) `camera-native-modes-software` |
| Largest table scan | 9,216 words | same |
| Maximum-frame copy steps; USB requests | 2,462; 2,461 | same |

[Native evidence](../camera-native.md#all-native-sizes-use-bounded-memory-and-complete-output)
and [application evidence](../camera-application.md#image-bound-sizes-and-full-frame-usb-capture)
describe layout, execution and complete still and stream payloads.

## Shared scope and limitations

Shared files: `include/a733_iommu.h`, `drivers/iommu/a733_iommu.c`,
`include/a733_device_memory_layout.h`, `platform/a733-smp-cached/memory.c` and
`platform/a733/memory_reservations.c`. No shared scheduler, USB wire, or
recovery-policy change is needed. The camera samples its three configured rails
through the PMIC provider and borrows their shared parent lease; it does not
initialize the GPU consumer or request exclusive DCDC4 control.

- No board run exercises any size; physical images, receiver timing, DMA
  status gap, drain and startup time are unqualified.
