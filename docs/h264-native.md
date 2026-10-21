# Native H.264 bounded decoding profiles

The native application programs the video engine (VE) and exposes copied planar
YU12 pictures. Baseline builds retain the 48-by-32 single/two-picture,
640-by-480 streaming and 1920-by-1080 streaming modes. A separate High-profile
build admits runtime geometry, CABAC, I/P/B pictures and multiple short-term
references through the same controller and application. Physical decoding remains
unqualified. Long-term references, unrestricted streaming, broader profiles and
formats, other codecs and encoding remain required work.

## High-profile runtime clips and presentation ordering

`SUNSTONE_H264_HIGH=ON` selects session version 4, the existing extended memory
providers and the same private 288-byte boot contract as the full-HD build.
`SUNSTONE_H264_GSTREAMER_SOURCE` names the pinned source cache recorded in
`apps/h264/sources.json`. The build verifies those source hashes and selects
standalone SPS/PPS/slice-header, bit-reader and scaling-list functions. The
FreeRTOS CPU runs these functions and adapted GStreamer picture-order-count
(POC), reference-list and decoded-picture-buffer (DPB) algorithms. No GStreamer,
GLib, GObject, V4L2 or Linux runtime executes on the board. Generated files retain
upstream LGPL notices. `apps/h264/LICENSE.gstreamer` retains those notices, the
Chromium BSD notice from the adapted decoder and complete upstream COPYING.

The admitted stream is High profile 100, progressive 8-bit 4:2:0, level at most
4.0, with one complete slice per picture and fixed SPS/PPS bytes throughout a
session. CABAC, 8-by-8 transforms, custom scaling matrices, explicit weighted P/B
prediction and implicit weighted B prediction are programmed from parsed syntax.
The initial picture must be IDR. Subsequent I/P/B pictures may use up to four
short-term references, including reference B pictures. Each reference list may
contain at most four active entries, including repeated weighted entries. A
four-reference stream that signals five active weighted entries is refused.

POC types 0 and 2, short-term list modifications 0/1, sliding-window marking and
memory-management operations 1/5 are implemented. Frame-number gaps, long-term
marking/references, fields/MBAFF, POC type 1, FMO, redundant slices, transform
bypass and unsupported marking operations are refused. The SPS must signal VUI
bitstream restrictions, at most two reordered pictures and a DPB limit of at
most four. Missing references fail rather than conceal damage. The completed
fixtures exercise POC type 0 and operation 1; wraparound and operation 5 remain
source-reviewed paths without execution coverage.

Coded dimensions are multiples of 16, from 16-by-16 through 1920-by-1088.
Visible dimensions must be positive and even, at most 1920-by-1080. Even crop
amounts on all four edges are accepted. The full coded picture remains available
for prediction; copied YU12 output contains only visible samples. Geometry,
level limits and reference count are validated together against the fixed pool.
For example, 1920-by-1088 permits two references plus the destination;
1280-by-720 permits four references plus the destination. Runtime dimensions do
not increase the existing extended-memory reservation.

### Fixed device and CPU storage budgets

The High build retains six guarded device spans and the existing four IOMMU
windows. No generic IOMMU policy changes are required.

| IOVA | Capacity in bytes | Device access |
| --- | --- | --- |
| `0x10000000` | 4,194,304 input | Read |
| `0x10401000` | 6,266,880 Y pictures | Read/write |
| `0x109fc000` | 3,133,440 chroma pictures | Read/write |
| `0x10cfa000` | 217,088 picture scratch | Read/write |
| `0x10d30000` | 786,432 motion vectors | Read/write |
| `0x10df4000` | 49,152 neighbor scratch | Read/write |

For coded width W and height H, each picture uses page-rounded W*H Y bytes and
W*H/2 chroma bytes. Each motion-vector field requires
`ALIGN((W/16)*ceil((H/16)/2)*32,1024)` bytes; a page-rounded picture slot holds two
fields. The destination needs a slot separate from every current reference.
The parser checks all slot products against the aggregate capacities before
publishing a plan. Picture scratch uses the greater of 130 KiB and
`18*4096 + H*128` bytes. Source-defined SRAM selection for widths through 1920
and the effective A733 allocation anchors are the same as the full-HD branch
below. These formulas include coded padding and are independent of visible crop.

Actual allocator execution requires 14,749,696 uncached bytes, including tables,
alignment and guards. CPU write-back allocations require 23,920,640 bytes: a
4 MiB upload, one optional oracle, five copied outputs and 1 MiB of parser/DPB
metadata. Each copied-output capacity is 3,110,400 bytes. The actual metadata
structure uses 243,064 bytes. Observed native parser stack use in the allocation
fixture is 2,616 bytes; the existing USB task has a 16 KiB stack. These are
modeled execution measurements, not worst-case stack or physical timing proofs.
All eight CPU leases are retained before publication. Device spans, CPU leases,
alignment, guard capacity and table construction are validated before device use.

### Authenticated input and independent ownership transitions

The sealed session authenticates a fixed manifest of at most 256 pictures. Its
128-byte prefix binds geometry, crop, reference/reorder limits, optional-oracle
mode and the SPS/PPS digest. Each 128-byte descriptor binds one access unit's
length/digest, optional pixel-oracle length/digest, decode ordinal, presentation
ordinal, GOP, frame number, POC, slice kind, reference flag and IDR flag. Every
picture's complete upload is copied, hashed and parsed before its first device
write. Parsed values must match authenticated claims. The manifest does not
supply device drain authority. Partial uploads remain bounded and unpublished.

A prepared DPB transition is speculative. It commits only after the engine's
source-defined decoded/macroblock completion, guarded visible copying and the
executor's final status, fault, liveness and deadline checks. References keep
their coded Y/chroma and motion-vector extents until their last completed
consumer and subsequent marking retire them. Input refill follows healthy
completion checks. Copied-output acknowledgment controls only the CPU copy;
it cannot retire a device reference or authorize input refill. Uncertain
completion prevents subsequent decoding and retains every allocation.

