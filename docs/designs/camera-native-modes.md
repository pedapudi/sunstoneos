# Native OV5640 image sizes and bounded memory

Status: implemented with software checks. The consumer is the existing native
still and finite-stream camera application. Both camera connectors use the same
image-size selection. No camera is attached; source and model checks cannot
qualify physical images, receiver timing, DRAM ownership, or recovery.

## Select one existing sensor mode per image

The sensor driver already derives ten UYVY modes, from 160 by 120 through 2592 by
1944, in `drivers/camera/ov5640.c`. Native construction currently selects VGA,
and its receiver, allocations, copies, and host interpretation require VGA.

Add a build selection for the existing size enum, defaulting to VGA. Derive the
sensor mode with `ov5640_mode_get(size, OV5640_UYVY, 0)`, retaining the source's
default frame length. Native admission compares its derived dimensions with the
selected allocation geometry before allocating or accessing hardware. Receiver
admission compares the complete selected mode, including link and pixel rates.
No runtime mode switch is admitted after construction.

An immutable, file-backed ELF profile records the selected size, width, height,
UYVY input/NV16 output, still or stream operation, plane and slot lengths, table
capacity, and linked memory reservation. The build identity includes these
values and every defining source. Host preparation reads the profile from a
read-only load segment, validates its arithmetic and the linker symbols, and
binds the existing session to that image. Host observation uses this profile.
The session and USB wire layouts need no additional fields: the existing image
identity authenticates the profile and each USB response already carries the
total payload length. Preserved VGA images remain an explicit legacy profile.

## Reserve complete guarded planes and one owned output copy

For width W and height H, define payload P = W * H, page-rounded payload
Q = ceil(P / 4096) * 4096, and slot S = Q + 4096. Each slot contains P image
bytes, page-rounding padding, and one additional 4096-byte guard. Copy and USB
lengths use P, never Q or S. This also handles sizes whose payload is not a
multiple of a page.

The source allocation contains the IOMMU tables, 8192 parameter bytes, and two
guarded slots for still capture or four for the two-slot stream. The parameter
allocation retains its existing parameter page and 4096-byte guard. A separate
CPU-owned allocation holds 2 * P output bytes, rounded to complete pages by the
allocator. The table arena is 32768 bytes except for the largest mode, whose
native layout reserves 36864 bytes for five second-level tables.

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
compact pool may additionally lose up to 12288 bytes to the source allocation's
16 KiB alignment. The maximum profile starts at an aligned fixed address and
needs no such gap.

Images whose selected operation fits retain the 2 MiB compact pool. Larger
images select a camera-only 32 MiB write-back pool at the half-open physical and
identity CPU range `0x48000000..0x4a000000`. Its allocator keeps page alignment,
with 16 KiB requested for the source allocation. It uses the existing bounded
ledger, without the general extended pool's coarse allocation class.

The largest stream has this exact layout:

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
bytes followed by the 4096-byte guard. Existing admission still requires
explicit guard/prefetch evidence; additional padding does not establish it.

Device addresses start at `0x10000000` for parameters. Y starts at
`0x10002000`; UV starts after both Y slots. The final mapped byte is below
`0x11342000`, so five 4 MiB windows suffice. All camera IOVAs stay below 4 GiB;
all backing remains below 2 GiB and within the existing 34-bit physical bound.
The parameter span is read-only to the device, and the plane spans are
write-only. Tables and the output copy have no camera IOVA. CPU aliases must
resolve to the allocated physical spans with the existing write-back attributes.

## Extend IOMMU capacity without enlarging legacy callers

Add a sized offline builder accepting only 32768 or 36864 bytes. Keep
`a733_iommu_build` as the existing 32768-byte, four-window wrapper. Record the
arena length in the resulting internal table binding. Validation, overlap
checks, publication, and controller admission use that admitted length.

