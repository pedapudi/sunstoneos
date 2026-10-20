# Native A733 H.264 encoding

The FreeRTOS application implements one retained two-picture job. It accepts two
caller-supplied 640×480 NV12 frames, stride 640, and encodes an IDR picture followed
by a P picture using the first reconstructed picture. Each copied AnnexB output
has a 1 MiB limit. The fixed mode uses Baseline profile 66, level 31, CAVLC, QP26 and
30 fps. Focused instruction and host models pass. Physical encoding, numerical
conformance, reconstructed references, burst confinement and memory visibility
remain unqualified.

Other dimensions, formats, frame counts, rate control, High/CABAC/B pictures,
multiple references and unrestricted streaming remain required encoder scope.
H.265 encoding and broader decoder coverage remain required separately. No
performance claim follows from the models.

## Encoder authority and source-derived programming

The application uses the existing encoder resource coordinator: VE_ENC ownership,
MMIO 0x01c10000, TOP+0x800, ENCPP+0xa00, encoder+0xb00, extension+0xc00,
PCK 3, IRQ 80, NSI 9 and IOMMU master 3. Its distinct typed H.264 wrapper shares the
actual resource lock and exclusive VE_ENC lease with JPEG. The common registry
refuses simultaneous JPEG/H.264 ownership in either acquisition order. Stable
parent, fabric, rail and OSC/PCK leases persist. Separate IOMMU preparation
excludes SYS/VI/DE/VE_DEC writers without reacquiring its own VE_ENC lease.

The inherited-powered mode requires authenticated prior transaction/walk drain,
all-master exclusion, skipped-master quiescence and absence of remote writers.
The provider checks powered PCK 3, clocks, gates, reset state, inactive masked IRQ 80
and compound identity 0x0003331000021320 before codec access. Register readback
cannot establish the admission facts. The existing cached multicore startup and
its physical prerequisites still apply.

The effective A733 CedarC implementation is available only as pinned library
instructions. The native image links no foreign library and uses no Linux
runtime. [The dependency record](../apps/h264-encode/sources.json) pins the package,
libraries, public headers and notices. Older open-source encoder headers do not
establish this chip's register sequence.

Relevant libvenc_codec.so instruction anchors are:

- H264InitVer2 0x24838–0x248a0 selects register groups 0/12/14/17.
- Allocation at 0x24b8c–0x24c58 establishes padded reconstruction geometry.
- Normal-P reference selection at 0x1f9e8–0x1fa3c selects the preceding frame and
  the opposite destination. Reference/output Y/C, auxiliary and line addresses
  use the source-defined fields at 0xba0–0xbc4.
- Encode at 0x2a8e0–0x2a908 resets/enables the codec for each frame while retaining
  external reconstructed references. The reviewed libVE branch pulses TOP+4
  bit 24, rereads, clears it, then selects TOP bits 6 and 7 with intervening reads.
- Trigger 0x271c4, wait 0x27220, count/status 0x2728c/0x272dc and publication 0x2784c
  establish the completed range. Frame-counter advancement follows publication.
- Default parameters at 0x1ffe0 set a 2 MiB minimum-free guard. A 1 MiB ring passes
  initialization but returns BITSTREAM_IS_FULL before codec programming.

The native mode retains a 4 MiB ring for both pictures. It derives the next
64-byte-aligned cursor from the actual first completion count and preserves the
2 MiB free-space guard. It neither wraps nor rewinds the ring. The P header has
six pending bits; the source-defined ready bit must precede the put-bits command
and final trigger. Fixed headers and 140 native ENCPP/encoder/extension writes
match the preserved instruction reference after DMA-address relocation.

Completion requires finish status without stall/error, a nonempty byte-aligned
absolute bit count, an in-ring range and a copied-output bound. The application
checks fault/status/count/guards around bounded 32 KiB copy chunks and again before
publication. The first output remains immutable through a later failure. Source
completion supports copying the bounded range; it does not establish a fabric
reclamation condition. Every resource remains retained through board power removal.

## Sealed caller input and indexed copied output

The 856-byte session contains the ordinary 592-byte board prefix, owners, four
physical-admission evidence digests and a nonzero SHA-256 of 921,600 raw bytes.
The reserved reference-oracle digest must be zero. Encoding requires caller
pixels; it does not require a numerical reference output. The protected 288-byte
contract retains the existing extended-memory layout and appends separate session
and entry digests. Both retained session copies publish before provider setup.
The synchronous board-only view cannot escape validation.