Five private CPU output slots support up to two published, unacknowledged
pictures plus the admitted reorder interval. Output publication follows
presentation order; decode order may differ. The application applies
backpressure when copies or caller ACKs prevent progress. Acknowledging a copy
cannot resurrect a failed session. Completed copies remain queryable after
failure, including completed future anchors that had not reached normal
presentation order. Such responses carry an explicit reorder-pending flag.
A partially copied failing picture remains private.

High commands use wire version 2. The 256-byte response has version 5 and binds
the session, manifest and input digests. It reports presentation/decode ordinals,
GOP, signed POC, visible/coded dimensions, strides, all crop edges, reference/IDR
flags, completed decode count and reorder-pending state. The host saves actual
copied bytes before ACK. An optional client-supplied independent oracle enables
numerical comparison; normal decoding returns copied bytes and metadata with
`oracle_verified` false. Host capture stops on failure; manual indexed queries
can retrieve additional retained reorder-private anchors.

The controller programs frame-list POCs, both motion-vector field pointers,
both reference lists, prediction weights and raster scaling matrices from the
immutable plan. Cedrus places reference-list/weight programming after decoder
INIT/FLUSH and scaling programming before INIT. The lower native VE policy now
admits the weight-denominator register at `0x01c0e210`; the focused access check
executes that actual policy, including width, alignment and peripheral-base
refusals. Existing VLD offset-zero copying, busy guards and decoded-macroblock
completion checks remain in force.

### Compiled evidence and physical limits

Evidence is retained under `~/.cache/sunstoneos/evidence/h264-high-20261019`.
The native candidate SHA-256 is
`ecd05003b5430d12940e77fce7f1a6e2634c88afe5779181da07574ba759dc66`.
`compiled/dependencies.json`, `compiled/commands.json` and
`compiled/objects.json` identify 462 compiler inputs and 175 linked objects.
The source/linked-input freeze records the final closure. Provenance-only
updates to `apps/h264/sources.json` after compilation do not change this accepted
executable; its original build-identity input is preserved separately.

`allocation-budget` and `allocation-four-ready` execute actual native parser,
CPU storage, allocator and IOMMU setup before composed device cases. The entry
check executes the generated AArch32 launcher for accepted High, rejected
full-HD and invalid session versions. Root's `root/layout-review.json` and
`root/frozen/manifest.json` preserve the linked startup/retained-contract review.
Generated boot and linker bytes match the accepted full-HD startup. No shared
CPU startup, GIC or secondary execution is repeated.

Producer fixtures use actual host libx264 High/CABAC clips. Independent FFmpeg
supplies visible pixel references and syntax traces. The native composed checks
execute the linked parser, DPB, allocation/table builder, controller, visible
copy, digest, presentation order and serializer. Power/admission, MMIO, pixel
writes, device translation, fault scans and USB are explicit models. Injected
pixels establish copy/oracle behavior but do not qualify hardware arithmetic,
reference visibility, DMA confinement or timing. The native results are in `native/result.json` and
`native-four/result.json`: twelve full-HD pictures across an IDR boundary,
twelve HD pictures including reference B pictures, twelve four-reference HD
pictures and three normal-mode decoded pictures. The independent register
oracles are `register-oracle.json` and `four-register-oracle.json`. They derive
DPB transitions, lists, POCs, motion-vector pointers, strides and prediction
weights from FFmpeg syntax rather than native parser plans. `host/result.json`
checks actual bundles and presentation capture with native-produced frames,
modeled transport/progress and the actual fsync-backed output writer. It
checks output persistence before ACK and preserves the earlier output on B
failure. `bounds/result.json` records focused sanitized syntax refusals.
`preparation-coverage.json` records setup failures and their limited repairs.
No completed native case is replayed for those repairs.

The focused Astra source review covers POC/lists/marking, speculative commit,
reference/MV lifetime and output ordering. Existing Baseline parser and CPU startup evidence remains scoped to
its unchanged paths.

The inherited multicore startup, extended-memory mapping, electrical state,
initial device exclusion, IOMMU authority and whole-board recovery remain
physical prerequisites. Source-backed healthy next-picture programming does
not prove physical transaction drain or permit reclamation. All resources stay
retained until whole-board power removal after success or failure. No physical
attempt is authorized by these host-only results.

## Full-HD streaming and extended memory

`SUNSTONE_H264_FULLHD=ON` selects session version 3 and a private 288-byte boot
contract. It accepts progressive 8-bit 4:2:0 Baseline level 4.0. The coded picture
is 1920 by 1088: 120 by 68 macroblocks. The SPS must specify cropping with zero
left, right and top offsets and bottom offset 4. Each vertical crop unit covers
two luma rows, so the visible picture has 1080 rows. Other crop configurations are
rejected. The manifest authenticates at most 256 complete Annex B access units,
each at most 4 MiB, before decoder access. SPS/PPS identity, one reference,
default list 0, POC type 2, four frame-number bits, IDR resets and unsupported
syntax restrictions match the VGA streaming profile below.

The engine keeps all 1088 coded rows in each reference. Y and chroma strides are
1920 and 960 bytes. Each reference holds 2,088,960 Y bytes and 1,044,480 chroma
bytes; Cr starts after 522,240 coded Cb bytes. The primary stride register is
`0x03c00780`, and completion requires exactly 8,160 decoded macroblocks. Copied
output contains only the visible rows: 2,073,600 Y bytes and 518,400 bytes for each
chroma plane, totaling 3,110,400 bytes. Its 192-byte version-4 response header
reports both visible and coded geometry, strides and crop rows. The copy callback
handles at most 32 rows per invocation. Each invocation checks the guard pages,
per-slot slack, status, faults and liveness around its rows. The High build
also checks the unused capacity beyond its admitted reference slots, which can
reach megabytes, before the first rows and after the last. The callback reads
device memory as aligned 64-bit words and publishes only after the final hash
and fault scan. A partial
copy remains private after failure.

