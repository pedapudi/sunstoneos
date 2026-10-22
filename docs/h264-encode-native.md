# Native A733 H.264 encoding

The FreeRTOS application implements one retained sequence of 1 to 30 pictures.
It accepts two caller-supplied 640×480 NV12 frames, stride 640. Picture k encodes
supplied frame k mod 2. The sequence is one group of pictures: an IDR picture
followed by P pictures, each referencing the previous reconstructed picture.
Each copied AnnexB output has a 1 MiB limit. The fixed mode uses Baseline profile 66,
level 31, CAVLC, a fixed QP of 26 for every picture and 30 fps. Focused
instruction and host models pass. Physical encoding, numerical conformance,
reconstructed references, burst confinement and memory visibility remain
unqualified.

Other dimensions, formats, sequences longer than 30 pictures, a new frame for
every picture, rate control, High/CABAC/B pictures, multiple references and
unrestricted streaming remain required encoder scope. H.265 encoding and broader
decoder coverage remain required separately. No performance claim follows from
the models.

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
  use the source-defined fields at 0xba0–0xbc4. Picture k writes reconstruction
  and auxiliary pair k mod 2 and references the other pair; the IDR names pair 0
  for both.
- Encode at 0x2a8e0–0x2a908 resets/enables the codec for each frame while retaining
  external reconstructed references. The reviewed libVE branch pulses TOP+4
  bit 24, rereads, clears it, then selects TOP bits 6 and 7 with intervening reads.
- Trigger 0x271c4, wait 0x27220, count/status 0x2728c/0x272dc and publication 0x2784c
  establish the completed range. Frame-counter advancement follows publication.
- Default parameters at 0x1ffe0 set a 2 MiB minimum-free guard. A 1 MiB ring passes
  initialization but returns BITSTREAM_IS_FULL before codec programming.

The native mode retains one 4 MiB ring for the sequence. Each picture starts at
the 64-byte-aligned offset after the previous picture's completion count. The
previous picture's range is copied before the next picture is submitted, so its
ring bytes may be reused. When that offset exceeds 2 MiB, the next picture
starts at offset zero instead. At least 2 MiB therefore remains between a
picture's start and the ring end, as the source's minimum-free guard requires,
and a 1 MiB output never reaches the end. The library behaves differently: with
outputs released, it continues past the ring end and lets the device wrap, and
raises the 0xb8c limit above the ring size. The native mode never relies on
that unqualified device wrap.

The CPU writes each picture's start code and leading slice header bits into the
ring. The IDR header carries frame_num 0 and POC 0. The P header for picture k
carries frame_num k and POC least-significant bits 2k, both 12 bits wide, and
has six pending bits. The source-defined ready bit must precede the put-bits
command and final trigger. The pinned library inserts its next IDR after 30
pictures, which bounds one sequence. QP stays fixed; the source rate control is
not reproduced.

A pinned-library run of 32 pictures, each modeled with 512 payload bytes,
provides the reference. For all 30 pictures of the longest sequence, the 2,128
native ENCPP/encoder/extension writes and every CPU-written header match it
after DMA-address relocation. Its picture 30 is an IDR. The two-picture
sequence still reproduces the earlier 140 matching writes.

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
length 921600, the sequence length of 1 to 30 pictures, epoch and generation. Bounded fragments enter private
WB RAM. Complete hashing precedes publication, DMA input copies and device use.
Truncated input remains unpublished and expires at the admitted deadline.
Changed identities, duplicate commits and writes after commit refuse.

A query contains bytes 0 and picture ordinal and requires the committed upload.
The 160-byte result header binds status, length, epoch/generation, input/output
hashes, ordinal, sequence length and fixed codec metadata. Successful payload
bytes come from private WB output. An ACK contains command 1, LE32 ordinal,
LE64 generation and output digest. Actual USB transferred-byte completion gates
ACK consumption. A valid ACK of picture k permits submission of picture k+1; it
does not release any DMA extent. Wrong, early and duplicate ACKs cannot
authorize progression.

