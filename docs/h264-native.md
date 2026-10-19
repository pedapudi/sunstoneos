# Bounded native H.264 IDR decoding

The native driver implements a bounded Annex B H.264 mode. It parses one
sequence parameter set (SPS), one picture parameter set (PPS) and one IDR
intra slice, then programs the video engine (VE) from those fields. The mode
supports progressive, macroblock-aligned Baseline 8-bit 4:2:0 pictures from
16 through 64 pixels wide and 16 through 32 pixels high. Input is limited to
4,096 bytes. Access-unit delimiters and supplemental enhancement information
may precede the slice; supplemental metadata does not alter decoded samples.

The engine executes CAVLC entropy decoding, intra prediction, inverse transforms,
deblocking and output writes. The CPU validates headers and bounds; it does not
decode macroblock payloads. A malformed payload can therefore pass header
admission and fail in the engine. Device errors, data requests, busy completion
and deadlines retain every allocation and lease.

The application accepts an uploaded 48-by-32 picture and copies 2,304 planar
Y, Cb and Cr bytes. The firmware contains no clip or pixel reference. A new
clip requires a new admitted session and prepared bundle, while the same ELF
can be reused. Each session binds a nonzero stream digest. Its optional pixel
reference is a qualification oracle; an all-zero reference digest selects
normal decoder output. Normal output checks ownership, completion and copied
byte integrity without asserting numerical conformance.

The upload owns one job for the retained decoder lifetime. A completed or
failed job cannot accept another clip, reclaim buffers or release its leases.
Runtime ingestion removes the need to rebuild firmware for each clip; repeated
decoding within one session remains unsupported.

Supported headers use one slice beginning at macroblock zero, picture-order
count type 0 or 2, at most one reference-frame declaration and no frame-number
gaps. CABAC, picture-order count type 1, multiple pictures or slices, interframe
prediction, field pictures, cropping, slice groups, redundant slices, long-term
reference storage, PPS extensions, scaling matrices and HRD scheduling are
unsupported. Header validation cannot establish complete picture coverage.
The wider Baseline/Main/High families and the board's advertised codec limits
remain required work.

## Open-source control and A733 instructions establish the operation