### Source-defined storage and hardware format

Pinned Cedrus `cedrus_h264.c:369–382,593–616` uses internal deblocking and
intra-prediction SRAM for widths up to 2048. The effective A733
`H264MallocFrmBuffer` instructions at `0x13a5c–0x13a84` require external buffers
for older engine IDs or widths above 2048. The admitted A733 IDs and width 1920
therefore do not require additional external deblocking/intra-prediction spans.
This decision uses both source and the pinned effective A733 implementation.

The CedarC motion-vector formula and effective A733 instructions at
`0x134a0–0x134fc` require `ALIGN(120 * ceil(68/2) * 32, 1024)` bytes per field:
131,072 bytes. Each picture reserves 262,144 bytes for its two field pointers.
Cedrus picture scratch uses `18*4096 + 1088*2*64 = 212992` bytes inside a
217,088-byte guarded capacity. Neighbor scratch uses 49,152 bytes with 16 KiB
alignment. The full coded picture, including cropped rows, remains the reference;
visible copying does not shorten any device extent. Source pins and notices are
recorded in `apps/h264/sources.json`.

The existing 16 MiB uncached pool supplies the following fixed IOVA layout.
The generic IOMMU policy is unchanged; six spans fit three existing 4 MiB windows.

| IOVA | Capacity | Device access |
| --- | --- | --- |
| `0x10000000` | One 4,194,304-byte input | Read |
| `0x10401000` | Two 2,088,960-byte Y slots | Read/write |
| `0x10801000` | Two 1,044,480-byte chroma slots | Read/write |
| `0x10a00000` | 217,088-byte picture scratch | Read/write |
| `0x10a36000` | Two 262,144-byte MV slots | Read/write |
| `0x10abc000` | 49,152-byte neighbor scratch | Read/write |

The actual allocator needs 11,358,208 uncached bytes, including the 32 KiB table,
alignment, prefixes and guards. It validates each capacity and alignment before
publication. Four private write-back allocations need 13,533,184 page-rounded
bytes: one 4 MiB upload, one optional-oracle buffer and two copied outputs. Their
ledger leases enter RETAINED state before scheduler release and pointer
publication; that ledger never changes during the session. AU/output atomics
control CPU payload access separately. This avoids concurrent allocator-ledger
updates during USB validation. No private write-back buffer receives a device
mapping. Partial allocation failure retains acquired extents and publishes no
provider access.

Reference retirement, healthy input refill and copied-output ACK remain separate
transitions, as described below. The full-HD build preserves the same failure
retention and two-output backpressure rules. Optional reference pixels are a
qualification oracle supplied by the validation client. Normal decoding returns
actual copied bytes and metadata without asserting numerical qualification.

### Startup contract and qualification boundary

The build uses existing `link-devices.ld` memory providers: write-back
`0x48000000..0x78000000` and uncached `0x78000000..0x79000000`. The complete
288-byte live boot record is compared before application effects. Its protected
copy survives BSS clearing. A synchronous local 224-byte view serves only the
existing board session validator; the original protected 288-byte record reaches
extended-memory admission. Both sealed 856-byte session copies survive physical
session-page changes. The session page at `0x41f00000` is outside BSS and retained
copies. The full-HD launcher binds the session, entry evidence and pool ranges.

The inherited board-derived multicore startup and extended-memory mapping scans
remain physical prerequisites. GPU single-core evidence does not qualify this
startup. Host checks stop before shared cached preparation, GIC programming or
secondary execution. Modeled admission of 200,704 pages establishes no physical
startup duration. Initial isolation, recovery and full power removal requirements
remain those of the existing device admission; no caller supplies drain authority.
No image may reclaim mappings or storage after uncertain completion.

### Full-HD evidence scope

The full-HD evidence directory is
`~/.cache/sunstoneos/evidence/h264-fullhd-20261019`. Its independent libx264
six-picture clip crosses an IDR boundary; FFmpeg supplies visible pixels and
syntax traces. Parser checks validate the actual SPS crop and coded geometry,
including malformed crop and oversize refusals. The actual generated AArch32
launcher and private EL3 sealing checks validate the 288-byte contract and both
session copies. The sealing check stops at shared cached preparation. Root's
`root-startup-layout-review.json` preserves the source and linked placement
review for image `b48123e301ec40ae4ec104e58d4c57b347e619b6a0a62a82263e0d0fe491a643`.
No shared startup or physical memory scan executes in these checks.

The composed native check executes the linked allocator, page tables, parser,
controller, guarded crop/copy/hash loop and serializer. Its modeled boundaries
include power, admission readiness, device translation, IOMMU fault responses,
USB and device pixel writes. The model supplies coded padding separately from
the independent visible reference. Therefore copied output verifies crop offsets,
Cr addressing and byte integrity without qualifying physical device arithmetic,
DMA confinement, reference visibility or timing. The accepted VGA image and all
unaffected VGA evidence remain unchanged.

`native-inline-read-boundary/result.json` records six completed pictures, two
copied-output slots, ACK backpressure, wrong ordinal/digest refusal, native
provider length/ordinal refusal, one-page-short uncached allocation refusal and
a retained later failure after 64 private Y rows. Normal mode checks one copied
picture with oracle qualification false. The model explicitly supplies device
translation and physical completion reads because the compiled full-HD copy
inlines its load helper. Harness stack/scratch disjointness is asserted against
the linked pool ranges before allocation. Earlier setup/model-only failures are
retained separately; no native image change resulted.