Picture k is copied into WB output k mod 2, which holds picture k-2 until then.
That copy requires the ACK of picture k-2, and picture k cannot start before
the ACK of picture k-1. Each output therefore stays immutable until the host has
acknowledged it and its successor. Once acknowledged, an output that a later
picture will overwrite cannot be queried again; such a query stops the session.

The host freezes the executable/session/upload identities, durably saves each
raw response, copied bitstream and result record, then sends that picture's ACK.
A later failure preserves earlier files and ledger entries. Overall success
requires every native output of the sequence. Host status explicitly leaves
numerical qualification false. The host decoder and upload helpers take the
sequence length; the supervised capture tool still requests and captures two
pictures.

## Fixed allocation fits the existing extended pools

The selected linker profile retains WB 0x48000000–0x78000000 and
NC 0x78000000–0x79000000. No CPU startup or shared memory reservation changes are
introduced. Actual linked allocator execution uses 6,254,592 NC bytes and
3,022,848 WB bytes. Application metadata, including its 32 KiB task stack and
per-picture state for 30 pictures, occupies 38,896 bytes outside those
allocations.

| Allocation | Retained bytes | Ownership |
| --- | ---: | --- |
| IOMMU tables |32,768| NC, master 3 only |
| Two raw DMA inputs |2×462,848| Read-only device mappings |
| One bitstream ring |4,194,304| Device read/write, reused after each copy |
| Two reconstructed pictures |2×491,520| Opaque device Y/C format |
| Two auxiliary buffers |2×12,288| Opaque device state |
| Two line buffers and misc |2×8,192+4,096| Device scratch |
| Guard pages and table alignment | Remaining NC allocation | Retained and checked |
| Raw upload |925,696| Private WB, immutable after commit |
| Two copied outputs |2×1,048,576| Private WB, immutable until acknowledged and reused |

The source reconstruction allocation splits 327,680 Y bytes and 163,840 C bytes;
it is not linear NV12. Eight IOMMU spans cover ten logical buffers. A guarded
scratch arena packs the two line buffers and miscellaneous state. Its internal
A5 guard pages are mapped and checked; outer guards remain unmapped. Actual
allocation alignment, capacity, translation permissions and guards are checked
before publication. Any uncertain completion freezes reference, input, ring,
copied-output and lease ownership.

## Preserved software evidence and remaining physical work

Evidence for the two-picture image is under
`~/.cache/sunstoneos/evidence/h264-encode-20261020/`. The initial linked image is `candidate-initial.elf`; the final image is
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

The sequence image passes the same native cases with byte-identical two-picture
programming and outputs. Further cases of `checks/h264-encode-native.py` run
longer sequences on it:

- `gop` encodes 30 pictures through the controller; `checks/h264-encode-registers.py`
  then matches all writes and headers against the 32-picture library reference.
- `task-wakes` with four pictures drives the actual task loop through each ACK.
- `ring-reuse` models 900,000-byte outputs; the fourth picture restarts at ring
  offset zero and both WB outputs keep their pictures.
- `unacked-copy` withdraws the ACK of picture 0 before picture 2; its copy is
  refused, the session faults and picture 0 stays intact.

`checks/h264-encode-usb.py` refuses a query of acknowledged picture 0 in a
six-picture sequence and still serves it in a two-picture sequence. These
results are under `~/.cache/sunstoneos/build/audit-h264-gop/checks/` and are not
frozen into the evidence store. Root integration has not been rerun for the
sequence image.

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
numerical qualification claim. A longer sequence on the board additionally needs
the supervised capture tool to request and acknowledge more than two pictures. All resources remain retained afterward. Fault
recovery, DMA confinement and memory visibility require physical evidence;
cancellation or software timeouts do not prove drain.
