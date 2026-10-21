# Native offscreen rendering

The optional indexed-texture application accepts caller-supplied vertices,
uint16 indices and RGBA8 texels, then streams a copied framebuffer up to
1920 × 1080 pixels. It retains the graphics context between frames. The
triangle-color application below keeps its separate 64 × 64 profile. Physical
GPU execution remains unqualified.

## Indexed textured frames

Build `apps/gpu-render` with `SUNSTONE_GPU_RUNTIME_TEXTURE=ON`,
`SUNSTONE_GPU_SUPERVISED=ON` and `SUNSTONE_GPU_SINGLE_CORE_APPLICATION=ON`.
Select `sunstone-a733-gpu-texture-single-core`. A GPU-only session envelope
appends five uint64 values to the 592-byte board prefix: magic
`0x4153373347545831`, version 1, envelope length 632, framebuffer width and
height. The coordinator authenticates all 632 bytes before launch. The retained
copy has a link-time size and placement assertion outside the zero-initialized BSS region.
Framebuffer dimensions remain fixed for the session and its firmware render
target records. The executable bytes do not change with selected input or
session dimensions.

Profile 2 uses resident trusted vertex and fragment shaders. Each vertex has
float4 clip position followed by float2 normalized texture coordinates, with a
24-byte stride. Positions have finite x/y in [-1, 1], z equal to zero and w equal
to one; texture coordinates have finite components in [0, 1]. The vertex shader
passes position and texture coordinates; the fragment shader uses explicit
level-zero sampling and packs the result into RGBA8. Texture coordinates use
nonperspective interpolation. The sampler uses nearest filtering and
clamp-to-edge addressing, with one mip level, layer and sample.

The vertex allocation covers all 65,536 uint16 index values. The 128 KiB index
allocation admits up to 65,535 indices in complete triangle lists. Every index
must name a supplied vertex; restart, instancing and base vertex are disabled.
Textures and framebuffers each admit widths 1 through 1920 and heights 1 through
1080. Linear row strides round width up to two pixels. Returned rows include
this padding; consumers use the reported width and stride. These are initial
allocation policies, not hardware limits.

`pvr_gpu_service_texture()` copies one scoped request and up to 64 KiB before
enqueue. BEGIN binds a private draft to the session owner, previous frame,
generation, texture shape and geometry counts. WRITE appends copied chunks at
the next contiguous byte offset. The canonical input contains vertex bytes,
index bytes and padded texture bytes in that order. A replacement BEGIN starts
a new generation. Generation and ticket wrap are refused.

SUBMIT requires the complete draft. The privileged worker validates its shape,
all vertices and all indices, then checks native ownership, the original paired
queues, the previous frame and both acquired completion timelines. Only accepted
submission revokes prior copied output. It initializes and publishes active
input, descriptors, draw count and guarded output before the combined kick.
Input and commands remain immutable through completion. Invalid drafts preserve
the prior frame; uncertain submission retains every allocation and context.

The existing graphics owner leases these allocations from the admitted write-back (WB) pool.
Texture and geometry mappings are GPU-read-only; guarded output is writable.
Upload drafts, copied output and transfer slots occupy CPU-only portions of the
lease, absent from GPU mappings. At maximum geometry, the complete graphics VM
needs 19 table pages: one root, three directories and fifteen leaf tables.
Construction still derives and checks the complete actual mapping plan.
Extended-memory ownership remains an external launch prerequisite; selecting
this image does not establish physical DDR or GPU authority.

Preparation and cache publication advance in batches of at most sixteen pages.
Both acquired timelines precede output invalidation, guard checks and a complete
CPU snapshot. Invalidation also advances by at most sixteen pages per step;
snapshot copies advance by at most 64 KiB. Texture OPEN has a 15-second budget
because initial free-list setup has separate allocate, zero and publish steps
for 1,024 global pages. Submission has a five-second budget. Upload drafts and
frame streams each have a 30-second limit; partial requests and client transfer
waits have one-second limits. The inherited 60-second session deadline
bounds all operations. Timeout retains uncertain device ownership.

READ copies a bounded snapshot chunk into one scoped download slot.
`pvr_gpu_service_texture_output()` lends that copy to the requesting client
after its reply is collected. RELEASE must match its ticket, client, frame and
offset. Replacement and CLOSE refuse a borrowed slot. The application releases
each slot only after the final TinyUSB transfer callback, including automatic
zero-length packets used to terminate packet-aligned transfers.

