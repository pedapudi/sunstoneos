# GPU rendering to retained HDMI frames

The GPU/display application connects the existing trusted indexed-texture GPU
renderer to DE1, TV0 and HDMI at 1280×720, 60 Hz. Its native image and focused software composition checks pass. Physical GPU rendering and
HDMI output remain unqualified.

The native caller submits two caller-selected canonical texture inputs through
existing GPU service operations. Trusted graphics profile 2 supplies the resident
vertex and fragment programs. It uses the corrected geometry plus partial-render
kick followed by final fragment submission. The existing paired completion,
cache invalidation and guard checks precede output access.

The caller borrows each sequence-matched read-back slot, which holds one 64 KiB
chunk of the GPU output frame, converts its RGBA8
pixels to RGB565 and returns that exact ticket before borrowing another slot.
The conversion drops alpha and uses the upper five, six and five bits of red,
green and blue. It performs no scaling; the GPU already rendered the complete
720p surface. A separate display allocation receives the converted pixels. GPU
and display engines share no framebuffer mapping or imported ownership token.

Before each kick the worker fills the output frame with a poison value, 3.7 MB
at 720p, and cleans it. A tile that the GPU fails to write then reaches HDMI as
the poison colour instead of pixels from an earlier frame. This application
reads every frame back through READ slots, and the worker learns whether a
frame will be read or presented only after completion, so every frame keeps the
fill. The design in [designs/gpu-graphics.md](designs/gpu-graphics.md) removes
it for scanout: scanout slots are fixed at OPEN and skip the fill, and
read-back keeps it until a board run shows that every tile is written.

Both display frames and register command pages become immutable before display
translation installation and DE start. The application closes the GPU normally,
publishes the noncacheable display bank and then allows TV0 to fetch it. DE's
register command queue selects slots 0→1→0. Each reported presentation requires
fresh queue completion, the selected source-address readback and a subsequent
DE frame-end. These observations do not establish visible pixels, transaction
drain or permission to recycle either frame.

One counter, clock registry, register/I2C lock set, pin and rail owner, PMIC task
and global GPU/PCK manager serve both engines. GPU resource client 7 and the
HDMI session's distinct client share stable oscillator/PCK leases. GPU retains
its exclusive local control; HDMI owns VO1, and the display owner separately
reserves DE and complementary system-IOMMU scopes. GPU uses its existing private
MMU. System-IOMMU master 8 receives only the display read-only bank; GPU is absent
from that system-IOMMU master list.

The application uses the existing extended write-back and noncacheable memory
pools. Its display owner reserves one complete 4 MiB noncacheable parent after
the GPU reservations. The parent contains 32 KiB of IOMMU tables, a leading
4 KiB guard, two 1,847,296-byte padded RGB565 frames and two 4 KiB command pages.
Those regions require 3,739,648 bytes. The active RGB565 pitch is 2560 bytes and
each frame contains 1,843,200 pixel bytes. No full framebuffer occupies BSS.
The actual linked constructors and VM planners accepted the complete allocations
and mapping capacities. Write-back parents occupy 23,382 pages; noncacheable GPU
parents occupy 72 pages, followed by the aligned 1,024-page display parent.
The display parent starts at physical 0x78048000 in the admitted extended pool.

The root-owned authenticated envelope includes GPU/HDMI initial-state, memory,
electrical and supervision authority plus the exact extended-memory reservation identity and application data. Its complete upload
at 0x41f00000 must fit below 0x42000000 and survive BSS clearing before provider
construction. The application data header is 144 bytes: magic, version, total
length and two 64-byte records. Each record contains the existing five-word
texture shape, payload offset and length, a zero reserved word and SHA-256.
Payloads follow in frame order without gaps. Their combined capacity is the
space left by the containing 1 MiB envelope; the native renderer's larger
per-frame maxima remain separate capabilities.

`apps/gpu-display/host.py` packs two existing `texture.input_bytes()` results.
The root preparation layer supplies its remaining envelope capacity. The native
caller revalidates shape, canonical extents, indexed geometry and digests before
copying input through service-owned chunks. Digests identify immutable bytes;
they confer no GPU, display, memory or physical authority.

