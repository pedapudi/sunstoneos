# Design: GPU graphics features and zero-copy presentation

Status: **design**. No code implements it yet. Every item below ends at
implemented and host-modeled; physical behavior stays pending until the board
evidence listed at the end exists.

The design adds a new graphics profile, profile 5, to the retained texture
renderer described in [gpu-render.md](../gpu-render.md). Profile 5 adds:

- trusted shader programs compiled on the host by the pinned Mesa compiler;
- general blend factors and equations;
- hardware clipping;
- several textures per session, mipmaps and more texture formats;
- 4× multisampling resolved on chip.

It also lets the display engine scan the GPU's rendered frame in place, with
one ownership hand-off and no copy. Profiles 3 and 4 become program sets of
profile 5, and their per-frame copy code is removed. The fixed-clear,
triangle and profile-2 images stay unchanged as the frozen images for the
first physical GPU qualification.

## Data flow and ownership

The graphics owner's existing write-back lease holds four kinds of region.
Each region's size is fixed when the session opens.

- **Program region.** Unified shader core (USC) and programmable data
  sequencer (PDS) programs, at most 64 KiB and 16 KiB. The authenticated
  session envelope carries a program set of up to 32 pipelines. Each pipeline
  is a vertex and fragment program pair with metadata. The metadata gives the
  vertex attribute layout (at most 8 attributes, stride at most 64 bytes), the
  uniform bytes (at most 256), the texture slots (at most 4), the sample count,
  the register counts, and whether the fragment program reads the destination
  colour. At OPEN the worker validates
  the metadata once against the hardware limits and refuses programs that use
  scratch memory, spills, barriers, atomics or storage writes. It then copies
  the programs into the region and cleans it once. Blend state is compiled into
  fragment variants on the host by Mesa's blend lowering, so a general blend is
  the selection of one pipeline. The board does not verify shader behavior.
  The envelope's authentication establishes trust in the programs, and the
  GPU MMU confines their writes to the output slots and the depth attachment.
- **Input region.** Up to 8 textures, a vertex area of 65,536 × 64 bytes,
  the 128 KiB index area, and a batch table of up to 64 batches. A batch
  record names a pipeline, an index range, textures, fixed-function state and
  a uniform block. The host builds mip chains and twiddled (Morton-order)
  layouts, so the board copies texels as supplied. Texture formats are those
  that the pinned Mesa format table (`pvr_arch_formats.c`) lists as sampleable
  for this core: R8, R8G8, B5G6R5, B5G5R5A1, R4G4B4A4, R8G8B8A8 UNORM and sRGB,
  R16G16B16A16 SFLOAT, and the ETC2 and EAC formats.
- **Output slots.** Two slots for read-back sessions and three for scanout.
  Each slot is a 64-byte guard, the frame, and a 64-byte guard, rounded to
  pages. The render format is RGBA8 or 565. The 565 scanout pipelines swizzle
  their output so that red lands in bits 15–11, which is the layout that TV0's
  RGB565 path reads.
- **Depth and stencil attachment.** Unchanged from profile 4, and four
  samples per pixel when multisampling is selected.

The input region is lent to its producer (the USB task, or the application
task in the display image) while it is CPU-owned. The producer writes the
bytes once, in place. SUBMIT returns the region to the worker, which encodes
the batch table into service-owned command pages and cleans the written byte
ranges. A texture is written once per upload. A frame that keeps its textures
writes only vertices, indices, batch records and uniforms.

Output slots move through five states, and only the worker writes a slot's
state:

1. FREE: the GPU may be given the slot.
2. RENDERING: the slot belongs to one submitted frame.
3. COMPLETE: both timelines reached the frame and both guards are intact.
4. LENT: one consumer holds the slot under a PRESENT or READ ticket.
5. RELEASED: the consumer returned the ticket, and the slot becomes FREE.

The render target address changes per frame through the pixel back end (PBE)
end-of-tile program, which ends each render and stores the tiles. The worker
builds one such program per slot at OPEN and stores it in the program region.
The per-frame fragment command, which the worker already rebuilds for every
frame, selects the program for the frame's slot.

The display task holds a PRESENT request open. The worker answers it when a
frame reaches COMPLETE, and the display task queues the slot's address
through the existing TV0 register command queue (RCQ) replay. The display
returns the previous slot after two observations: a source-address readback
that shows the new slot, and a later DE frame-end. CLOSE is refused while any
slot is LENT. A read-back consumer receives the whole completed slot in one
reply and streams it from the slot into the USB FIFO.