The private version-4 USB protocol has 64-byte requests and response headers.
It carries epoch, frame, generation, byte offset and input dimensions. One
READ_FRAME operation returns a header followed by the complete framebuffer
through 64 KiB copied slots; a 1080p result requires 127 such slots. The host
requests 65 bytes for each 64-byte header so full-speed headers, including final
CLOSE, consume their terminating zero-length packet. The existing version-3
triangle protocol remains unchanged.

`apps/gpu-render/texture.py` provides session-envelope construction,
`input_bytes()`, `frames()` and `capture_frames()`. Capture consumes the
coordinator's identity-validated, locked, already running USB handle. It does
not discover, open, launch or reset a device. Host input and output selection
require no firmware rebuild. CLOSE uses the existing normal GPU shutdown.

## Depth testing and source-over blending

The same retained texture application supports graphics profile 3. Pass
`profile=3` to `texture.session()`, `input_bytes()` and `capture_frames()`.
The session envelope remains 632 bytes and uses version 2. Its authenticated
geometry selects the depth allocation before OPEN. The runtime protocol uses
version 5 with the same 64-byte header. Version-1 sessions retain graphics
profile 2 and version-4 requests.

The last request word carries render state on BEGIN. Bits 0–1 select depth
comparison: 0 disables depth testing, 1 selects LESS, 2 selects LESS_OR_EQUAL,
and 3 selects ALWAYS. Bit 2 enables depth writes; it requires enabled depth
testing. Bit 3 selects straight-alpha source-over blending. Other bits and
state on other operations are refused. State applies to every triangle in the
submitted indexed list. Each accepted frame clears color and depth.

For example, state 5 draws with LESS and depth writes. State 10 draws with
LESS_OR_EQUAL, depth writes disabled and source-over blending. The blend
computes RGB as `src.rgb * src.a + dst.rgb * (1 - src.a)` and alpha as
`src.a + dst.a * (1 - src.a)`. Profile-3 vertex positions admit finite z in
[0, 1], with w equal to one. The existing position/UV layout, texture bounds,
index limits and framebuffer dimensions apply. State is captured with the
copied draft and revalidated before input replacement or output revocation.

A single D32_SFLOAT depth attachment uses twiddled F32Z storage. Its physical
width and height round up to powers of two, with a minimum of 32 pixels per
axis from the pinned GPU's depth load/store tile geometry. The allocation
reserves 16 MiB for depth plus two unmapped guard pages. At 1080p, the physical
extent is 2048 × 2048 pixels. The GPU mapping at `0x800d000000` is writable;
the CPU initializes and publishes it in bounded page batches before each frame.
Hardware clears logical depth to one and stores it without an initial depth
load. Depth contents do not persist between frames or appear in copied output.

The additional depth mapping increases the native maximum to sixteen mappings
and 27 table pages. Actual construction and admission derive the entire VM
plan, including the eight additional leaf tables. The existing graphics lease
retains depth, shaders, input, commands and output through both final timelines
and every uncertain failure. Completion precedes depth-guard acquisition and
framebuffer copying. Existing output-slot ownership and healthy CLOSE apply.

The resident source-over fragment shader contains 344 instruction bytes plus
32 prefetch bytes. It uses 32 temporaries, eight coefficients and 27 shared
registers, matching the existing replacement shader's resource allocation.
It occupies USC offset 3072; replacement remains at offset 2560. Its separate
36-byte PDS kick program occupies PDS offset 2560. PPP data selects between
the resident programs after previous completion. No USC code is rewritten.
The compiler reports framebuffer fetch, no phase change and enabled output-lock
checks. The translucent pass and disabled pixel merging preserve source-order
blending. Both variants retain zero scratch, spills, barriers and atomics.

Mesa's `pvr_arch_pipeline.c:2247–2288` supplies the actual blend initializer;
`pco_nir.c:1061` lowers blending and `pco_trans_nir.c:904–938` reads the previous
on-chip pixel with output-lock checks. `pvr_arch_job_render.c:958–1025` defines
depth load/store state. `pvr_arch_cmd_buffer.c:1080–1111,1767,1878–1908` supports
CLEAR backgrounds, final-color storage for partial renders, and depth
CLEAR/STORE without an initial load. The single stored attachment has no resolve
or separate color scratch requirement.

Focused evidence is retained in
`~/.cache/sunstoneos/evidence/gpu-depth-blend-20261019/`. The source comparison
checks resident shader/PDS bytes, every admitted state, firmware offsets and
padded depth geometry with sanitizers. Native service cases cover two changing
1080p frames, legacy-profile reuse, invalid-state refusal, depth guards and an
uncertain final kick. GPU writes and firmware effects remain explicit models.
These checks do not establish physical depth testing or blending.