All providers and operations share the original absolute 20-second lifetime.
GPU budgets are capped by the remaining interval before the final two-second
HDMI stop reserve. No request starts a new window. HDMI health monitoring
continues while a replay is pending; IOMMU fault scans are serialized with
replay admission and polling. Healthy shutdown stops HDMI/TV0, disables DE and
requests the PMIC owner's idle/park acknowledgment before publishing completion.
Every allocation, mapping and retained provider identity remains owned. Any
uncertainty retains resources and prohibits rollback, reset or reuse.

The fixed TV0 replay extension follows the pinned display driver's output-ID
RCQ path in `de_top.c:673–683,730–744,1096–1161`. It changes only the UCH3 source
address and preserves RGB565 format, pitch, size and TV0 routing. The existing
native submit helper keeps the busy observation and update write within one
bounded critical section under the shared register mutex. LCD1 replay behavior
remains the same. Astra reviewed shared-provider identity, copied-slot lifetime,
immutable bank publication, replay deadlines, continuous HDMI health checks and
PMIC park ordering before execution.

Physical memory ownership and initial all-master IOMMU drain/exclusion, qualified
HDMI analog inputs, an attached monitor/cable, GPU firmware execution and actual
pixels remain external prerequisites. Broader HDMI modes, other display formats,
arbitrary new-frame streaming, frame retirement, graphics shader modes and
performance work remain required beyond this bounded integration.

The preserved evidence is under
`~/.cache/sunstoneos/evidence/gpu-display-20261020/`. The final image SHA-256 is
`ae7c6ee15b117f214ea1975b39d4d1f8f27ba007632f7efa3ed2c137460d717c`.
Its exact compiled closure records 95 objects and 214 dependency inputs.
Root checked full-envelope authentication, retained publication, memory-authority
binding and quiet SPI95 routing without executing CPU startup.

Application checks execute the actual shared-provider constructors, extended
allocators, graphics planners, two-input native caller, copied-output accessor,
RGB565 conversion, IOMMU installation and TV0/DE register paths. They check
sequence-matched release, valid CPU versus DEVICE ownership, immutable bank
publication, slot observations 1→2→1, healthy DE stop and the actual PMIC owner’s
park acknowledgment. A submitted replay that reaches its deadline retains the
bank and refuses subsequent MMIO.

Those application results execute the preserved first image with SHA-256
`a794a6d4ebdd05563bab6174ad55fde5824d1c5b296e9580fa6737ddd1a4bdbb`.
The final image changes only admission, boot, snapshot and build-identity objects;
the other 91 objects are identical. Root separately checked the changed memory
binding. Application results are reused through that object comparison.

GPU service completion and pixel values, HDMI preparation/health/stop, PMIC
readiness, register effects, CPU admission/cache/translation boundaries and
FreeRTOS scheduling are modeled in these checks. The PMIC park task, display
mapping checks and native RCQ helper execute. This evidence does not establish
concurrent live GPU/HDMI lease operation, interrupt exclusion on silicon, GPU
shader execution or visible pixels. The offline fixture’s synthetic GPU proof
bytes confer no physical admission authority.

Host preparation and the generated SRAM wrapper are described in
[the preparation guide](gpu-display-preparation.md). Root verifies the unchanged
native image and accepted host/SRAM evidence under
`gpu-display-host-20261020/root-integration.json`; six actual AArch32 launcher
cases stop before reset with CPU state and MMIO modeled. Ordinary production
validation refuses the positive modeled bundle. The preserved finite image
predates the USB observation path below; no physical launch or output is established.


## Copied USB observations

The supervised image adds USB0 PIO observation to the retained two-frame path.
Its version 2 authenticated envelope inserts a 136-byte USB authority at byte
704 and moves the application inputs to byte 840. The authority binds a distinct
USB owner and handoff, three evidence digests, the admitted ROM-device starting
state and four supply voltages. The complete envelope still fits in 1 MiB.

USB borrows the existing counter, clock registry, rail leases, register/I2C locks
and PMIC task. It creates no second native USB provider. Four PMIC snapshots are
acquired and reaped before USB readiness permits GPU submission. Once USB is
ready, its health checks observe provider metadata without creating another
PMIC request. GPU completion and HDMI stop precede the existing PMIC park
acknowledgment. USB continues polling retained provider metadata afterward.

