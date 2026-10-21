# Native JPEG application decodes authenticated runtime images

The application accepts one caller-supplied baseline YCbCr420 JPEG per admitted
version 2 session, or a manifest of up to eight such images per version 3
session, which it decodes in order under the same retained resources. It authenticates and parses an owned CPU copy before acquiring any JPEG
decoder power, clocks, memory or device access. It returns copied active planar
Y/Cb/Cr pixels and the actual decoded dimensions. An independent pixel oracle
is optional. Ordinary decoding verifies input and copied-output integrity;
it does not establish numerical correctness.

The native target is `sunstone-a733-jpeg`, configured from
`apps/jpeg/CMakeLists.txt` with the existing pinned FreeRTOS, TinyUSB, networking
and C-header dependencies. It links the unchanged supervised board startup,
JPEG parser/executor, cold decoder platform and IOMMU driver. The image has no
embedded JPEG or reference and requires no registered fixture pair. Existing
constant and nonconstant diagnostic generators remain available in
`apps/jpeg/fixture.py`.

The board composition retains its PMIC, USB0 and software supervisor tasks. The
JPEG owner waits for an authenticated upload before requesting its resources.
It inherits the existing all-core startup and does not qualify sustained
multicore scheduling. The sixty-second supervised lifetime never arms a
watchdog. Confirmed removal of every board supply is the recovery operation.
Persistent storage is untouched.

## Session authority precedes input admission

The session retains its 856-byte layout and uses version 2 for one image or
version 3 for a sequence. Its 592-byte board prefix selects workload 2. The extension binds the permanent owner, PCK client,
initial-isolation, memory, electrical and supervision records. The existing
full-session digest, private startup snapshot and board-prefix comparison remain
mandatory. Old version 1 sessions are refused.

In a version 2 session, the field named `fixture` contains the caller's JPEG
SHA-256. A zero `reference` digest selects ordinary decoding. A nonzero digest requires an uploaded
independent planar pixel oracle with that digest and the exact active output
length. Every fragment remains bound to the admitted epoch, generation,
authority, input hashes and deadline. Session inputs are immutable throughout
the lifetime.

Initial authority must exclude all ten IOMMU requesters and secure or remote
writers, establish drained prior transactions and page walks, keep both banks
disabled, own masked decoder IRQ81 and both IOMMU IRQs, and establish the decoder
OFF with external reset asserted. Skipped requesters remain quiescent for the
complete lifetime. Idle register observations do not establish these facts.
The mapping must reach the genuine immutable nonsecure ROM profile at
`0x17000..0x21fff`.

The board schematic v1.2 groups VDD-VE with VDD-SYS, VDD-DRAM and VDD-NPU on
DCDC2 at 0.8 V. The preserved schematic SHA-256 is
`4e0e92afbb1cca97a9c8593b8f2679178d9738f242ea431ed3650e893fe28ed0`,
PDF page 2, under
`~/.cache/sunstoneos/sources/board-documents/schematic-v1.2.pdf`.
The task acquires a shared DCDC2 lease and requires an enabled 800 mV PMIC
observation. It changes no voltage. Separate electrical authority must qualify
the selected VE frequency and inherited VE/IOMMU clocks.

The executor performs at most one register operation per poll, and decoding one
picture writes 659 registers. The task polls again while each poll advances the
executor's state or index, up to 1,024 polls per wake, and rechecks board
liveness between polls. It sleeps for one 1 ms tick only when a poll makes no
progress, such as while the engine is busy.

## One bounded upload publishes immutable parsed bytes

USB0 uses VID/PID `1f3a:efe8`, revision `0100`, product
`sunstoneos native JPEG` and serial `sunstoneos-a733-jpeg`. The vendor OUT
endpoint accepts a 32-byte upload header followed by the JPEG and optional
oracle. The header contains little-endian 32-bit magic `0x5547504a`, version 1,
JPEG length and oracle length, followed by 64-bit epoch and generation.

The JPEG capacity is 524,288 bytes (512 KiB) and the optional oracle capacity
is 460,800 bytes, the active output of a 640-by-480 image.
The native producer copies bounded fragments into private CPU arrays. It checks
hashes, the actual parser result, plane capacities and oracle extent before
publishing completion with a release store. The owner task acquires that store
before reading bytes or geometry. A fresh clock observation after parsing and
hashing rejects a commit that crosses its deadline. Invalid framing, mismatched
identity, backward time, late completion or another upload terminates the
lifetime. Published bytes are never modified while their image is being
decoded; in a sequence, the next image overwrites them only after the previous
result was delivered.

The parser requires baseline 8-bit YCbCr420, components 1/2/3, explicit tables
and one interleaved scan. It validates marker framing, tables and restart
structure. Entropy-coded coefficient validity and physical device access bounds
remain separate requirements. Unsupported input is refused before decoder
resource acquisition.