## Geometry, partial renders and final completion

The shared render worker follows pinned Mesa's geometry/partial-render/final
sequence. It first publishes a combined geometry and partial-fragment kick.
Geometry carries its cleanup reference; the partial-fragment kick has none.
A separate final-fragment kick carries one fragment cleanup reference. The
partial render uses the same depth state and final color PBE/EOT programs for
this single stored attachment. Normal and partial background programs both
implement CLEAR, following the upstream load operation.

For API frame n, geometry updates its timeline to n. Partial fragment updates
to 2n−1 and final fragment updates to 2n. Only the final values admit output
or replacement. The pair retains both rings through the final kick and checks
their exact serials before reuse. An uncertain final kick faults both rings.
This follows `pvr_drm_job_render.c:488–604`, pinned Linux `pvr_queue.c:699–705`
and the cleanup rule in `pvr_cccb.c:257`.

Earlier frozen rendering evidence used one combined final-fragment kick. It
remains historical software evidence; the corrected native legacy-profile case
covers the changed sequencing. Compute queues retain their separate behavior.

## Compiler contract and evidence

The producer uses Mesa revision
`86c7176d2dd0065555521bf8c61f208bf53c75d6`. It reuses the actual PCO allocators,
descriptor allocation and vertex-primary programmable data sequencer (PDS)
generator. The combined descriptor
is 80 bytes, including image metadata, sampler metadata and gather words.
Fragment metadata occupies shared register 0; descriptor registers occupy
[4, 24). Mesa also reserves spill-address registers even when no spill occurs.
Those reservations do not represent scratch use.

The compiled vertex shader has 72 instruction bytes, six inputs, six outputs,
three shared-register reservations and zero temporaries. The fragment shader
has 200 instruction bytes, 32 temporaries, eight coefficient registers and 27
shared registers. Both include 32 bytes of unified shader core (USC) prefetch padding. Neither uses
scratch, spilled temporaries, barriers or atomics. Native construction preserves
the emitted bytes. Resource units in native state come from this metadata. These are
trusted resident programs; hashes, allocation bounds and output guards do not
independently verify arbitrary executable shader behavior.

Pinned source anchors include `pvr_common.h:198–225` for descriptor layout,
`pvr_arch_tex_state.c:139–255` for linear image state,
`pvr_arch_sampler.c:127–230` for sampler state,
`pvr_arch_pipeline.c:2470–2670` for descriptor allocation,
`pvr_arch_cmd_buffer.c:9084–9180` for indexed vertex data master (VDM) commands and
`pvr_arch_job_common.c:173–175` for pixel backend (PBE) row stride. The default pinned Linux/FW
job-coherency contract remains required in both directions; CPU cache cleaning
alone does not invalidate GPU caches.

Offline evidence is preserved under
`~/.cache/sunstoneos/evidence/gpu-textured-render-20261019/`. It separates pinned
compiler/reference comparison, actual native FreeRTOS service checks and the
linked application/host checks through TinyUSB's FIFO completion path. The
service checks model only unchanged architectural admission and GPU/firmware
effects. The application checks model service replies, GPU results and USB
endpoint hardware. These checks establish no physical rasterization claim.

Arbitrary shaders, perspective rendering, stencil, general blend equations, other
topologies/index modes, mipmaps, other texture formats/filtering, arrays/cubes,
multisampling, display presentation, physical context-disposal qualification and
allocation reclamation remain required work.

## Healthy CLOSE cleans firmware objects before stopping

The native graphics owner keeps its allocations throughout CLOSE. The service
first closes submission admission and refuses staged input, an incomplete render,
a texture draft or borrowed output. A completed render requires its exact paired
queue identities, serials, acquired geometry and final-fragment timelines, and
completed framebuffer copy. An unused context requires zero timelines and fresh
client rings; the existing formatter initializes both rings without a job.

The service drains already-owned free-list responses until the manager is idle,
no request remains, and control polling reports no queued events. It then retires
grow and reconstruction dispatch. Any later grow or reconstruction event faults
before reaching the resource handler, including events serviced synchronously
inside shutdown polling. This prevents reconstruction from rewriting disposed
render-target records.

Cleanup uses the native owner's admitted firmware addresses in this order:

1. Fragment common context, then geometry common context, when rendering exists.
2. The supporting compute context, including an unused compute context.
3. Render-target data record 1, then record 0.
4. Local free list, then global free list.