The executor adapts the GPL-2.0-or-later H.264 control in
[Linux Cedrus](https://github.com/torvalds/linux/blob/70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56/drivers/staging/media/sunxi/cedrus/cedrus_h264.c).
The parser adapts the syntax order in H6-CedarC `H264DecodeSps`,
`H264DecodePps` and `H264DecodeSliceHeader`, replacing hardware bit-reader
callbacks with bounded software reads. Those sources provide field and
allocation semantics under their LGPL version 2 notice. The source manifest is
[apps/h264/sources.json](../apps/h264/sources.json); it records per-file hashes,
revisions, source URLs and the preserved license files.

The A733/T736 CedarC package supplies an architecture-specific register
reference. Its package and library provenance are described in
[video-codecs.md](video-codecs.md). The relevant `libawh264.so` has SHA256
`c977fe936a0bf1418f0a452c9769158b3ad2c8ff32aa972c1db4e21630f7fcc3`.
Selected original AArch64 instructions ran on the host with inert register
storage and bounded service substitutions. These libraries are neither linked
into the FreeRTOS image nor loaded by the native application.

The instruction comparison establishes H.264 group offset `0x200` for the
three previously admitted compound identities. It also checks selected SPS,
PPS, slice, bitstream, scratch and frame-SRAM fields. The actual primary-only
branch of `H264ConfigureFrameInfoRegister` programs planar YU12 without enabling
secondary output. The vendor scaling helper is not used.

The metadata audit follows producer code as well as register consumers. In
particular, the VLD base field contains an encoded address rather than a raw
physical address. Motion-vector fields are 1,024 bytes apart for this picture.
The full audit and producer disassembly remain with the raw evidence. Frame
SRAM picture-order fields remain zero for this standalone I-picture mode; the
parsed picture-order count has no reference or output-reordering consumer.
Actual frame-order fields need implementation before those consumers are added.

## Existing providers retain the decoder and every DMA buffer

The application reuses the video platform, shared clock and power leases,
decoder IOMMU, uncached memory pool and native USB providers. A typed H.264
adapter shares cold acquisition and reset sequencing with the existing video
provider. The JPEG public layout and its codec-zero preparation path remain
unchanged. H.264 selects codec one and permits a separate register subset.

The privileged session reserves decoder masters 6 and 7, interrupt 81 and all six
buffer extents. Admission requires reviewed initial transaction and page-walk
isolation, electrical state, uncached physical ownership, alias exclusions and
manual whole-board power removal. Idle register values do not establish those
facts. The application does not manufacture admission records.

Primary EL3 copies the full session once and verifies its digest against the
immutable boot contract before publishing that snapshot to the other CPUs.
The application consumes the snapshot and requires its board prefix to match
the board providers' admitted session. Later changes to the uploaded session
cannot change decoder ownership, clock selection or evidence identities.

The I/O virtual addresses (IOVAs) presented by the decoder are fixed:

| IOVA range, exclusive end | Mapped capacity | Device access |
| --- | --- | --- |
| `0x10001000..0x10002000` | 4,096-byte padded input | Read |
| `0x10010000..0x10011000` | 4,096-byte luma allocation | Write |
| `0x10014000..0x10015000` | 4,096-byte chroma allocation | Write |
| `0x10020000..0x10041000` | 139,264-byte picture-info allocation | Read/write |
| `0x10044000..0x10045000` | 4,096-byte motion-vector allocation | Read/write |
| `0x10048000..0x10054000` | 49,152-byte neighbor allocation | Read/write |

The picture-info minimum is 135,168 bytes; its remaining mapped tail retains
canaries. Motion-vector data uses two 1,024-byte fields. The neighbor allocation
has a 16 KiB aligned CPU and IOVA base. Every allocation has an unmapped guard
page on each side; its physical prefix is 16 KiB for the neighbor buffer and
4 KiB otherwise. Guard bytes and unused tails retain canaries. The 32 KiB IOMMU
table is a separate allocation. These source-derived capacities do not qualify
hardware burst bounds or physical IOMMU confinement.

Output luma has a 64-byte stride and 32 rows. Each chroma plane has a 32-byte
stride, 16 rows and a 512-byte allocation. The private output removes row
padding and contains 1,536 Y bytes followed by 384 Cb and 384 Cr bytes.

## Runtime upload publishes immutable owned input before provider access

The private USB vendor endpoint uses the project's bounded asset pattern:
receive declared bytes into owned storage, validate them, then commit once.
The upload header is 32 bytes, with little-endian fields:

| Offset | Field | Accepted value |
| --- | --- | --- |
| 0 | Magic, 32 bits | `0x55363248` (`H26U`) |
| 4 | Version, 32 bits | 1 |
| 8 | Annex B length, 32 bits | 1 through 4,096 |
| 12 | Oracle length, 32 bits | 0 for normal output; 2,304 for qualification |
| 16 | Epoch, 64 bits | Current sealed session epoch |
| 24 | Generation, 64 bits | Current sealed session generation |

The body contains the stream followed by the optional oracle. USB reads stop
at header, stream and oracle boundaries, with at most 512 bytes per iteration.
Fragmentation and coalescing do not change those lengths. Every fragment checks
the same session generation, authority, input digests and deadline. Partial
input remains unpublished until completion or the supervised deadline.

The producer hashes the copied stream and optional oracle, parses the stream
and enforces the application's 48-by-32 bounds. A fresh clock check after that
validation rejects an expired commit. Release/acquire publication transfers
read access to the decoder task; the task cannot acquire VE resources before
publication. Writes and duplicate uploads after commit fail without modifying
the published bytes and revoke board liveness. The CPU stream and any oracle
remain owned until the prescribed whole-board power removal.

[The bundle producer](../apps/h264/prepare.py) takes `--input` and optional
`--reference`. The session template must contain their digests; a missing
reference requires a zero reference digest. Bundle version 2 seals these files
alongside the image, launcher and session. Input bytes travel through the
application endpoint after enumeration, outside the ROM image-upload spans.
The host capture tool derives the upload from the frozen bundle.

## Completion permits a verified private copy without reclaiming DMA

After acquisition, the executor parses the retained immutable CPU stream and reads the
supported compound identity before preparing mappings or codec registers. It
requires the selected H.264 mode, disabled framebuffer compression (AFBC)
mode fields, clear video control unit (VCU) enable
and reset controls, clear LBC enable and no stale status or active H.264 pipeline.
The top timeout-enable field must also remain clear, matching shared preparation.
Unknown identities and unsupported state retain ownership and refuse decode.

Programming derives SPS/PPS, quantization and deblocking fields from the parsed
headers and sets the bounded plane geometry, scratch pointers and
frame-SRAM slot zero. It writes zero to SDROT control and programs no
secondary-output addresses or scaling triggers.
The VLD end is inclusive and covers the 4,096-byte padded input allocation.
Only the encoded slice NAL contributes to the programmed bit length. The native
provider copies that slice from the immutable stream into the padded input
allocation at byte zero, preserving emulation-prevention bytes.

The executor follows Cedrus's offset-zero sequence: initialize software decode
with trigger 7, flush the parsed header bits including the NAL header in
chunks of at most 32, poll VLD busy after each chunk, then trigger slice decode
with 8. The pinned [Cedrus `cedrus_skip_bits` comment](https://github.com/torvalds/linux/blob/70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56/drivers/staging/media/sunxi/cedrus/cedrus_h264.c#L322)
documents intermittent corruption with nonzero VLD offsets. The separate slice
copy preserves the offset-zero path for complete Annex B inputs.

Errors take precedence over completion. A completion flag with any applicable
pipeline, transform, bitstream-DMA or writeback-busy bit remains pending.
The executor disables codec interrupt enables and acknowledges only observed
low status bits. It then requires fault scans for both masters, intact guards,
quiet H.264 status and, in qualification mode, equality with every independent
oracle sample. A second
scan and status/guard check precede the final private-copy digest.

The terminal state is COPIED. In normal mode this reports completed, owned
output with a digest; it does not assert a pixel oracle match. It permits access
to the immutable private result
while all mappings, buffers, clocks, power and interrupt reservations remain
held. Completion flags, canaries, fault scans and CPU barriers do not establish
accepted-transaction drain. Failure and success both require the prescribed
whole-board power removal before reuse.

## Supervision and copied USB output bound one attempt

The image uses the existing 60-second supervised board lifetime with the
existing 10 ms application margin. Every external executor operation has
monotonic deadline checks before and after it. Timeout, backward time, an
unexpected callback result or a device error produces retained failure.
Linking and host modeling do not qualify operation within that physical window.

A zero-valued USB request byte returns a 128-byte header. It contains the H.264
magic/version, status, byte count, session epoch/generation and input/reference/
private-output hashes. Only successful copied status appends the 2,304 planar
bytes. Pending and failure carry no pixel bytes or private-output digest.
The host retains raw frames, verifies the copied byte count, session identities
and payload digest, and checks its deadline before publishing the durable
result. Qualification mode also compares every copied pixel with the sealed
oracle. `oracle_verified` is false for normal output.
USB health, assertion and invalid-request failures revoke board liveness before
parking, preventing subsequent decoder operations while retaining its resources.

Focused host validation covers full-session sealing, executor/provider
sequencing, guarded native copying, pixel comparison, hashing, fault and busy
refusal, the native USB serializer, launcher admission and durable host capture.
The complete PCM stream preserves the earlier independent FFmpeg pixel oracle
and selected A733 library register trace. Four libx264 streams exercise predicted
intra macroblocks, residual transforms, nonzero deblocking offsets and the
minimum/maximum driver geometry. FFmpeg header traces independently establish
their syntax values and pixel references. ASan/UBSan checks damaged and truncated
inputs; the native executor's register model checks the planned fields.

The runtime-upload image, linked dependency closure, normal/qualification
bundles and focused checks are under
`~/.cache/sunstoneos/evidence/h264-input-20261018`.
The image is `build/sunstone-a733-h264.elf`. Native instruction checks cover
fragmented and coalesced upload, partial input withholding provider access,
late commit and duplicate upload refusal, copied output and the actual USB
serializer in both modes. Device pixel writes are injected in those checks;
normal-mode comparisons establish copied bytes and metadata, not codec accuracy.
ASan/UBSan input checks consume upload headers emitted by the actual host
producer and test session, length, mode, digest and deadline refusal.

The unchanged parser and register-planning evidence remains under
`h264-stream-20261018`; its four libx264 streams and FFmpeg header/pixel
references are reused. The primary-source and fixed pixel/register evidence
remains under `h264-native-20261017` and `h264-native-20261018`. Host FFmpeg/libx264
identities and execution boundaries remain in
[the source manifest](../apps/h264/sources.json).

Hardware registers, DMA results, initial physical authority and USB transport
remain modeled boundaries. Physical H.264 decoding, identity, DMA confinement,
pixel conformance and supervised timing remain unqualified. Both boards are
unavailable for recovery, so no physical attempt is authorized by this evidence.
The coordinator owns any later board attempt.