`a733-fullhd-reference.json` compares primary format/stride and repeated-IDR POCs
against the pinned A733 instructions. Native MV spacing is checked against the
source-backed formula separately; retained frame-helper traces are not an
independent comparison of every SRAM field. `host-capture/result.json` covers
normal/qualification producer bundles and exact 192-byte-header/payload/ZLP
capture. Its ACK handler checks that copied bytes and the saved output record
already exist before acknowledgment. A later failure preserves earlier outputs.
Physical admission and transport are modeled; the actual separately checked
launcher bytes are reused. `closure-comparison.json` binds all 434 unchanged
compiled inputs. The final freeze preserves those inputs, 171 linked objects,
configuration, tools, source reviews and results; later shared-tree changes do
not silently replace the accepted image's inputs.

The original host capture checks bypassed two host entry guards. The actual VGA
launcher fix has separate evidence below. The shared supervisor also omitted
`h264_result` from its result allowlist, so public H.264 invocation refused before
transport. Commit `70650ed` corrects that allowlist. Maintained
`checks/h264-supervisor-entry.py` reaches a readiness sentinel through actual
`run_session` with the caller policy and reviewed FEL digest: a 59-second H.264
result is admitted; 60-second and unknown-result requests refuse. Evidence is
`h264-supervisor-entry-20261019/result.json` and `frozen/manifest.json`. This check
performs no queue, USB or FEL execution. Decoder and launcher evidence is
unaffected by this host-only correction.

## VGA streaming with a fixed retained pool

Session version 2 selects 640-by-480 progressive Baseline 8-bit 4:2:0 pictures.
Each access unit (AU) contains one sequence parameter set (SPS), one picture
parameter set (PPS) and one IDR or P slice beginning at
macroblock zero. SPS/PPS bytes must remain identical throughout the session.
The SPS requires picture-order count (POC) type 2, four frame-number bits, one
reference and
`direct_8x8_inference_flag=1`. P pictures use default list 0 and reference the
preceding completed picture. Frame numbers advance from 0 through at most 15;
an IDR restarts them at 0. POCs are twice the frame number. This supports
repeated groups of pictures (GOPs) without implementing frame-number wrap or
display reordering.

A session admits at most 256 pictures. Each complete Annex B access unit is
bounded to 524,288 bytes. Every picture has 1,200 macroblocks and produces
460,800 copied bytes: 307,200 Y bytes, 76,800 Cb bytes and 76,800 Cr bytes.
Y and chroma strides are 640 and 320 bytes. The primary stride register is
`0x01400280`; the primary chroma-plane length is 76,800 bytes.

Weighted prediction, reference-list changes, adaptive marking, long-term
references, nonreference pictures, B pictures, fields, multiple slices,
cropping, CABAC and SPS/PPS changes are rejected. HRD and the other unsupported
syntax described for the retained profile below remain unsupported. This is a
finite prepared clip consumed incrementally; future unmanifested camera/network
frames are not admitted. No playback rate or real-time performance is claimed.

### Source-backed geometry and healthy-frame reuse

Pinned Cedrus `cedrus_dst_format_set` programs planar YU12 geometry and stride.
The original A733 `H264ConfigureFrameInfoRegister` instruction helper agrees
with the VGA primary-format fields. Width remains below the 2,048-pixel branch
that needs additional external deblock/intra-prediction buffers.
The 16-bit decoded-macroblock counter can represent the required count of 1,200.

Motion-vector storage follows the conservative pinned CedarC formula in
`h264_dec.c:1207–1216`: align `40 * ceil(30/2) * 32` to 1,024 bytes per field.
Each field occupies 19,456 bytes; the bottom-field pointer adds 19,456 bytes.
Each picture reserves a page-rounded 40,960-byte slot. The direct-inference
requirement prevents CedarC's doubled-storage case. Cedrus's picture-info
formula gives `18*4096 + 480*2*64 = 135168` bytes; VGA canaries begin after
that used extent, inside a 139,264-byte allocation. Neighbor storage remains
49,152 bytes. Context scratch is initialized once and retained across GOPs.

