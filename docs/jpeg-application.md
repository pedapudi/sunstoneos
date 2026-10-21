# Native JPEG application accepts one authenticated runtime image

The application accepts one caller-supplied baseline YCbCr420 JPEG per admitted
session. It authenticates and parses an owned CPU copy before acquiring any JPEG
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

The session retains its 856-byte layout and uses version 2. Its 592-byte board
prefix selects workload 2. The extension binds the permanent owner, PCK client,
initial-isolation, memory, electrical and supervision records. The existing
full-session digest, private startup snapshot and board-prefix comparison remain
mandatory. Old version 1 sessions are refused.

The field named `fixture` contains the caller's JPEG SHA-256. A zero `reference`
digest selects ordinary decoding. A nonzero digest requires an uploaded
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

The JPEG capacity is 4,096 bytes and the optional oracle capacity is 6,144 bytes.
The native producer copies bounded fragments into private CPU arrays. It checks
hashes, the actual parser result, plane capacities and oracle extent before
publishing completion with a release store. The owner task acquires that store
before reading bytes or geometry. A fresh clock observation after parsing and
hashing rejects a commit that crosses its deadline. Invalid framing, mismatched
identity, backward time, late completion or another upload terminates the
lifetime. Published bytes are never modified or reused.

The parser requires baseline 8-bit YCbCr420, components 1/2/3, explicit tables
and one interleaved scan. It validates marker framing, tables and restart
structure. Entropy-coded coefficient validity and physical device access bounds
remain separate requirements. Unsupported input is refused before decoder
resource acquisition.

## Parsed geometry determines guarded planes and the copied payload

The application retains a 32 KiB aligned IOMMU table and three guarded 12 KiB
Normal Non-cacheable allocations. Only the central 4 KiB page of each allocation
is mapped: input read-only, luma and chroma write-only. Physical guard pages and
unmapped neighboring IOVA pages remain reserved. Input is zero-padded through
its mapped page; the codec's inclusive input end is that page's final byte.

Luma stride and padded rows round the parsed width and height up to multiples
of 32. Luma requires `stride * rows` bytes; contiguous Cb and Cr together require
half that count. Each plane allocation must fit its 4 KiB page. The active
payload contains `width * height` Y bytes and two chroma planes of
`ceil(width/2) * ceil(height/2)` bytes. The active payload must fit 6,144 bytes.
The arithmetic first bounds each dimension to 128. Accepted shapes include
64-by-64 and 128-by-32; 65-by-65 exceeds the padded-plane capacity.

The copy uses parsed dimensions, padded strides and the driver's padded Cb/Cr
plane offset. It includes the final partial JPEG minimum coded unit's (MCU's) active pixels, including
rounded chroma dimensions for odd widths or heights. Padded output regions may
change. The unused mapped tails and physical guard pages must retain their
poison values.

The retained-copy path checks IOMMU faults, codec errors and guards before
copying. It then copies active pixels into private CPU output, optionally compares
the independent oracle, scans faults again and rechecks codec errors and guards.
Only that owned copy is published. COPIED does not establish transaction drain,
DMA ownership transfer or permission to reset or reuse device memory. Every
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

Focused evidence is under
`~/.cache/sunstoneos/evidence/jpeg-runtime-input-20261018/`.
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