A 32-byte request supplies magic, protocol version, consecutive request ID,
length, epoch and generation. Admission checks the exact identity before posting
the single mailbox. The display task copies a 1,024-byte response containing two
frame records, three presentation records and terminal state. USB reads only
that immutable response; it never reads mutable GPU or display internals.

Each frame record contains the canonical input digest, hashes of copied RGBA8
and converted RGB565 bytes, and sixteen pixel samples. Samples cover four fixed
columns and four fixed rows, including the image corners. Presentation records
bind slot 0→1→0 to completed register commands, source-address readback and a
subsequent frame-end observation. The response also binds the lease, native
image identity and authenticated full-envelope digest. Hashes and samples are
selected evidence; they do not establish full pixel correctness or visible HDMI
pixels.

The mailbox remains owned until every response byte and the terminating USB
zero-length packet complete. The producer waits for capacity for all 1,024 bytes
and writes them once. An unexpected short FIFO write faults and retains the
response. The pinned TinyUSB stream writer can submit when its FIFO reaches an
endpoint packet, and its vendor completion callback runs before automatic
zero-length packet selection (`src/tusb.c:407–417` and
`src/class/vendor/vendor_device.c:916–921`). These semantics require retaining
the response through the later zero-length completion. Both 64-byte full-speed
and 512-byte high-speed endpoints divide the response exactly.

The caller in `apps/gpu-display/capture.py` reads up to 1,025 bytes so the final
zero-length packet terminates the transfer. It requires exactly 1,024 data bytes,
validates response identity and terminal proof, and offers each raw response to
a persistence callback before decoding it. At most 200 exchanges transfer
204,800 response bytes plus 6,400 request bytes. Every transfer uses the original
19-second host deadline; GPU/display share the original 20-second session and
HDMI stop starts at 18 seconds. No observation extends those deadlines.

After healthy terminal publication, an idle USB task parks at session expiry.
An incomplete request, posted snapshot, response or final zero-length packet at
expiry latches failure and retains ownership. Display continues servicing copied
observations after terminal publication while USB remains live. Uncertainty
never authorizes a reset, memory disposal or another frame.

The system-IOMMU access mask remains 0x100 for display master 8. It authorizes
the micro-register subset this application may inspect, rather than listing all
powered devices. The pinned A733 device tree assigns USB master 0 separately
and places DE, DI, G2D and EINK in the shared DE_SYS domain
(`sun60iw2p1.dtsi:448–451,494–515`). USB uses fixed PIO and performs no DMA;
all-master DMA exclusion and initial drain remain independent requirements.
Unadmitted micro-register sets are skipped. Unexpected global IOMMU faults
retain the application; this path does not claim to scan USB micro-fault state.

The observation evidence is under
`~/.cache/sunstoneos/evidence/gpu-display-capture-20261020/`.
The final image SHA-256 is
`700413937babe0471ee7a9022c0df09c20079f1280ecf7a86f47dc827a9710d3`.
Its compiled closure contains 104 objects and 248 dependency inputs. Actual
shared-provider construction, USB platform MMIO, copied-output hashing and
sampling, presentation timestamps and terminal park execute in the preserved
initial observation image. The final image changes USB framing and image
identity only; the other 102 raw objects, allocated sections and relocation
bytes match. Those unchanged component results are reused explicitly.

Focused final-image checks execute the parser, copied snapshot and corrected
USB framing through full-speed and high-speed final-packet completion. They
reuse only writable terminal state from the hashed component checkpoint, retain
final code and read-only data, and bind the final session identity. They also
cover identity refusal, mailbox immutability, request limits, expiry races and
retained failures. TinyUSB transfer effects, provider readiness in these narrow
framing cases, counter progression and scheduling are modeled. Earlier partial
FIFO-write concatenation is preserved as component evidence and supplies no
USB packet-framing proof. Root admission/retention and quiet-IRQ checks remain
separate evidence. Physical GPU, USB and HDMI operation remain unqualified.