Hardware clipping follows pinned Mesa: the programmable primitive pipeline
(PPP) control word sets `clip_mode` and `wclampen` (`pvr_arch_cmd_buffer.c`),
with a guard band of one quarter of the viewport. Multisampled colour resolves
on chip through the PBE box-filter downscale (`pvr_setup_emit_state` with
`down_scale`), so no multisampled colour buffer exists in DRAM. Depth and
stencil are stored only by the partial-render kick. The final kick clears
them through the background object and leaves the store bit `zstoreen` clear,
as Mesa does for an attachment whose store operation is not STORE.

**Failure classes** (section 1.8 of the performance architecture):

- An invalid batch, pipeline index, texture index or state word, a full slot
  ring at the request deadline, and an invalid program set at OPEN are refused
  before any device access. The session stays usable, and the input region
  returns to its producer.
- An uncertain kick, a missing timeline at the deadline, or a damaged guard
  leaves the device state unknown. The worker retains the rings, slots, input,
  programs and lease, which is the existing behavior.
- A display replay that misses its deadline leaves its slot LENT, and the slot
  is never reused. The GPU keeps rendering into the remaining slots. CLOSE
  retains the lease.

## Interrupts and polling

The GPU firmware interrupt wakes the worker through the existing
`pvr_gpu_service_interrupt` path. The worker answers held requests when the
render service reaches COMPLETE. A one-tick timeline poll remains as the
timeout path while GPU interrupt delivery is unqualified on the board. The
poll is removed once the board evidence shows every completion arriving by
interrupt. The per-frame CPU work fits in one worker burst, so no tick pacing
remains for CPU work.

DE frame-end and RCQ completion stay tick-polled, at 1 ms. That is the heavier
form of section 1.3, because no display interrupt is bound or qualified; the
poll goes when the display owner binds the vertical-blank interrupt. USB uses
the shared service loop.

## Memory placement and cache policy

| Region | CPU writes | GPU | Display | CPU cache maintenance per frame |
| --- | --- | --- | --- | --- |
| Programs and per-slot end-of-tile programs | Once at OPEN | Read-only | None | None |
| Command pages, at most 16 KiB | Per frame, by the worker | Read-only | None | Clean the bytes written |
| Input region | By the producer while CPU-owned | Read-only | None | Clean the byte ranges written |
| Output slots | Guards only, once at OPEN, then clean and invalidate | Read-write | Read-only through system-IOMMU master 8 | Invalidate the two guard lines; invalidate a read-back range once, before lending it |
| Depth and stencil | Guard pages once at OPEN | Read-write | None | Invalidate the two guard pages |
| Firmware contexts, rings, timelines | Existing non-cacheable pool | Existing | None | None |

No CPU stores reach slot pixels after OPEN, so the CPU cache never holds a
dirty line that could overwrite GPU output. A line that the CPU prefetches
speculatively stays clean, so the display, which reads DRAM directly, is not
affected. The firmware's existing job-coherency flush precedes the final
timeline update, and the display reads a slot only after that update.
`aarch64_device_cache` cleans and invalidates by virtual address over exact
ranges. One `DSB SY` completes the maintenance before each kick, and the two
kick doorbells keep the existing `DSB SY` until weaker barriers are qualified
in `a733_mmio.h`.

## Synchronization across cores

The GPU worker is the only writer of render state, slot states and command
pages. Producers and consumers communicate only through the existing service
request queue, whose critical sections order a producer's stores before the
worker's receive. Data-cache maintenance by virtual address to the point of
coherency is broadcast to every core in the shareable domain. The worker can
therefore clean bytes that a producer on another core wrote, and invalidate a
range before lending it to a reader on another core. The worker reads the
timelines with acquire loads, the fragment timeline first, as it does today.
The interrupt handler only posts a wake message. The display returns a slot
only after the DE has stopped reading it, so no CPU-side ordering is needed
for that hand-off. Worker placement follows the GPU service's affinity. The
per-frame CPU cost budget makes placement secondary.

## Reuse and removal

The design reuses the paired geometry, partial-render and final kick, the
timelines, the PRESENT and RELEASE tickets, the Mesa producer scripts, the
TV0 RCQ replay, the authenticated envelope and the graphics lease.

It removes the following from the per-frame path:

- the CPU snapshot copy, the draft, and the 64 KiB upload and download slots;
- the poison fill of the whole output. Texture profiles never check the
  poison, and the background clear writes every pixel of every frame;
- the zero-fill and copy of the maximum-size texture and geometry areas;
- the CPU initialization of the depth attachment, which the background clear
  replaces and no kick loads;
- the per-page `a733_gpu_identity_mapped` translations. They run once at OPEN,
  and the mappings cannot change before DISPOSE;