## A sequence session decodes up to eight images in order

In a version 3 session, `fixture` holds the SHA-256 of a sequence manifest and
`reference` must be zero. The manifest is a 16-byte header (little-endian
32-bit magic `0x5347504a`, version 1, image count from one to eight, zero) and
one 80-byte entry per image: 32-bit JPEG length, oracle length, width and
height, then the JPEG SHA-256 and the oracle SHA-256, zero without an oracle.
Each entry must fit the single-image capacities on its own.

The upload starts with the usual 32-byte header, but with version 2 and the
manifest length in place of the JPEG length; the oracle length must be zero.
The manifest follows and is checked against `fixture` before any image is
accepted. Each image then arrives as its own frame: 32-bit magic `0x4947504a`,
image index, JPEG length and oracle length, 64-bit epoch and generation, the
JPEG and the optional oracle. The index must be the next unpublished image, and
lengths, digests and parsed dimensions must match its entry.

Image k is accepted only after the USB task has completely transmitted a
success result for image k-1. Until then the decoder still owns that image's
input and output, and an early frame ends the lifetime. On publishing image k,
the USB task replaces the success status of image k-1 with pending; a failure
status is never replaced.

The decoder, IOMMU mappings and allocations are set up once, sized from the
largest input, luma and chroma entries of the manifest. After a verified copy
and a delivered result, the owner task rewrites the input buffer with the next
JPEG, zero-padded to its mapped end, rewrites both output planes with the poison
value, and starts the next executor job with `a733_video_jpeg_next`. That job
keeps the admitted identity, ownership token, mappings and preparation; it
parses the new input, rejects stale completion status and programs all 659
registers again. The H.264 streaming profiles refill their input buffers after
a healthy completion in the same way. Reuse depends on that healthy completion:
guards intact, no IOMMU fault and no codec error. It does not establish physical
transaction drain, and nothing is unmapped, released or reset. A failed image
leaves every resource retained and ends the sequence. The reviewed supervision
fact for a version 3 session is therefore `no_reuse_after_fault` instead of
`no_reuse`.

Results of a sequence use version 3 with a 160-byte header. It adds three
32-bit fields after the version 2 fields: the image index, the image count and
the number of published images. The digest fields carry the current image's
JPEG and oracle digests from the manifest. `apps/jpeg/prepare.py` builds the
manifest, the manifest upload and the image frames, and `apps/jpeg/observe.py`
decodes version 3 results. The supervised capture tool,
`tools/check-a733-jpeg-supervised.py`, still sends one version 1 upload and
reads one result, and the bundle format holds one image. A physical sequence
run needs both extended first.

## Parsed geometry determines guarded planes and the copied payload

Luma stride and padded rows round the parsed width and height up to multiples
of 32. Luma requires `stride * rows` bytes; contiguous Cb and Cr together require
half that count. The padded luma plane must fit 307,200 bytes, the size of a
640-by-480 plane, and each dimension must be within the decoder's 1920-by-1080
limit. Accepted shapes therefore include 640-by-480, 1920-by-160 and every
smaller image; 641-by-480, 640-by-481 and 1920-by-1080 are refused at upload.
The active payload contains `width * height` Y bytes and two chroma planes of
`ceil(width/2) * ceil(height/2)` bytes, at most 460,800 bytes.

After the upload commits, the application retains a 32 KiB aligned IOMMU table
and three Normal Non-cacheable allocations sized from that geometry. Each
allocation is a guard page, the buffer rounded up to whole 4 KiB pages, and a
second guard page. Only the rounded buffer is mapped: input read-only at IOVA
`0x10001000`, luma write-only at `0x10100000` and chroma write-only at
`0x10180000`. Unmapped IOVA pages separate the largest spans, and all three lie
in one 4 MiB translation-table window. The largest job uses about 1 MiB of the
2 MiB uncached pool. Input is zero-padded through its last mapped page; the
codec's inclusive input end is that page's final byte. A 64-by-64 image still
maps one page per buffer.

Full-HD decoding needs about 5 MiB of planes and input, which exceeds the
2 MiB uncached pool that `cmake/a733-jpeg.cmake` links into this image. It
requires an extended memory profile like the one the H.264 full-HD image uses.

The copy uses parsed dimensions, padded strides and the driver's padded Cb/Cr
plane offset. It includes the final partial JPEG minimum coded unit's (MCU's) active pixels, including
rounded chroma dimensions for odd widths or heights. Padded output regions may
change. The unused mapped tails and physical guard pages must retain their
poison values.