Five fixed scratch slots bound window collection. A fifth window remains a
refusal through the legacy API; a sixth is a refusal through either API.
Validate capacity, page and 16 KiB directory alignment, physical/IOVA overflow,
aliases, table overlap, and output overlap before modifying the arena or binding.
Canonical validation continues to check every directory and leaf word, including
unused entries. No live mapping changes or new master permissions are added.

The camera capture configuration carries the selected table capacity; existing
callers with no explicit capacity retain 32768 bytes. The native publication
callback requires that capacity and the allocated table address. Other IOMMU
clients continue to allocate and publish 32768 bytes.

The reference is the pinned Allwinner IOMMU v2 page-table implementation at
Linux revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. Its header defines
34-bit IOVAs, a 22-bit directory shift, and 12-bit pages. Its source/header
SHA-256 values are `c48d2f4c80f3d15455e811eaddbea0ab485af217654f9823e927fe299f329b76`
and `93ff578b4632306c9403f1edd9c5e1841558d465ecc112caf07346d47373eba3`.
Both cached files match `sources.lock.json`. This extends software table storage;
it does not change the hardware page format or assume larger device pages.

## Bind the pool before constructing CPU mappings

The camera linker patch reserves the external pool as a distinct NOLOAD segment.
Code, mutable application state, the session at `0x41f00000`, and the monitor
and CPU tables at `0x42000000..0x42200000` retain their current reservations.
The external segment must be writable, non-executable, identity-addressed, and
have zero file bytes. The host uploads only initialized image segments and the
existing session; it neither uploads nor clears the 32 MiB reservation.

The camera boot hook already retains and validates the session before CPU table
construction. A camera-only mapping admission hook requires that retained
session, the compiled image/profile identity, the exact linker pool endpoints,
and no non-cacheable pool. It must succeed before adding any external mapping.
The host requires DDR ownership evidence for both `0x41000000..0x42200000` and
`0x48000000..0x4a000000`, bound to the actual image and existing session memory
digest. A compact reservation record cannot admit a large image.

Reuse the cached builder's existing descriptor construction to add sixteen
2 MiB blocks in both EL3 and EL1. Both aliases are inner-shareable write-back
with matching physical addresses. EL3 uses the existing non-secure descriptor
bits; EL1 denies EL0 access, and both mappings deny execution. The camera
owner and USB task already use the privileged task allowlist. The camera application rejects general extended, NPU large-graph, and GPU
read-only-pool build combinations. Its external branch excludes the broad DRAM
decode/mapping pass, including when that provider would return success. The
ordinary compact and general extended branches retain their admission and maps.

The existing pre-scheduler pool preparation checks one write translation per
2 MiB block and publishes the allocator only after every check succeeds.
The camera pool check requires the exact endpoints. There is no fallback to a
smaller reservation or to the general 768 MiB pool. Failed admission publishes
no allocator. The one-second boot deadline and sixty-second supervised session
remain bounds; no physical startup-time claim follows from the memory estimate.

## Preserve bounded delivery and retained stop

Still capture copies each payload into its existing owned output allocation.
Finite streaming keeps one delivered slot HELD while copying at most 4096 bytes
per owner step, with capture polling before each copy. The maximum frame needs
2462 copy steps. DMA may drop frames into its active slot while the held slot
remains protected. Giving a slot back still requires the existing slot lease;
this change creates no post-stop reuse authority.

USB continues to read only the completed immutable output copy. Its maximum
frame needs 2461 requests, each at most 4096 bytes; the last request is short
when necessary. The host bounds pending status requests separately from the
geometry-derived payload request count, under the existing absolute deadline.
It persists every byte and reports the selected dimensions. Response framing
uses the existing version's 72- or 80-byte header, including short transfers.

Successful finite copying requests sensor stop and reports retained status when
packet drain remains unavailable. Pre-admission refusal has no device effect.
Uncertain sensor, DMA, IOMMU, cache, cancellation, or deadline failure retains
the applicable allocations and leases. The existing whole-board power-removal
condition governs recovery; size selection supplies no new drain proof.