[Cedrus normal completion](https://github.com/torvalds/linux/blob/70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56/drivers/staging/media/sunxi/cedrus/cedrus_hw.c#L152)
returns successfully completed input/output jobs without resetting the engine.
CedarC’s outer UPDATE_DATA path includes resets; Cedrus supplies the no-reset
completion model used here.
The pinned [CedarC reference-marking code](https://github.com/Allwinner-Homlet/H6-CedarC/blob/d92ffcb082595f0fffda45ea481cec40fe5a3a8d/vdecoder/videoengine/h264/h264_mmco.c#L419)
derives sliding-window removal when the reference count is full. Its
[decoder completion path](https://github.com/Allwinner-Homlet/H6-CedarC/blob/d92ffcb082595f0fffda45ea481cec40fe5a3a8d/vdecoder/videoengine/h264/h264.c#L1432)
applies marking after completed-frame processing, then installs the current
short-term reference. These sources
support healthy progression through a retained pool. They do not establish
fabric drain after a fault or authorize allocation release.

The controller alternates destination slots A and B. Decoding P1 into B keeps
A referenced; only successful completion of P1 retires A as a reference.
P2 can then write A while referencing B. A later IDR removes the prior reference
from the programmed frame list. Frame SRAM contains actual slot addresses,
POCs and MV pointers; reference-list entries select the current preceding slot.
All unused entries are cleared. No IOMMU mapping is changed between pictures.

Completion still requires an exact macroblock count, no engine error or busy
condition, both decoder-master fault scans, intact guards, copied-output checks
and post-callback deadline/liveness checks. Only then does the controller return
the input lease from DEVICE to CPU ownership. Before the next refill, a fresh
non-destructive IOMMU scan must complete for masters 6 and 7. The application
rechecks guards/liveness, refills the owned input, publishes it with the existing
barrier and returns it to DEVICE ownership. Input bytes beyond the current access
unit remain zero: a refill clears only the bytes that the previous, longer
access unit occupied. Engine checks precede the next
trigger. This scan observes faults; it supplies no drain authority.

Reference-slot retirement, input refill and copied-output acknowledgment are
separate transitions. Y/C/MV allocations remain device-owned. A private copied
output remains immutable until USB transmission finishes and the client
acknowledges its ordinal and digest. Two occupied private outputs stop the next
decode without overwriting either result. The input, AU, tables, references,
context scratch and unacknowledged outputs remain retained after uncertain
completion, faults or protocol failure. No failed session accepts further frames.
All allocations and mappings require whole-board power removal before release.

### Fixed storage and runtime admission

The existing 2 MiB uncached arena supplies these static I/O virtual address
(IOVA) spans:

| IOVA | Capacity | Device access |
| --- | --- | --- |
| `0x10000000` | One 524,288-byte padded input | Read |
| `0x10100000` | Two 307,200-byte Y slots | Read/write |
| `0x10200000` | Two 155,648-byte C slots | Read/write |
| `0x10300000` | 139,264-byte picture-info scratch | Read/write |
| `0x10400000` | Two 40,960-byte MV slots | Read/write |
| `0x10500000` | 49,152-byte neighbor scratch | Read/write |

The 32 KiB table and guarded allocations total 1,814,528 bytes before alignment
gaps. Table and neighbor allocations require 16 KiB alignment; other allocations
use 4 KiB alignment. The actual pool allocator must satisfy all capacities and
alignments before publication. Exhaustion retains the session and refuses decoder
programming. Guard pages remain unmapped; context and slot tails retain canaries.
The CPU also owns fixed staging/oracle arrays, two copied outputs and a
24,576-byte manifest. Their payload storage totals 1,931,264 bytes, plus bounded
control metadata. These arrays are outside decoder mappings. No startup or
memory-map expansion is used. Source-derived extents do not qualify physical
burst bounds or DMA confinement.

The 856-byte sealed session layout is unchanged. Version 2 binds the manifest
SHA256 in `fixture`; `reference` must be zero. Oracle mode is explicit in each
authenticated manifest descriptor. Each descriptor is 96 little-endian bytes:

| Offset | Field |
| --- | --- |
| 0 | Access-unit length, 32 bits |
| 4 | Oracle length: 0 or 460,800, 32 bits |
| 8 | Frame number, 32 bits |
| 12 | Zero, 32 bits |
| 16 | Access-unit SHA256, 32 bytes |
| 48 | Oracle SHA256, or zero when absent, 32 bytes |
| 80 | Sixteen zero bytes |

The descriptor array index is the global picture ordinal. The entire fixed
manifest is copied, hashed and validated before any VE resource access.
Each subsequently received AU and optional oracle is copied, authenticated,
parsed and checked against common geometry/configuration before that AU is
published. Future AU headers are not presumed valid from their manifest hashes.
Partial uploads never publish provider access. Every fragment and command checks
the current epoch, generation, authority and monotonic deadline. The provider
binds the immutable profile/spans and each active AU's ordinal, parsed fields
and actual length; the reusable address alone grants no authority.

### Private wire protocol and copied output

Every command has a 32-byte little-endian header: four 32-bit words containing
magic, version 1, argument and auxiliary value, then epoch and generation as
64-bit words. Commands are:

| Magic | Argument / auxiliary | Body |
| --- | --- | --- |
| `0x4d363248` (H26M) | Count / count*96 | Manifest descriptors; once only |
| `0x46363248` (H26F) | Ordinal / zero | Admitted AU, then optional oracle |
| `0x51363248` (H26Q) | Ordinal / zero | None; request copied result |
| `0x41363248` (H26A) | Ordinal / zero | Copied-output SHA256; acknowledge |

A VGA response has the existing 160-byte header layout with version 3.
The digests at offsets 32, 64 and 96 identify manifest, access unit and copied
output. The eight words at offset 128 identify ordinal, count, frame number,
POC, width, height, slice type and terminal state. Status 1 appends 460,800 bytes;
0 is pending, 2 is failed and 3 means that the copied output was acknowledged.
Pending, failed and acknowledged responses carry no payload/output digest.
A retained copied result still has status 1 after a later failure.

USB sends the header separately from payload chunks. The host reads exactly
160 bytes, validates the bounded length, then reads that payload length. It
tolerates endpoint zero-length packets within deadline/fragment limits. The host
verifies metadata and copied digest, writes each result durably and only then
sends an acknowledgment (ACK). An optional sealed oracle additionally checks every sample.
`oracle_verified` remains false for normal output: copied integrity alone does
not establish numerical decoder conformance. Invalid/duplicate/stale ACKs fail
without freeing a copied result. Protocol failure leaves authenticated retained
outputs queryable within the supervised lifetime.

Use [stream.py](../apps/h264/stream.py) to split a prepared Annex B clip into
bounded AU files and a manifest. It removes AUD/SEI metadata, repeats the common
SPS/PPS prefix and optionally splits an independent planar reference per picture.
Pass the resulting `manifest.bin` to [prepare.py](../apps/h264/prepare.py) with
an admitted version 2 session template. The version 4 bundle seals every AU and
oracle alongside manifest, image, launcher and session. The same ELF serves new
clips through new admitted sessions. Legacy session version 1 retains bundle
version 3 and its original upload/query/response formats.

### Focused evidence and remaining physical gaps

Evidence is under `~/.cache/sunstoneos/evidence/h264-vga-20261019`:

- The libx264 clip has six VGA pictures, I/P/P/I/P/P, with an independent FFmpeg
  pixel reference and syntax trace. ASan/UBSan parser checks compare header
  fields and bounds; refusal checks use independent field positions for direct
  inference, weighted prediction and unsupported PPS flags.
- `a733-vga-reference.json` compares primary stride/chroma length, repeated-IDR
  POCs and frame-SRAM fields with pinned original A733 instructions.
- `native-refill/result.json` runs the linked parser, immutable upload protocol,
  real allocator, IOMMU builder/validator, controller, guarded copy and serializer.
  It covers the IDR boundary, reference alternation, copied-output backpressure
  and ACK, wrong ordinal/digest, a late fault refusing refill without a next
  trigger, and retained copied output after a later engine error.
- `native-iommu-read-fixed/result.json` executes actual IOMMU scan/poll through
  the real native read, Device-mapping predicate and ordered loads. It records
  all 26 addresses in both banks and decoder masters 6/7, plus token/deadline/
  range/alignment/mapping refusal and a retained observed decoder fault.
- `host/result.json` covers exact-length/ZLP capture, durable output before ACK,
  qualification and retained partial failure. `final-bundles/result.json` covers
  final version 4 bundle identities. Normal copied output, legacy two-picture
  native behavior and the single-IDR parser wrapper have focused compatibility
  evidence. Earlier unchanged parser cases are reused.

The VGA launcher accepts session versions 1 and 2. A focused check in
`checks/h264-launcher.py` executes the actual generated AArch32 launcher with
producer-built legacy and VGA sessions, including unsupported-version refusal.
The earlier host and bundle checks substituted launcher construction. They did
not cover its session-version guard; the retained earlier launcher refuses VGA
with reason 14 before native startup. Its native ELF and decoder evidence remain
preserved. The corrected launcher has separate evidence under
`h264-launcher-20261019`; launcher instruction checks model system registers,
counters and MMIO and stop before the hardware handoff.

Memory-mapped I/O (MMIO) values, power/isolation facts, initial IOMMU installation, CPU
translation facts, USB transport and device pixel writes remain modeled at the
stated check boundaries. The lowest-read check executes the actual policy;
its CPU translation result is supplied by the model. Injected pixels verify
copying and protocol integrity. They do not establish physical codec arithmetic. The accepted
pre-refill-scan image and results remain frozen under `pre-refill-scan`.
Physical reference visibility, pixel conformance, confinement and supervised
timing remain unqualified. Healthy-frame reuse follows the pinned decoder
completion model; fault drain, unmapping and allocation reclamation remain
unsupported. No physical attempt occurred.

## Retained single/two-picture profile

The native driver implements a bounded Annex B H.264 sequence: one IDR intra
picture followed by an optional P picture that references the decoded IDR.
It parses all sequence parameter set (SPS), picture parameter set (PPS) and
slice headers before the first device write. The video engine (VE) executes
CAVLC entropy decoding, intra/inter prediction, inverse transforms, deblocking
and output writes. The CPU validates headers and bounds; it does not decode
macroblock payloads. Malformed payloads can pass header admission and fail in
the engine.

The driver supports progressive, macroblock-aligned Baseline 8-bit 4:2:0
pictures from 16 through 64 pixels wide and 16 through 32 pixels high. A clip
has at most 8,192 bytes, with at most 4,096 bytes in each NAL unit. It contains
one SPS and PPS, one slice per picture starting at macroblock zero, and at
most two pictures. Access-unit delimiters and supplemental enhancement
information may precede slices; supplemental metadata does not change samples.

The two-picture mode requires picture-order count (POC) type 2, frame numbers
0 then 1, POCs 0 then 2, one short-term reference and default reference list 0.
The P slice may override the active-reference count only to one. Weighted
prediction, reference-list modification, adaptive reference marking and frame
number gaps are rejected. A single IDR also accepts the existing POC type 0
mode. CABAC, B pictures, fields, multiple slices, cropping, slice groups,
redundant slices, long-term references, PPS extensions, scaling matrices and
HRD scheduling are unsupported. Header checks do not establish complete
macroblock coverage; completion also requires the decoded macroblock count.
The broader Baseline/Main/High families, longer clips, multiple references and
advertised codec size limits remain required work.

The application accepts runtime clips whose pictures are all 48 by 32 pixels.
It copies 2,304 planar Y, Cb and Cr bytes per picture into separate immutable
private outputs. The firmware contains no clip or pixel reference. Each sealed
session binds a nonzero stream digest. An optional independent pixel reference
is a qualification oracle; a zero reference digest selects normal output.
Normal output checks ownership, completion, metadata and copied byte integrity
without asserting numerical conformance.

One upload owns the decoder for its retained lifetime. The same ELF can accept
a different clip in a new admitted session without rebuilding firmware. A
completed or failed session cannot accept another clip, reclaim its buffers or
release its leases. Decoding the second picture uses retained resources;
repeated clips within one session remain unsupported.

### Pinned sources establish control and reference fields

The executor adapts the GPL-2.0-or-later H.264 control in
[Linux Cedrus](https://github.com/torvalds/linux/blob/70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56/drivers/staging/media/sunxi/cedrus/cedrus_h264.c).
The parser adapts the syntax order in H6-CedarC `H264DecodeSps`,
`H264DecodePps` and `H264DecodeSliceHeader`, replacing hardware bit readers
with bounded software reads. Those sources provide field and allocation
semantics under their LGPL version 2 notice.
[The source manifest](../apps/h264/sources.json) records per-file hashes,
revisions, source URLs, host-only oracle dependencies and preserved licenses.

The A733/T736 CedarC package supplies an architecture-specific instruction
reference, described in [video-codecs.md](video-codecs.md). Its `libawh264.so`
has SHA256
`c977fe936a0bf1418f0a452c9769158b3ad2c8ff32aa972c1db4e21630f7fcc3`.
Selected original AArch64 instructions execute on the host with inert register
storage and bounded service substitutions. These libraries are neither linked
into the FreeRTOS image nor loaded by the native application.

The instruction checks establish H.264 group offset `0x200` for the three
admitted compound identities and selected SPS, PPS, slice, bitstream, scratch
and frame-SRAM fields. The original `H264InitPoc` produces POCs 0 and 2 for the
admitted frame numbers. Frame-SRAM slots 0 and 1 contain those actual POCs,
progressive-frame flags and distinct Y, C and motion-vector addresses. Default
reference list 0 selects retained slot 0. The primary-only
`H264ConfigureFrameInfoRegister` branch programs planar YU12; secondary output
and the vendor scaling helper are unused. The encoded VLD base is not a raw
physical address. Motion-vector top and bottom fields are 1,024 bytes apart.

Picture-info and neighbor scratch belong to the retained decoder context.
[Cedrus context allocation](https://github.com/torvalds/linux/blob/70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56/drivers/staging/media/sunxi/cedrus/cedrus_h264.c#L526)
and the pinned CedarC `h264_dec.c` framebuffer-allocation code (lines 1355–1384)
preserve this
storage across pictures; `h264_hal.c` (lines 1158–1177) programs the context
addresses for each
picture. Both pictures therefore use the same initialized scratch allocations.
The CPU neither clears nor relocates them between IDR and P decoding. Distinct
input, Y, C and motion-vector slots retain the producer picture's reference
state while the P picture executes.

### Providers retain the decoder and all DMA storage

The application reuses the video platform, shared clock and power leases,
decoder IOMMU, uncached pool and native USB providers. A typed H.264 adapter
shares cold acquisition and reset sequencing with the existing video provider.
H.264 selects codec one and a separate register subset. The JPEG public layout
and codec-zero preparation path remain unchanged.

The privileged session reserves decoder masters 6 and 7, interrupt 81 and all
six buffer extents. Admission requires reviewed initial transaction and
page-walk isolation, electrical state, uncached ownership, alias exclusions,
retained context/reference mappings and manual whole-board power removal.
Idle register values do not establish those facts. The application does not
manufacture admission records. Primary EL3 snapshots the authenticated full
session before application access; the application requires its board prefix
to match the providers' admitted session. Later upload changes cannot alter
ownership, clock selection or evidence identities.

The decoder's I/O virtual address (IOVA) mappings remain static throughout both
pictures:

| IOVA range, exclusive end | Mapped capacity | Device access |
| --- | --- | --- |
| `0x10001000..0x10003000` | Two 4,096-byte padded input slots | Read |
| `0x10010000..0x10012000` | Two 4,096-byte luma slots | Read/write |
| `0x10014000..0x10016000` | Two 4,096-byte chroma slots | Read/write |
| `0x10020000..0x10041000` | 139,264-byte context picture-info storage | Read/write |
| `0x10068000..0x1006a000` | Two 4,096-byte motion-vector slots | Read/write |
| `0x10070000..0x1007c000` | 49,152-byte context neighbor storage | Read/write |

Luma and chroma mappings permit the P picture's reference reads. No mapping,
lease or context scratch is reset, unmapped or reused between pictures.
Picture-info requires 135,168 bytes; its remaining tail retains canaries.
Each motion-vector slot uses two 1,024-byte fields. Neighbor storage has a
16 KiB aligned CPU and IOVA base. Every allocation has unmapped guard pages;
the physical prefix is 16 KiB for neighbor storage and 4 KiB otherwise.
Guards and unused slot tails retain canaries. The separate IOMMU table occupies
32 KiB. These capacities do not qualify physical burst bounds or confinement.

Device luma has a 64-byte stride and 32 rows. Each chroma plane has a 32-byte
stride, 16 rows and a 512-byte allocation. Each private output removes padding:
1,536 Y bytes, then 384 Cb bytes and 384 Cr bytes. Copying a later picture or
retaining a failure cannot modify an already published private output.

### Runtime upload validates the complete clip before provider access

The private USB endpoint follows the bounded asset pattern: receive declared
bytes into owned RAM, authenticate and parse them, then commit once. The
32-byte little-endian upload header contains:

| Offset | Field | Accepted value |
| --- | --- | --- |
| 0 | Magic, 32 bits | `0x55363248` (`H26U`) |
| 4 | Version, 32 bits | 2 |
| 8 | Annex B length, 32 bits | 1 through 8,192 |
| 12 | Oracle length, 32 bits | 0, or 2,304 times the parsed picture count |
| 16 | Epoch, 64 bits | Current sealed session epoch |
| 24 | Generation, 64 bits | Current sealed session generation |

The body contains the complete stream followed by the optional oracle. USB
reads stop at header, stream and oracle boundaries, with at most 512 bytes per
iteration. Fragmentation and coalescing do not change lengths. Every fragment
checks generation, authority, input digests and deadline. Partial or truncated
input remains unpublished until completion or the supervised deadline.

The producer hashes the copied bytes, parses every picture and enforces common
48-by-32 geometry and oracle length. A fresh clock check rejects late commits.
Release/acquire publication transfers read access to the decoder task; VE
resources cannot be acquired before publication. Duplicate commits and writes
after commit fail without changing published bytes and revoke board liveness.
The CPU clip, parsed sequence and optional oracle remain owned until power
removal. The native provider copies each slice NAL, including emulation
prevention bytes, to byte zero of its separate padded device input slot.

[The bundle producer](../apps/h264/prepare.py) takes `--input` and optional
`--reference`. The session template must contain their digests; absent reference
requires a zero digest. Bundle version 3 seals these files with image, launcher
and session. Input travels through the application endpoint after enumeration,
outside ROM image-upload spans. Host capture derives the upload from that bundle.

### Completion gates sequential programming and immutable output

Before mappings or codec programming, the executor parses the immutable clip. It checks the supported compound identity, H.264 mode, disabled framebuffer
compression, clear video control unit enable/reset, clear LBC and timeout enable,
and no stale status or active H.264 pipeline. Unsupported state retains
ownership and refuses decode.

Programming derives syntax, quantization and deblocking fields from parsed
headers. It selects the picture's frame-SRAM slot, POC, input and output/MV
addresses while preserving the shared scratch bases. SDROT control is zero;
secondary-output addresses and scaling triggers are absent. The VLD end is
inclusive over the picture's 4,096-byte padded slot, while its bit length covers
only the slice NAL.

The executor initializes software decode with trigger 7, flushes parsed header
bits including the NAL header in chunks of at most 32, polls VLD busy after each
chunk and triggers slice decode with 8. The pinned
[Cedrus `cedrus_skip_bits` comment](https://github.com/torvalds/linux/blob/70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56/drivers/staging/media/sunxi/cedrus/cedrus_h264.c#L322)
documents intermittent corruption with nonzero VLD offsets. Separate offset-zero
slice copies preserve that source-backed path for complete Annex B clips.

Errors take precedence over completion. Completion with pipeline, transform,
bitstream-DMA or writeback busy remains pending. The decoded macroblock count
at group offset `0x2c` must equal the full picture's macroblock count. The
executor disables codec interrupt enables and acknowledges only observed low
status bits. Copying requires fault scans for both masters, quiet status and
intact guards. Qualification mode additionally compares every sample with its
picture's independent oracle. A second scan and status/guard check precede the
private digest. The task publishes that picture only after the executor's
post-callback deadline and liveness checks pass.

Only then may the next picture be programmed. The existing provider lease,
IOMMU mappings, reference slots and context scratch remain held. This follows
[Cedrus normal job completion](https://github.com/torvalds/linux/blob/70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56/drivers/staging/media/sunxi/cedrus/cedrus_hw.c)
and the A733 decoder's busy/status/macroblock checks. It establishes a bounded
source-backed next-picture control path. Physical producer-reference visibility
and accepted-transaction drain on this board remain unqualified. Flags, canaries, fault
scans and CPU barriers do not prove either property. No allocation reclamation
is authorized. Success and failure require whole-board power removal before
reuse; failure of the P picture preserves the completed private IDR output.

### Indexed USB output and focused evidence

A query is five bytes: zero followed by a little-endian 32-bit picture index.
The response has a 160-byte version 2 header. Its first 128 bytes contain the
magic/version, per-picture status, byte count, epoch/generation and
input/reference/private-output digests. Eight 32-bit fields follow:

| Offset | Field |
| --- | --- |
| 128 | Requested picture index |
| 132 | Committed picture count, or zero before upload |
| 136 | Frame number |
| 140 | Picture-order count |
| 144 | Width |
| 148 | Height |
| 152 | Slice type: 2 for I, 0 for P |
| 156 | Clip terminal state: 0 pending, 1 copied, 2 failed |

Copied per-picture status is 1 and appends 2,304 bytes; pending is 0 and failure
is 2, with no payload or output digest. A completed IDR remains status 1 even
if the clip later fails. Host capture verifies identities, geometry, byte count
and digest, then persists each output independently before querying the next.
It writes a joined clip only if all pictures succeed. Qualification mode also
compares each payload with the sealed oracle; normal mode reports
`oracle_verified` false. USB health, assertion and invalid-request failures
revoke board liveness and retain resources.

The image retains the existing 60-second supervised lifetime and 10 ms margin.
Every external executor operation checks a monotonic deadline before and after
it. Timeout, backward time, unexpected callback results and device errors retain
failure. Host modeling does not qualify the physical timing window.

Focused evidence is under
`~/.cache/sunstoneos/evidence/h264-interframes-20261019`:

- `input.h264` is an actual two-picture libx264 stream with inter-predicted P
  macroblocks; `reference.yuv` is the independent FFmpeg pixel oracle.
  `headers.log`, `oracle-result.json` and `a733-reference-trace.json` compare
  parsed syntax/register fields with FFmpeg and pinned A733 POC/reference helpers.
- `checks/h264-sequence.c` composes the actual controller and video provider.
  It covers two-picture completion, engine failure, incomplete macroblock count
  and copy refusal while preserving the first output. The bounded parser checks
  use ASan/UBSan and reject changed frame numbers, list modification, adaptive
  marking and a third picture without publishing partial results.
- `native-context/result.json` records the corrected persistent-scratch image's
  composed native upload, actual allocator, IOMMU construction/validation,
  controller, guarded copy and indexed serializer. MMIO, power/admission callbacks,
  USB transport and device pixel writes are modeled. It checks normal output,
  qualification output and retained IDR on P failure; injected pixels do not
  establish codec accuracy. `native-shared` records the final shared-header build. The final-image producer
  identity check and bundles are in `producer-final-comments`; the allocated bytes differ
  from the `host-context` image only in 32 build-identity bytes.
- `host-context/result.json` records actual bundle production and indexed durable
  capture against native-produced frames. Physical admission/transport and
  launcher construction are modeled. The linked dependency inventory and frozen
  evidence identify which ELF each check used.

The unchanged single-IDR parser cases and their four libx264/FFmpeg references
remain under `h264-stream-20261018`; upload/session refusal evidence remains under
`h264-input-20261018`. The primary-source pixel/register evidence remains under
`h264-native-20261017` and `h264-native-20261018`. These accepted checks are reused
where their source and behavior are unaffected.

The lower native VE read policy admits the decoded-macroblock counter at
`0x01c0e22c`. `checks/h264-native-access.py` executes the actual native read
function with inert MMIO and checks this read plus neighboring/null refusal.
Its focused evidence is under `h264-native-access-20261019/final`. Earlier
controller/native sequence models substituted the load callback and bypassed
this lower whitelist. Their accepted image would refuse before reading the
counter, retaining failure without publishing that picture. Those results
remain evidence for their stated modeled paths; the whitelist correction has
no physical qualification.

Physical H.264 execution, inter-picture reference visibility, pixel conformance,
initial authority, DMA confinement and supervised timing remain unqualified.
The missing prerequisite for claims about safe reuse is evidence that completed
producer writes are visible to subsequent reference reads and that accepted
transactions have drained before reclamation. This implementation retains all
resources rather than claiming that reclamation is safe. Both boards are
unavailable for recovery; no physical attempt is authorized by this evidence.
The coordinator owns any later supervised board attempt.