Resource-only graphics owners have no render context pair; they still own both
render-target data records, both free lists and the supporting compute context.
The common render-target record has no independent cleanup command. Static depth
storage is mapped memory rather than an on-demand firmware ZS-buffer object.

The pinned Linux source defines fragment-before-geometry destruction in
`pvr_context.c:168–180`, reverse render-target cleanup before free-list reference
release in `pvr_hwrt.c:501–518`, and local/global reference order in
`pvr_hwrt.c:78–85`. Native cleanup uses `pvr_fw_structure_cleanup`'s KCCB command
106 with flags 0 and types 0, 1 and 2. Each acquired receipt must match its epoch,
serial and slot, and return status 1. The service retains every receipt and
completed step if a later cleanup fails. BUSY, poll failure, missing acknowledgment
or an expired deadline prevents healthy shutdown; no retry occurs.

Only complete cleanup enters the existing firmware power and isolation protocol.
One absolute CLOSE deadline covers draining, cleanup and shutdown. The service
retains mappings and allocations by default. The optional standalone lifecycle
profile below can remove child mappings, release their parent allocations and
reconstruct the session. Physical cleanup and memory draining remain unqualified.

Eight focused cases exercise the actual native reserved owners, allocator,
mappings, admission callbacks, service and queue transport. They cover completed
clear with copied output, unused rendering, resource-only graphics, partial BUSY,
wrong-slot completion, reconstruction during cleanup, reconstruction during the
first shutdown power command, and staged or borrowed output refusal. All pass in
`~/.cache/sunstoneos/evidence/gpu-render-cleanup-20261020/`. Firmware returns,
GPU pixels, register responses, mode observation and isolation are modeled.
The separate native application build establishes compilation and linking only.

## Standalone sessions can dispose and reconstruct child resources

The supervised single-core GPU application accepts `SUNSTONE_GPU_LIFECYCLE=ON`
with its existing runtime-input, triangle or texture configuration. The selected
texture target remains `sunstone-a733-gpu-texture-single-core`; its manifest
workload becomes `copied-indexed-texture-draw-lifecycle`. Default builds retain
CLOSE ownership and their existing wire versions.

After healthy CLOSE, explicit DISPOSE binds the exact closed client and queue
epoch. The native backend checks the completed firmware shutdown, retired
queues, reset/isolation acknowledgment, platform OFF and zero resource token.
This uses the [accepted normal shutdown contract](gpu-isolation.md); physical
visibility and shutdown remain unqualified. Power OFF alone is insufficient.

The sole GPU worker invalidates complete WB child extents, removes child MIPS
translations, clears GPU VM tables and context records, then releases three
compute parent leases and four graphics parent leases. Global firmware parents,
the backend and shared power/clock/PMIC/USB providers remain retained. Successful
cleanup receipts and each completed release remain recorded after later failure.
A partial reconstruction preserves new allocations as DEVICE-owned or
constructor-RETAINED; the poisoned service prevents retry.

OPEN after successful DISPOSE reconstructs child descriptors and allocations
with fresh lease generations. It initializes new queues, timelines and event
references, and publishes exactly the next queue epoch. Client ticket counters
survive. The complete authenticated compute package stays outside disposed
pools. Graphics reconstruction uses retained profile metadata; caller texture
and geometry drafts require a fresh upload after OPEN.

This profile admits only the source-audited standalone image. That image names
its static disposal owner in the service configuration (`disposal`); every other
image leaves the field empty, so DISPOSE and reopening are refused. The platform
returns the owner only to the actual GPU worker while the owner names a backend
bound to this service and its firmware memory, a nonzero session limit, and one
registered GPU client. Static task allocation, disabled timers and an exact pool
ledger containing only the GPU parents are also required. Mixed-device runtime
pool reclamation still needs shared serialization.

All queued mutations and copied outputs bind their queue epoch at service
consumption. Optional wire versions are compute 6, triangle 7, texture 8 and
depth/blend 9; legacy versions 2 through 5 retain their layouts. Render OPEN
publishes frame/generation zero and requires fresh submission before READ.
Compute OPEN executes the original sealed input and publishes sequence one.
Texture replies use a separate 72-byte header, followed by the existing copied
stream and terminal-ZLP ownership rules.

DISPOSE and reconstruction each allow up to 15 seconds, capped by the original
absolute session deadline. CLOSE retains its one-second allowance. Disposal
handles at most 16 WB pages per worker step, and the worker runs up to 16
disposal steps per tick of the configured 1 kHz rate. The admitted maximum depth
layout needs 1,620 steps, about 102 ticks. Neither an expired deadline nor
uncertain completion authorizes reuse.