Receiver settle and deskew remain explicit per-port admission inputs. A larger
mode has its source-derived lane rate and must have matching physical timing
evidence. Neither the cached reference nor modeled images provides that evidence.

## Costs and selected checks

VGA retains its arena, copies, register sequence, and payload length. The largest
table scan/publication grows from 8192 to 9216 words, a 12.5 percent increase
required by the fifth window. Large-pool preparation requires sixteen address
translations and adds sixteen descriptors per CPU translation regime, using
the existing table pages. There are two allocations per session and none per
frame. Native streaming retains one copy into output and bounded 4096-byte work
per owner step. USB retains the existing response copy and service loop.

Native admission initially fills the guarded source area: at most 20,185,088
bytes. Cache maintenance covers the admitted source spans and the selected
delivered payloads, with exact lengths recorded by the existing fixture.
Physical cache-operation duration and the resulting DMA status gap remain
qualification conditions; an exceeded interval must still fault.

Use the existing checks and evidence snapshot tools:

- Exercise every size in compiled native still and stream paths with the actual
  mode derivation, plane/slot layout, table builder, and provider callbacks.
  Check complete output lengths and first/last payload bytes, including page
  tails, guard exclusion, a held slot, and refusal after an overdue DMA interval.
- Extend the IOMMU fixture for a fifth window, sixth-window refusal, table and
  span aliases, corrupted metadata/leaf entries, exact publication length, and
  no output modification on refusal. Reuse unaffected controller evidence and
  check representative existing camera/codec/display callers retain 32 KiB.
- Extend the CPU memory fixture for the camera profile. Compare every EL1/EL3
  entry, permissions, exact pool preparation, and refusal without session
  admission or with wrong endpoints. Compare the unchanged compact map too.
- Build actual default VGA, maximum still, and maximum stream ELFs. Exercise
  their profile/image/session admission, DDR-range binding, loader segment
  selection, USB output, and full host capture with existing fixtures. Include
  wrong geometry, truncated output, a short final packet, and wrong header
  version. Retain the preserved still-ELF compatibility check.
- Record source closure, linked image/object sizes, register accesses, cache
  lengths, copy counts, allocation placement, and maximum modeled DMA status
  gaps. Compare VGA with accepted CAM2/native-stream evidence. New physical
  throughput or startup timing is not inferred from these counts.

Shared scope agreed with the coordinator: `include/a733_iommu.h`,
`drivers/iommu/a733_iommu.c`, `include/a733_device_memory_layout.h`,
`platform/a733-smp-cached/memory.c`, and
`platform/a733/memory_reservations.c`. Camera profile, native/capture, application,
host, CMake/link patch, component documentation, and their existing checks are
owned by this implementation. No shared scheduler, USB wire, or recovery-policy
change is required.

## Software results and evidence reuse

[Native evidence](../camera-native.md#all-native-sizes-use-bounded-memory-and-complete-output)
records the layout, native execution, mapping checks, and measured costs.
[Application evidence](../camera-application.md#image-bound-sizes-and-full-frame-usb-capture)
records complete still/stream payloads and profile admission. The evidence root
is `~/.cache/sunstoneos/evidence/camera-modes-1026/`.

`prepared-max-stream-result.json` records the actual offline preparation,
launcher build, freeze, and frozen-bundle verification of the final maximum
stream image. The external 32 MiB segment is a reservation and contributes no
upload bytes. The sealed profile and both DDR ranges survive frozen validation.
The fixture evidence explicitly supplies no physical authority.

The earlier maximum-stream application check executed separate compiled owner
and USB fixtures. Its parent image predates the explicit profile-field loads;
its tested capture and USB production units remain unchanged. Final actual-image
profile admission and sealing are covered separately. The evidence closure
records both identities and limits this reuse to those unchanged units.
