# Native offscreen rendering

The optional triangle application accepts a copied, caller-selected triangle
list through `pvr_gpu_service_draw()`. Real vertex and fragment shaders, vertex
fetch programs, primitive state and draw commands feed the existing paired
geometry and fragment queues. The application returns a copied 64 × 64 RGBA8
framebuffer. Physical GPU rasterization remains unqualified.

## Copied triangle submission

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

`pvr_gpu_service_draw(service, epoch, vertices, bytes, count, timeout, wait,
ticket)` copies the complete input before enqueue. The epoch comes from the
caller's OPEN reply. One scoped staging slot binds the copy to its client and
ticket; queue refusal and consumed rejection release that slot. The privileged
worker checks the current owner, epoch, native mappings and READY state again
before accepting a draw. Invalid input does not change device backing.

The graphics owner constructs and publishes all program, interpolation,
fragment-metadata, scissor and primitive state before OPEN. Runtime submission
copies the vertices into its private page and updates the draw count. It
publishes both changed pages through their EL1 mappings before the combined
kick. Input and commands remain immutable for the submitted lifetime. Both
acquired completion timelines precede output invalidation, guard checks and
pixel copies. Guard checks protect the copied extent; they do not independently
prove rasterization. Uncertain completion retains all owners.

One draw is accepted per session. Existing `pvr_gpu_service_render_read()`
returns 64 copied pixels per page after completion. CLOSE follows the normal
reviewed shutdown path. Repeated draws and context disposal remain unsupported.

Build `apps/gpu-render` with `SUNSTONE_GPU_RUNTIME_DRAW=ON`,
`SUNSTONE_GPU_SUPERVISED=ON` and `SUNSTONE_GPU_SINGLE_CORE_APPLICATION=ON`,
then select `sunstone-a733-gpu-draw-single-core`. The executable contains the
trusted shader pair; caller-selected vertices do not change its bytes.
`apps/gpu-render/draw.py` supplies `vertices()`, `sequence()` and `capture()`.
The latter uses a coordinator-owned, identity-validated, locked device handle;
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

The worker formats both command rings and publishes their commands before one
combined firmware kick. Geometry includes its timeline update. Fragment waits
on that geometry timeline, then executes its render and its own update. Both
kick records reference the actual render-target cleanup counter. Single-core
mode and service admission prevent overlap with fixed compute execution while
retaining both resource sets in one service.

The worker samples the dependent fragment timeline before geometry, preventing
a false ordering failure if completion occurs between loads. After both reach
the submitted value, it invalidates the five output allocation pages and checks
every pixel plus the surrounding guards. Output begins poisoned. CPU code never
writes the expected clear pixels into the device output.

Each worker step has bounded memory access and observes the shared absolute
request deadline before and after callbacks. An uncertain kick, queue fault,
clock regression, missing completion, invalid pixel or damaged guard retains
both ring owners and all native parents. Terminal faults prohibit further device
access. This implementation admits one fixed render job per boot. Safe repeated
submission and verified context disposal remain required.

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

Full graphics still requires general draw commands, vertex and fragment shader
production, textures, more render formats and sizes, synchronization across
workloads, repeated submission and an application graphics API. Compute remains
an independent required capability. Performance work follows functional hardware
qualification.
