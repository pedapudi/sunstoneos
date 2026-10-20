# GPU rendering to retained HDMI frames

The GPU/display application connects the existing trusted indexed-texture GPU
renderer to DE1, TV0 and HDMI at 1280×720, 60 Hz. Its source is implemented;
linked and native composition checks are pending. Physical GPU rendering and
HDMI output remain unqualified.

The native caller submits two caller-selected canonical texture inputs through
existing GPU service operations. Trusted graphics profile 2 supplies the resident
vertex and fragment programs. It uses the corrected geometry plus partial-render
kick followed by final fragment submission. The existing paired completion,
cache acquisition, guard checks and copied snapshot precede output access.

The caller borrows each sequence-matched copied output slot, converts its RGBA8
pixels to RGB565 and returns that exact ticket before borrowing another slot.
The conversion drops alpha and uses the upper five, six and five bits of red,
green and blue. It performs no scaling; the GPU already rendered the complete
720p surface. A separate display allocation receives the converted pixels. GPU
and display engines share no framebuffer mapping or imported ownership token.

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
The actual constructors and VM planners must validate the final allocation and
mapping capacities; the size calculation does not establish admission.

The root-owned authenticated envelope includes GPU/HDMI initial-state, memory,
electrical and supervision authority plus application data. Its complete upload
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