Focused lifecycle evidence is retained in
`~/.cache/sunstoneos/evidence/gpu-resource-disposal-20261020/`. Native child
allocation/disposal/reconstruction and application protocol checks have separate
model boundaries. Separate linked consumers cover texture/depth and compute. The compute-only
[three-parent lifecycle check](gpu-compute-service.md#compute-only-disposal-and-reopening)
also exercises copied output across reopening. With the owner supplied through
the service configuration and bursts of worker steps, both native owner checks
passed again on rebuilt texture and compute lifecycle images; their models stop
publication at the same power admission boundary between burst steps. The
application protocol checks were not repeated. Triangle lifecycle runtime remains
outside these checks. These checks do not establish physical cache visibility,
firmware execution, rasterized pixels, SMP scheduling or mixed-device disposal.

## Copied triangle submission

The optional triangle application accepts a copied, caller-selected triangle
list through `pvr_gpu_service_draw()`. Real vertex and fragment shaders, vertex
fetch programs, primitive state and draw commands feed the existing paired
geometry and fragment queues. The application returns a copied 64 × 64 RGBA8
framebuffer. Physical GPU rasterization remains unqualified.

The draw profile admits 3 through 126 vertices, in groups of three. Each
`pvr_render_vertex` contains four binary32 clip-position components followed by
four color components, with a 32-byte stride. Positions have finite x/y in
[-1, 1], z equal to zero and w equal to one. Colors have finite components in
[0, 1]. A dedicated read-only GPU page holds at most 4,032 vertex bytes. The
126-vertex limit follows from one 4 KiB page rounded down to complete triangles;
it is an initial allocation policy, not a hardware limit.

The resident trusted vertex shader passes position and color to the rasterizer.
The fragment shader interpolates color without perspective correction and packs
it into RGBA8. Viewport and scissor cover the framebuffer. Uncovered pixels use
the existing background color `0xffc08020`. Degenerate triangles may produce
only background. This profile has no indices, instancing, depth/stencil,
blending, textures or multisampling. Broader shader and graphics support remains
required work.

`pvr_gpu_service_draw_next(service, epoch, previous_frame, vertices, bytes,
count, timeout, wait, ticket)` copies the complete input before enqueue.
The epoch comes from the
caller's OPEN reply. One scoped staging slot binds the copy to its client and
ticket; queue refusal and consumed rejection release that slot. The privileged
worker checks the current owner, epoch, native mappings and READY state again
before accepting the first draw. Further draws require the matching completed
previous frame. Invalid input does not change device backing or revoke prior
copied output. The original `pvr_gpu_service_draw()` submits only frame one.

The graphics owner constructs and publishes all program, interpolation,
fragment-metadata, scissor and primitive state before OPEN. Runtime submission
copies the vertices into its private page and updates the draw count. It
poisons the output and restores its guards for each frame. It publishes every
changed page through its EL1 mapping before the combined kick. Input and
commands remain immutable for the submitted lifetime. Both
acquired completion timelines precede output invalidation, guard checks and
pixel copies. Guard checks protect the copied extent; they do not independently
prove rasterization. Uncertain completion retains all owners.

Frames use increasing nonzero sequence numbers. Before overwriting input or
output, the worker acquires both previous timelines and requires complete
output copying. Accepting the next frame revokes service reads of the previous
frame. `pvr_gpu_service_render_read_sequence()` returns 64 copied pixels only
for the requested current frame after completion; stale reads return no pixels.
Already returned copies belong to their callers. The legacy untagged read
remains available. CLOSE follows the final completed frame and normal shutdown.

The same geometry and fragment contexts retain their two 2 KiB command rings,
read/write cursors and monotonic serials. They are formatted only before the
first submission. A new paired submission requires the original accepted pair
and both successful IDLE rings. Frame n sets geometry completion and the
partial-fragment dependency to n, and its geometry partial-render reference to
n−1. Its partial and final fragment updates are 2n−1 and 2n.
The two existing render-target data records alternate between frames. Both job
pointers and both cleanup references select the same record. CPU code preserves
firmware-owned render-target state and cleanup counters. FIRST/LAST geometry
flags and the background program clear each frame, including empty tiles.

The maximum frame sequence is `UINT32_MAX/2`. With freshly constructed cleanup
records, this conservatively bounds two cleanup references per complete frame
below 32-bit wrap. Ticket and ring-serial wrap are also refused. A timeout or
uncertain kick retains the context and allocations; it does not enable reuse.
CLOSE performs the firmware cleanup described above. Explicit child disposal and
reopening require the optional standalone lifecycle profile below.

Build `apps/gpu-render` with `SUNSTONE_GPU_RUNTIME_DRAW=ON`,
`SUNSTONE_GPU_SUPERVISED=ON` and `SUNSTONE_GPU_SINGLE_CORE_APPLICATION=ON`,
then select `sunstone-a733-gpu-draw-single-core`. The executable contains the
trusted shader pair; caller-selected vertices do not change its bytes.
`apps/gpu-render/draw.py` supplies `vertices()`, `frames()` and
`capture_frames()`. The host copies every frame before submitting the next.
The single-frame `sequence()` and `capture()` functions remain available.
Capture uses a coordinator-owned, identity-validated, locked device handle;
it does not discover or launch hardware. The private version-3 mailbox has
SUBMIT, READ and CLOSE operations. Existing compute version-2 framing remains
unchanged. Session and per-request deadlines bound partial uploads and work.

## Draw compiler and software evidence

The pinned Mesa revision listed below supplies the actual PCO shader compiler,
vertex/fragment allocators and programmable data sequencer (PDS) generators for
vertex fetch, interpolation, fragment launch and descriptor upload. The descriptor program loads
the required fragment metadata word. Compiler metadata reserves eight vertex
input and eight vertex output dwords. The fragment stage uses 16 coefficient
dwords for interpolation and one shared register for metadata. This register
is not application shared memory. Vertex code occupies 88 bytes and fragment
code 64 bytes; each has its required executable padding. Shader temporary
counts are zero and five respectively. No scratch or spill storage is used.

The producer uses Mesa's feature-dependent vertex CAM allocator and generated
Rogue control encoders. The initial primitive state contains 26 words; the
40-byte vertex control stream references it, configures vertex fetch, submits
a non-indexed triangle list and terminates. Resource fields use their source
units and aligned PDS data lengths. The programs fit within the existing PDS
and unified shader core (USC) pages. The vertex page adds 4 KiB to the existing allocation.

Focused evidence is retained outside Git in
`~/.cache/sunstoneos/evidence/gpu-triangle-draw-20261019/`. It includes actual
compiler output and source identities, complete native payload comparison,
numeric input refusal, and actual FreeRTOS service execution through native
owner, translation and cache helpers. The service cases cover copied input,
stale epoch, revoked ownership, queue saturation, guarded copied output,
uncertain submission and healthy CLOSE. The linked native application and host
consumer process two different scenes on the same image. A separate instruction
case checks refusal publication before mailbox release and a subsequent retry.
Firmware replies, GPU writes and USB effects are modeled. None of these checks
establishes physical cache visibility or shader execution.

The draw profile retains the accepted opaque firmware coherence contract:
default jobs flush device caches before completion. CPU publication and acquired
completion remain necessary. Physical qualification requires a known scene,
copied background and interior colors, guard integrity and healthy shutdown;
exact floating-point edge coverage is not assumed by software evidence.

Repeated-frame evidence is retained in
`~/.cache/sunstoneos/evidence/gpu-repeated-draw-20261019/`. Twelve changing
frames exercise both retained ring wraps, alternating render-target records,
new timelines, fresh output poisoning and sequence-tagged pixel copies before
healthy CLOSE. Additional cases refuse stale frames, missing completion,
substituted context and exhausted frame/ticket/serial counters while preserving
prior output. Second-frame guard damage and an uncertain kick retain owners.
The linked native application, fragmented USB framing and host consumer process
twelve frames on one ELF. Pinned firmware-structure comparisons cover timeline
values and render-target addresses. Compiler and shader evidence above is reused.
GPU writes, firmware replies and transport effects remain modeled.

## Fixed clear profile

The native graphics service submits a 64 × 64 RGBA8 clear using actual GPU
programs, geometry and fragment command rings, firmware render context, and
[parent-backed graphics resources](gpu-graphics-memory.md). The expected pixel
is `0xffc08020`, stored as RGBA bytes `20 80 c0 ff`. GPU execution and physical
color output remain unqualified.

The application submits `PVR_GPU_SERVICE_RENDER` through the existing privileged
GPU worker. Status reports render progress and retained failures. After complete
validation, `pvr_gpu_service_render_read()` returns 64 copied pixels per request;
its first-pixel index must be a multiple of 64. No application receives a device
pointer. Acknowledged submission alone does not expose output.

## Native resources and submission

`a733_gpu_graphics_memory_prepare_clear()` requires the actual reserved firmware
and compute owners, an uninitialized service, and an explicit 64 × 64 render
profile. It uses the existing four graphics parents and shared allocator ledger.
The ordinary resource-only constructor remains available.

The constructor adds 356 KiB of cached program, state, output and fixed-fence
backing, plus 12 KiB of non-cacheable firmware context, timelines and rings.
GPU page-table storage follows the combined mappings. Capacity refusal preserves
the requested profile. Parent retention and the free-list manager retain their
existing lifecycle limits.

The graphics address space contains these mappings:

| Purpose | GPU address | Permissions and size |
| --- | --- | --- |
| Fixed general-heap fence and reserved static data | `0x8000000000` | Writable, 4 KiB, bypass GPU system cache |
| Render-target state | `0x8004000000` onward | Writable, existing render-resource layout |
| Free-list tables | `0x8006000000` onward | Firmware-protected, bypass GPU system cache |
| Context-switch state, output and border table | `0x8008000000` | Writable, 344 KiB, bypass GPU system cache |
| Programmable data sequencer programs | `0xda00000000` | Read-only, 4 KiB |
| Unified shader core programs | `0xe000000000` | Read-only, 4 KiB |

The firmware render context contains paired geometry and fragment contexts.
Their two 2 KiB command rings occupy separate non-cacheable pages. Each context
references the same graphics address space and real context-switch programs.
The geometry stream terminates an empty scene; the background shader supplies
its clear color, and the pixel-end program stores the rendered pixels.

Before power, the lifecycle worker constructs the free lists, cleans the entire
program payload, publishes the shared firmware records, and publishes the GPU
page tables. It registers all firmware objects alongside fixed compute resources
in the same firmware mapping. Private native-owner checks reject substituted
providers and pool identities.

The worker formats both command rings once, then uses the geometry/partial/final
sequence above. Geometry and final fragment retain the actual render-target
cleanup counter. Single-core mode and service admission prevent overlap with
fixed compute execution while retaining both resource sets in one service.

The worker samples the dependent fragment timeline before geometry, preventing
a false ordering failure if completion occurs between loads. After geometry
reaches n and final fragment reaches 2n, it invalidates the five output allocation pages and checks
every pixel plus the surrounding guards. Output begins poisoned. CPU code never
writes the expected clear pixels into the device output.

Each worker step has bounded memory access and observes the shared absolute
request deadline before and after callbacks. An uncertain kick, queue fault,
clock regression, missing completion, invalid pixel or damaged guard retains
both ring owners and all native parents. Terminal faults prohibit further device
access. This profile admits one fixed clear per boot. Repeated fixed-clear
submission remains unsupported. Physical context disposal is unqualified.

## Compiler and firmware references

The compiler producer uses Mesa revision
`86c7176d2dd0065555521bf8c61f208bf53c75d6`. Its archived source has SHA-256
`ca5e3ac4b61b8dc659cc489c7eb3c7bcc750f6820fbf39f0b831ac87fb00da58`.
Actual PCO compilation produces the background clear and pixel-end shaders;
Mesa generators produce the programmable data sequencer code and context-switch
state. Required upstream license notices remain in the adapted source.

The board reference revision is
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.
Its `pvr_queue.c`, `pvr_cccb.c`, firmware ABI headers and heap configuration define
the paired kick, partial-render fence, cleanup references and fixed mappings.
Mesa's `pvr_arch_job_context.c`, `pvr_arch_job_render.c`, `pvr_usc.c`, register XML
and static-memory helper define the remaining programs and register values.
The fixed RGBA8 path does not consume the reserved YUV coefficient region.

The producer and reference checks require external pinned sources. Generated
binaries and downloaded sources remain outside Git. The retained compiler output
has payload SHA-256
`11a968c9e5cb7ddc2a7731b8b924eb27cc2363aa1f79c5a489ccf4effb1b4fa6`
and register-image SHA-256
`fc61bb04be2440683c4c1a6569b019a4044cb87e40577734c84f9652cc08f4ff`.

## Validation and remaining hardware work

`checks/pvr-render.py` compares native programs against retained compiler output,
context and command records against published firmware structures, and register
fields against pinned XML. It also checks preservation on overlapping-address
refusal. `checks/gpu-render.py` composes the actual FreeRTOS worker, native parent
allocators, VM publication, firmware memory adapter and paired command transport.

The native fixture models register responses and firmware acknowledgement. Its
injected timelines are explicit model inputs. Every case leaves the device color
buffer poisoned. The cases cover rejection after modeled completion, missing
timelines, uncertain combined kick and a kick that returns after the deadline.
The source comparison passed with address and undefined-behavior sanitizers.
All four native cases passed in one batch. The reference harness needed complete
upstream headers, an alignment macro, and complete dependency capture before its
first execution. Native and shader execution were not repeated.

Evidence is retained outside Git under
`~/.cache/sunstoneos/evidence/gpu-render-20261017/`: `producer1` holds the actual
compiler outputs, `reference1` holds the source comparison, and `native1` holds
the four emulator executables and logs. The final input manifest distinguishes
executed inputs from later harness-only archival corrections.
These checks establish software behavior; they do not establish rendering.

Physical qualification requires native platform startup, admitted power and
clock resources, firmware identity and execution, interrupts, completed GPU
timelines and the unaltered color/guard checks. A bounded supervised attempt may
retain every resource until admitted manual whole-board power removal. Modes
that require autonomous watchdog recovery still need that recovery qualified.
Physical cleanup and drain must precede resource reclamation. No physical request
is submitted by these host checks.

The bounded draw profiles above do not complete graphics support. General
shader selection, the remaining texture and render modes, synchronization across
workloads, display presentation and allocation reclamation remain required. Compute
remains an independent capability. Performance work follows functional hardware
qualification.

## Copy frames across explicit disposal and reopening

`apps/gpu-render/lifecycle.py` supplies opt-in `Triangle` and `Texture` clients.
Triangle uses protocol version 7; texture and depth use versions 8 and 9.
The corresponding supervised single-core runtime-draw or texture image must
be built with `-DSUNSTONE_GPU_LIFECYCLE=ON`.
They reuse the existing vertex and texture input builders. A queue epoch binds
all requests to one queue lifetime within the original sealed session epoch.
CLOSE retains resources; DISPOSE and OPEN require explicit calls.

Create the client with the coordinator's validated, locked device handle, its
original absolute monotonic deadline and a durable callback:

```python
client = lifecycle.Triangle.for_device(
    device, sealed_epoch, deadline=session_deadline, retain=retain_gpu_copy)
pixels = client.submit(draw.vertices(rows))
client.close()
client.dispose()
client.open()
new_pixels = client.submit(draw.vertices(new_rows))
client.close()
```

For textured output, use `Texture.for_device(device, sealed_epoch, width,
height, deadline=session_deadline, retain=retain_gpu_copy, profile=2)`.
The width and height must match the authenticated framebuffer geometry.
Pass `profile=3` for depth/blend. Submit the tuple returned by the existing
`texture.input_bytes()` builder with the same profile.

The callback receives `retain(kind, identity, payload)` and confirms durable
bytes and metadata using the coordinator's existing persistence helpers.
Identity includes profile, sealed session epoch, queue epoch, frame and,
for texture, upload generation and framebuffer geometry. Complete copied
frames and terminal lifecycle receipts are durable before further mutations.
Pending STATUS replies require no separate durable write.

OPEN advances the queue epoch by one and resets frame and upload generation
to zero. No previous framebuffer remains readable; each reopened texture or
depth frame requires fresh BEGIN, WRITE and COMMIT. The client accepts a
72-byte texture response header separately from its streamed body. Existing
bounded body publications and terminating USB packets remain in use.

The original session deadline covers every operation, including reopening.
Transport, identity, persistence or deadline uncertainty stops the instance.
Its failure record, partial bytes, copied outputs and lifecycle receipts remain
available; no retry, reconnect or relaunch follows. DISPOSED is a native disposal
receipt, not independent physical drain or memory lease qualification.

The 2026-10-20 depth-mode software checks use fourteen actual producer
exchanges from native image SHA-256
`780d954475aba09f7823cfb9559e379834818d746c30d513b3d625d00c6fbea5`.
One healthy lifecycle and twelve modeled failure cases passed. They compare
complete output, lifecycle receipts and interrupted stream prefixes with the
frozen producer bytes. The failures cover session/queue epochs, contradictory
OPEN state, persistence, interruption, deadline expiry and epoch exhaustion.
No prior arithmetic or pixel matrix was repeated.

The producer executed the native mailbox/client and TinyUSB path with modeled
GPU/service completion effects tied to a separate native reconstruction record.
End-to-end GPU/firmware execution remains unqualified. Its depth OPEN receipt is
synchronous; pending texture OPEN publication through STATUS remains source-reviewed.
Triangle and version-8 texture producer-backed runtime coverage remain pending.
Physical disposal, memory reuse, visibility and timing remain unqualified.
