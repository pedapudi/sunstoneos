# One finite H.264 hardware decode path

The native application implements one fixed H.264 operation: a 48-by-32,
progressive, 8-bit 4:2:0 Baseline IDR picture with one slice and six I_PCM
macroblocks, which carry pixel samples directly. Deblocking is disabled.
The nonconstant samples exercise bitstream
consumption, macroblock placement and planar DMA output. This operation does
not cover prediction, transform coefficients, interframe references, CABAC,
multiple slices, cropping, scaling or the wider Baseline/Main/High families.

The complete Annex B stream is 2,343 bytes. An independent FFmpeg decoder
reconstructed all 2,304 original Y, Cb and Cr samples. Native execution accepts
only the 2,320-byte IDR network abstraction layer (NAL) unit, without its start
code or parameter-set NALs. The register parameters correspond to the fixed
sequence and picture parameter sets (SPS/PPS) and slice header; the
application does not parse arbitrary H.264 input.

| Artifact | SHA256 |
| --- | --- |
| Complete stream | `086a771d3323a5c5c0ad4615b2fd91a400b8022bcb180ff66e2b4986e4224ae9` |
| Native slice input | `7021224278372ceb05be72796579c4b412e6f7758280d855a5d07f1225649d69` |
| Independent planar output | `4398156e0c9291091780d378ff220793691a3dd38eabf483f9e2e9400d6af72e` |

## Open-source control and A733 instructions establish the operation

The executor adapts the GPL-2.0-or-later H.264 control in
[Linux Cedrus](https://github.com/torvalds/linux/blob/70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56/drivers/staging/media/sunxi/cedrus/cedrus_h264.c).
The pinned H6-CedarC H.264 sources provide additional field and allocation
semantics under their LGPL version 2 notice. The source manifest is
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
The full audit and producer disassembly remain with the raw evidence.

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

## Completion permits a verified private copy without reclaiming DMA

After acquisition, the executor verifies the complete slice hash and reads the
supported compound identity before preparing mappings or codec registers. It
requires the selected H.264 mode, disabled framebuffer compression (AFBC)
mode fields, clear video control unit (VCU) enable
and reset controls, clear LBC enable and no stale status or active H.264 pipeline.
The top timeout-enable field must also remain clear, matching shared preparation.
Unknown identities and unsupported state retain ownership and refuse decode.

Programming sets the fixed picture and plane geometry, scratch pointers and
frame-SRAM slot zero. It writes zero to SDROT control and programs no
secondary-output addresses or scaling triggers.
The VLD end is inclusive and covers the 4,096-byte padded input allocation.
Only the 2,320-byte slice contributes to the programmed bit length.

The executor follows Cedrus's offset-zero sequence: initialize software decode
with trigger 7, flush the 24 header bits including the NAL header, poll VLD
busy, then trigger slice decode with 8. Cedrus documents failures from using a
nonzero VLD offset directly, so that shortcut is excluded.

Errors take precedence over completion. A completion flag with any applicable
pipeline, transform, bitstream-DMA or writeback-busy bit remains pending.
The executor disables codec interrupt enables and acknowledges only observed
low status bits. It then requires fault scans for both masters, intact guards,
quiet H.264 status and equality with every independent oracle sample. A second
scan and status/guard check precede the final private-copy digest.

The terminal state is COPIED. It permits access to the immutable private result
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
The host retains raw frames, verifies the complete copied output and checks
its deadline before publishing the durable result.
USB health, assertion and invalid-request failures revoke board liveness before
parking, preventing subsequent decoder operations while retaining its resources.

Focused host validation passed using one linked native image. The checks cover
full-session sealing and refusal, executor/provider sequencing, guarded native
copying, pixel comparison, hashing, fault and busy refusal, the native USB
serializer and shared revocation, launcher admission and durable host capture.
Hardware registers, DMA results, initial physical authority and USB transport
remain modeled boundaries in those checks.

The frozen image and compiled dependency closure are under
`~/.cache/sunstoneos/evidence/h264-native-20261018`.
The ELF SHA256 is
`3cf76267d42f5dff600677a940d45d34af14557daabb66ad733dd8587432a5c4`.
The embedded build ID is
`51fecb35477e907c53e522770bd380d1c6cb94457133e4a192ae0471afe0ab9a`.
The evidence preserves failed setup and fixture attempts, their input snapshots
and the reviewed corrections. Successful admission, executor and native cases
were reused when later packaging inputs changed. The independent FFmpeg output
and selected vendor instruction traces remain in the earlier evidence directory
`~/.cache/sunstoneos/evidence/h264-native-20261017`.

Physical H.264 decoding, identity, DMA confinement, pixel conformance and
supervised timing remain unqualified. The coordinator owns any board attempt.