The 32-byte little-endian upload header contains magic 0x55454853, version 1,
length 921600, frame count 2, epoch and generation. Bounded fragments enter private
WB RAM. Complete hashing precedes publication, DMA input copies and device use.
Truncated input remains unpublished and expires at the admitted deadline.
Changed identities, duplicate commits and writes after commit refuse.

A query contains bytes 0 and picture ordinal. The 160-byte result header binds
status, length, epoch/generation, input/output hashes, ordinal and fixed codec
metadata. Successful payload bytes come from immutable private WB output.
An ACK contains command 1, LE32 ordinal, LE64 generation and output digest.
Actual USB transferred-byte completion gates ACK consumption. A valid ACK 0
permits submission of the P picture; it does not release the first output or any
DMA extent. Wrong, early and duplicate ACKs cannot authorize progression.

The host freezes the executable/session/upload identities, durably saves each
raw response, copied bitstream and result record, then sends that picture's ACK.
A later failure preserves earlier files and ledger entries. Overall success
requires both successful native outputs. Host status explicitly leaves numerical
qualification false.

## Fixed allocation fits the existing extended pools

The selected linker profile retains WB 0x48000000–0x78000000 and
NC 0x78000000–0x79000000. No CPU startup or shared memory reservation changes are
introduced. Actual linked allocator execution uses 6,254,592 NC bytes and
3,022,848 WB bytes. Application metadata, including its 32 KiB task stack, occupies
36,960 bytes outside those allocations.

| Allocation | Retained bytes | Ownership |
| --- | ---: | --- |
| IOMMU tables |32,768| NC, master 3 only |
| Two raw DMA inputs |2×462,848| Read-only device mappings |
| One bitstream ring |4,194,304| Device read/write, no rewind |
| Two reconstructed pictures |2×491,520| Opaque device Y/C format |
| Two auxiliary buffers |2×12,288| Opaque device state |
| Two line buffers and misc |2×8,192+4,096| Device scratch |
| Guard pages and table alignment | Remaining NC allocation | Retained and checked |
| Raw upload |925,696| Private WB, immutable after commit |
| Two copied outputs |2×1,048,576| Private WB, immutable after publication |

The source reconstruction allocation splits 327,680 Y bytes and 163,840 C bytes;
it is not linear NV12. Eight IOMMU spans cover ten logical buffers. A guarded
scratch arena packs the two line buffers and miscellaneous state. Its internal
A5 guard pages are mapped and checked; outer guards remain unmapped. Actual
allocation alignment, capacity, translation permissions and guards are checked
before publication. Any uncertain completion freezes reference, input, ring,
copied-output and lease ownership.

## Preserved software evidence and remaining physical work

Evidence is under `~/.cache/sunstoneos/evidence/h264-encode-20261020/`.
The initial linked image is `candidate-initial.elf`; the final image is
`build/sunstone-a733-h264-encode.elf`. Root's final native admission correction
changes only admission and build-identity objects. The other 168 objects remain
byte-identical, so successful leaf checks retain their scope without replay.
`compiled-final/` contains the compiler-input inventory and raw dependency records.
The frozen manifest binds sources, objects, tools, reference inputs and results.

Focused checks execute the actual linked allocator/upload/controller/provider,
lowest native MMIO policy, IOMMU scan/copy, application publication and USB ACK
path. They cover IDR/P progression, header-ready timeout, stall/error/nonfinish,
empty/unaligned/oversized counts, a late copy fault, retained P failure, input
identity/truncation/duplicate refusal and copied-output backpressure. Private
packaging/capture and actual supervisor entry also pass. Root separately checks
the protected contract, full session lifetime and final linked layout.

The models inject entropy, completion and resource observations. FFmpeg parses
independent SPS/PPS/slice metadata for the copied sequence, but its decoder reports
invalid macroblock data because the entropy is a placeholder. Neither injected
entropy nor register matching qualifies numerical encoding or reference contents.
Early reference checkpoints retain their recorded source hashes but lack copies
of some intermediate model revisions. Fixture setup failures and resumed cases
remain explicit in the closure record; completed cases were preserved.

A first supervised board measurement still requires actual target admission,
retained encoder/fabric/power authority, inherited isolation and the existing
manual recovery conditions. The next physical check should upload nonconstant
raw frames, capture both outputs and independently decode their concatenation.
It must verify dimensions, IDR/P order and reference-dependent pixels before any
numerical qualification claim. All resources remain retained afterward. Fault
recovery, DMA confinement and memory visibility require physical evidence;
cancellation or software timeouts do not prove drain.