The retained-copy path checks IOMMU faults, codec errors and guards before
copying. It then copies active pixels into private CPU output in whole rows,
luma first, at most 64 KiB per call; the task sleeps one tick between calls, so
a 640-by-480 image takes eight calls. After the last row it rechecks guards,
optionally compares the independent oracle, scans faults again and rechecks
codec errors and guards.
Only that owned copy is published. COPIED does not establish transaction drain,
DMA ownership transfer or permission to reset or release device memory. Only a
sequence session rewrites the retained buffers, for its next image. Every
allocation, token and mapping remains retained after success or failure.
Physical confinement and pixel correctness remain unqualified.

## Copied results include their actual dimensions

After uploading, a zero-valued request byte retrieves a result. The version 2
result has a 144-byte little-endian header:

| Offset | Fields |
| --- | --- |
| 0 | 32-bit magic `0x4745504a`, version, status and payload length |
| 16 | 64-bit epoch and generation |
| 32 | Input, optional-oracle and copied-output SHA-256 digests |
| 128 | 32-bit width, height, padded luma stride and padded rows |

Status 0 means pending. Status 1 appends active planar Y/Cb/Cr pixels. Status 2
means retained failure and carries no pixels. The host checks geometry, length,
identity and digest; oracle mode also compares every active sample.
`apps/jpeg/observe.py` validates captures without opening a device.

`apps/jpeg/prepare.py` accepts `--jpeg` and optional `--reference`, together with
the native image, its compiled-input manifest, session template and reviewed
admission. It freezes those inputs, the ARM launcher and a session-bound
`upload.bin`. The execution profile is `jpeg-runtime-supervised-v2`.
`tools/check-a733-jpeg-supervised.py` uploads frozen bytes before querying,
assembles bounded response fragments and preserves copied output durably.
`apps/jpeg/manifest.py` uses the existing linked-object dependency collector.

## Offline evidence does not qualify physical operation

`checks/jpeg-sequence.py` covers sequence sessions. It builds four images of
64-by-48, 48-by-32 with an independent oracle, 640-by-480 with an oracle and
33-by-17 pixels. It feeds their uploads through the actual input code under
address and undefined-behavior sanitizers in fragments of 1, 17 and 512 bytes.
It also refuses each of these uploads: an image before the previous result was
delivered, an out-of-order image, changed image bytes, dimensions that differ
from the manifest, an image beyond the count, a sequence upload in a version 2
session, a version 3 session with a session oracle, a single upload in a version
3 session, a changed manifest and a nine-image manifest. The linked image then
executes USB ingestion, the version 3 serializer, delivery tracking, the owner
task's buffer refill and the executor's next-job entry for all four images,
with device pixels written by the model. It checks the refilled input and its
guard pages, the restored poison, the copied pixels and every result header.
Frames after the last image or before delivery stop the lifetime, a recorded
failure survives the next image, and an inconsistent job count fails.
`checks/jpeg-application-retained.c` runs a second job through the actual
executor and platform binding with modeled registers: no second admission,
mapping or preparation occurs, the register writes repeat those of the first
job, and changed spans or oversized files are refused. These results are under
`~/.cache/sunstoneos/build/audit-codec-streaming/checks/` and have not been
frozen into the evidence store.

Focused evidence for the earlier 4 KiB capacities is under
`~/.cache/sunstoneos/evidence/jpeg-runtime-input-20261018/`. The same checks
cover the current capacities: `checks/jpeg-runtime-input.py` adds 640-by-480
images with and without an oracle and a 1920-by-160 image, and
`checks/jpeg-runtime-input.c` refuses padded planes beyond 307,200 bytes.
`checks/jpeg-runtime-native.py` copies those images through the chunked native
path and serializes their results, `checks/jpeg-runtime-host.py` prepares and
captures the 640-by-480 oracle case, and `checks/jpeg-application-iommu.c`
builds and translates the largest spans. These results have not yet been
frozen into the evidence store.
Sanitizer checks use actual Python upload packets and the unchanged parser. They
cover fragmented input, ordinary and oracle modes, odd dimensions, maximum plane
capacity, identity and digest refusal, clock bounds and immutable publication.
Session sealing checks retain the existing full-session protections.

Native instruction checks execute actual USB ingestion, parser publication,
pre-input resource gating, dynamic copy and result serialization. They cover
partial MCU edges, optional oracle comparison, guards and retained IOMMU or
codec errors. Shared providers, scheduling, translations, device observations
and device-written pixels are modeled. Host checks freeze complete bundles and
ARM launchers, consume actual native result records and assemble fragmented
responses. Modeled authority files cannot establish physical entry facts.

The parser, executor, platform and IOMMU source behavior remains unchanged.
Earlier component evidence is preserved under
`jpeg-nonconstant-20261017/{central,parser,native,host}/` and
`jpeg-sealing-20261018/`. The registered 48-by-32 nonconstant diagnostic has an
independent FFmpeg reference; that evidence does not establish general JPEG
rounding conformance. No board, USB device, test queue or persistent storage was
accessed for runtime-input integration. Physical execution, copied-pixel
correctness, confinement and recovery remain outstanding.