- the per-word guard loop over the whole frame;
- the per-vertex position check, which hardware clipping replaces. The
  per-index check is also removed, because the GPU mapping of the vertex area
  is read-only and covers every 16-bit index;
- the RGBA8-to-RGB565 conversion and the display-owned frame copies in
  `apps/gpu-display`.

## Budgets

The baselines are counted from the code at the integration base, for a
1920 × 1080 profile-4 frame and for the 1280 × 720 profile-2 display frame.
They become measured when the harnesses below gain the counters. A new
Unicorn harness, `checks/gpu-render-frame-cost.py`, runs the actual
`pvr_render_service.c` frame path. Its hooks count instructions, `AT`
instructions, `DC` operations multiplied by 64 bytes, and bytes stored by
region. `checks/gpu-texture-runtime.py` and `checks/gpu-display-replay.py`
gain counters for round trips, wake-ups and ticks.

| Metric | Baseline | Target |
| --- | --- | --- |
| CPU bytes stored by the worker per frame | 60.2 MB at 1080p; 17.4 MB at 720p | At most 16 KiB of command words |
| CPU cache maintenance per frame | 68.5 MB at 1080p; 21.1 MB at 720p | Bytes written plus 8.3 KiB of guards; zero slot bytes on scanout |
| `AT` translations per frame | About 38,000 at 1080p, plus about 8,100 for read-back | Zero |
| Worker tick waits caused by CPU work | About 65 at 1080p; about 20 at 720p | Zero |
| Guard-loop iterations per frame | 2,074,624 at 1080p | 32 |
| Worker instructions per frame, 64 batches | Not measured | At most 100,000 |
| Copies per input byte after transport | Three; the texture is copied every frame | At most one; the texture once per upload |
| Copies per displayed pixel byte | Three, plus two SHA-256 passes | Zero |
| Copies per read-back byte | Three, including the USB FIFO write | One, the USB FIFO write |
| Queue round trips to read back a frame | 254 at 1080p; 114 at 720p | Two |
| Client wake-ups per frame | One per tick of rendering (STATUS poll) | At most two |
| GPU interrupt to RCQ submit | Not applicable; frames are presented after CLOSE | No tick wait, at most two context switches |
| CPU depth initialization per frame | 32 MiB plus 8 KiB at 1080p | Zero |
| Depth and stencil store in the final kick | Enabled | Disabled; partial renders only |
| Lease bytes for the same frame size and depth mode | 36.7 MB at 1080p profile 2 | No larger |

The register decode in a new `checks/gpu-render-programs.py` checks the clip,
store and downscale rows. It unpacks the PPP control, `CR_ISP_ZLSCTL` and PBE
words with the pinned Mesa packers. The gpu-conformance package compares
rendered pixels against host Mesa software rendering.

## Changes needed from other owners

- **GPU runtime** (`pvr_gpu_service.c`, `gpu_graphics_memory.c`):
  - held TEXTURE replies, answered on completion or when a slot frees, under
    the request's own deadline;
  - a `timed()` that sleeps while the render service waits on the device and
    the interrupt is armed;
  - lending the input region in place of the upload-chunk copy;
  - VM mapping plans for the slots and the program region.
- **Display** (`a733_display_framebuffer`): import N read-only frame extents
  into the system-IOMMU master 8 tables at admission, replacing its own frame
  copies.
- **Primary agent:** none.

## Left out

- **More than one frame in flight on the GPU.** It needs a second
  render-target record and parameter-buffer accounting. The slot ring already
  overlaps rendering with scanout.
- **Double-buffered input.** With resident textures the producer writes a few
  kilobytes per frame, so the GPU idles for microseconds between frames.
- **On-board shader compilation and untrusted shaders.**
- **Other topologies, instancing, primitive restart, storage resources and
  8× multisampling.** No board consumer uses them.
- **On-board mipmap generation.** The host generates mip chains.
- **Mailbox presentation, framebuffer compression and evidence hashing on the
  scanout path.** The DE cannot read compressed frames, and hashing stays in
  the evidence image.
- **Display modes and formats other than 1280 × 720 RGB565.** They belong to
  the display owner.

## Board evidence that confirms the design

- HDMI capture of a 60 Hz sequence of alternating high-contrast frames shows
  no torn or stale lines after slot recycling.
- DE underflow status stays clear during that sequence.
- Every frame's completion arrives by interrupt, and none is detected first by
  the one-tick poll.
- Guards stay intact over the sequence.
- Frames from each feature (clipped triangles, mip levels, ETC2 textures, 4×
  multisampling, the blend variants) match the host software renderer within
  the conformance tolerances.
- Frame times are measured under [validation.md](../validation.md).
