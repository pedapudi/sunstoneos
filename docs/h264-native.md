# Bounded native H.264 IDR and P decoding

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

## Pinned sources establish control and reference fields

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

## Providers retain the decoder and all DMA storage

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

## Runtime upload validates the complete clip before provider access

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

## Completion gates sequential programming and immutable output

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

## Indexed USB output and focused evidence

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
