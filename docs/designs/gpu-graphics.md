# Design: GPU graphics features and zero-copy presentation

Status: **design**. No code implements it yet. Every item below ends at
implemented and host-modeled; physical behavior stays pending until the board
evidence listed at the end exists.

The design adds graphics profile 5 to the retained renderer described in
[gpu-render.md](../gpu-render.md). Profile 5 adds:

- trusted shader programs compiled on the host by the pinned Mesa compiler;
- general blend factors and equations;
- hardware clipping;
- several textures per session, mipmaps and more texture formats;
- 4× multisampling resolved on chip.

The display engine (DE) scans the GPU's rendered frame in place, with one
ownership hand-off and no copy.

**One frame path.** The fixed-clear, triangle and texture renderers of draw
modes 0 to 4 (profiles 2, 3 and 4 among them) become program sets of profile 5.
The change that lands profile 5 deletes their per-frame code from
`pvr_render_service.c`: the draft, the snapshot, the per-reply pixel output,
the poison fill, the per-page `mapped()` checks and the per-word guard loop.
The upload-chunk copy in `pvr_gpu_service.c` is deleted in the same change
sequence by its owner. The images used for the first physical GPU qualification
are not rebuilt from the live tree. Each is pinned by the source commit and
image SHA-256 already recorded in [gpu-render.md](../gpu-render.md) and
[gpu-display.md](../gpu-display.md), and the board session loads the preserved
bytes after checking the SHA-256. A new `checks/gpu-render-frozen.py` rebuilds
each pinned image from its recorded commit under the package build directory
and compares objects and the image SHA-256 with the record, in the way
gpu-display.md already compares objects between its two images.

## Data flow and ownership

The graphics owner's write-back lease is acquired with 2 MiB alignment and
holds three regions. Their sizes are fixed when the session opens. Each limit
below names the consumer that needs it. The consumers are the gpu-display demo
(1280 × 720 RGB565 scanout), the gpu-render evidence frames (read-back), and
the gpu-conformance package, which compares rendered pixels with host Mesa
software rendering.

- **Small region**, one 2 MiB block. It holds the unified shader core (USC)
  programs (at most 32 KiB), the programmable data sequencer (PDS) programs (at
  most 8 KiB), one end-of-tile program per output slot and the command pages
  (at most 16 KiB). It also holds the 128 KiB index area and the batch table of
  at most 16 batches, the profile-4 limit that the evidence frames use.
- **Vertex area**, one 2 MiB block: 65,536 vertices of at most 32 bytes. Four
  attributes at most (position, texture coordinate, colour and one spare);
  profile 4 already uses 24-byte vertices.
- **Texture area**, at most 8 MiB, which holds the 1920 × 1080 RGBA8 texture
  of the profile-2 to profile-4 program sets. At most 4 textures per session;
  the mip and format evidence frame samples four. Required formats are
  R8G8B8A8 UNORM and sRGB, B5G6R5 UNORM, ETC2 RGB8 and EAC R11 UNORM, all
  sampleable in the pinned Mesa table (`pvr_arch_formats.c:89–105, 160–169`).
  The other formats in that table are optional and are not in the first
  implementation.
- **Output slots.** Two for read-back sessions and four for scanout. Each slot
  is a run of 2 MiB blocks. The frame starts 4 KiB into the run, with a 64-byte
  guard line directly before and directly after it.

There is no depth or stencil attachment in DRAM. Depth and stencil live in
on-chip tile memory: the background object clears them, the final kick neither
loads nor stores them (`zloaden` and `zstoreen` clear), and the ZLS base
addresses point at an unmapped GPU address. Multisampled colour resolves on
chip through the pixel back end (PBE) box-filter downscale (`down_scale`), so
no multisampled colour buffer exists in DRAM either.

The authenticated session envelope carries one program set of at most 8
pipelines. The largest set is the 1× evidence set: the profile-4 opaque
pipeline and five blends, the clipped-triangle pipeline and the mip and format
pipeline. All pipelines in a session share its sample count. Each pipeline is
a vertex and fragment program pair with metadata: attribute layout, uniform
bytes (at most 128, one 4 × 4 matrix and four vec4 constants), texture slots
(at most 4), register counts, and whether the fragment program reads the
destination colour. At OPEN the worker validates the metadata once and
refuses:

- programs that declare scratch memory, spills, barriers, atomics or storage
  writes;
- an attribute whose offset plus size exceeds the stride;
- a PDS data-segment template with a nonzero address field.

It then copies the programs into the small region, builds one end-of-tile
program per slot, writes the slot guards, and cleans and invalidates every
slot's full page range and the small region once. That pass runs after the
lease zeroing in `gpu_graphics_memory.c`, which uses cacheable stores, and
before the first kick. Blend state is compiled into fragment variants on the
host by Mesa's blend lowering, so a general blend is the selection of one
pipeline.

**Output format.** The per-slot end-of-tile program carries the PBE pack mode
and swizzle. Mesa accumulates both R8G8B8A8 UNORM and B5G6R5 UNORM as U8
(`pvr_arch_formats.c:199, 211`), so one fragment program serves both outputs.
The R5G6B5 pack mode places red in bits 15–11, the layout that TV0's RGB565
path reads, and the swizzle stays the identity.

**Program trust.** The board does not verify shader behavior. Programs are
trusted for three reasons. The envelope authentication admits only host-signed
program sets. The host producer decodes every USC and PDS instruction with the
pinned Mesa disassembler and refuses store, atomic, scratch and DMA-write
instructions before signing. OPEN refuses the metadata flags listed above. A
program that wrote memory anyway could reach these writable GPU mappings: the
general-heap fence and static data (`0x8000000000`), the render-target state
(`0x8004000000` onward), the context-switch state and border table
(`0x8008000000`), and the output slots. The worker writes every address in a
PDS data segment (vertex base, index base, texture descriptors, uniform
blocks) from validated batch fields. Host programs carry no absolute
addresses. The read-only GPU mappings of programs and input are an additional
barrier; the design does not rely on them.

**Input lending.** The small region's index and batch areas, the vertex area
and the texture area are lent to their producer (the USB task, or the
application task in the display image) while CPU-owned. The producer writes
each byte once, in place. SUBMIT returns them to the worker. The worker copies
each batch record once into its own memory, validates the copy and encodes
from it, so a producer that keeps writing cannot change what was validated. It
builds texture descriptors from validated fields and checks that each
texture's extent fits its texture slot. The extent includes the mip chain, the
twiddle padding and ETC2 and EAC 4 × 4 block rounding. It then cleans the
written byte ranges. A texture is written once per upload.

**Indices.** The per-index check goes away under three rules checked at OPEN
or in each batch: the attribute bound above, no base-vertex field in a batch
record, and a fixed 16-bit index type. Every index then addresses a vertex
inside the 2 MiB vertex block. An unmapped 2 MiB GPU address gap follows it.

**Output slots** have four states, and only the worker writes a slot's state:

1. FREE: the GPU may be given the slot.
2. RENDERING: the slot belongs to one submitted frame.
3. COMPLETE: both timelines reached the frame, the partial-render count is
   zero and both guard lines are intact.
4. LENT: one consumer holds the slot under a PRESENT or READ ticket. Returning
   the ticket moves the slot back to FREE.

**Present and retire.** The display task holds a PRESENT request open. The
worker answers it with a task notification when a frame reaches COMPLETE. The
display task runs the TV0 register command queue (RCQ) replay for the slot's
address. The pinned display driver issues an RCQ update and polls for RCQ
finish (`sunxi_de.c:715–775`, `de_top.c:1077–1082`) before it signals the
vertical-blank event. Neither it nor gpu-display.md establishes when the DE
stops fetching the old address, and it is not known whether the UCH3
source-address readback returns the pending or the active register. The
retire rule is therefore conservative. The display returns the previous slot
only after three observations:

1. RCQ finish;
2. the source-address readback that shows the new slot, in the same burst as a
   write-to-clear of the frame-end status bit;
3. two later frame-end observations.

A missed frame-end observation only delays the return. Four scanout slots
(displayed, pending, retiring and rendering) keep 60 Hz possible under this
rule. Board evidence that isolates retirement (below) may later qualify a
one-frame-end rule, which reduces scanout to three slots.

A read-back consumer receives the whole completed slot in one reply. The worker
invalidates the frame range once before lending it, and the consumer streams
it into the USB FIFO.

**Partial renders are refused.** The global free-list maximum is sized at OPEN
to a parameter-buffer bound computed from the admitted limits: vertex output
bytes plus control-stream bytes per tile for the worst primitive-block count.
OPEN refuses limits whose bound exceeds the 16,384-page free-list maximum.
After both timelines complete, the worker reads the render target's
partial-render count from the firmware HWRT data (`pvr_rogue_fwif.h:2049–2050`).
A nonzero count means the frame lost the geometry rendered before the partial
render. The frame is reported as failed and its slot returns to FREE. The
first render clears depth, and no frame loads stored depth, so the CPU depth
initialization goes away.

**Shutdown.** The display stops issuing replays. Then, in order:

1. Stop TV0 and the DE, and observe the stop.
2. Remove the master-8 IOMMU translations of the slot extents and invalidate
   the IOMMU TLB. The display owner holds these translations.
3. Return every LENT slot.
4. Close the GPU. CLOSE is refused while a slot is LENT, and DISPOSE is refused
   while any extent is imported.
5. Park the PMIC.

The sequence must finish inside the existing 2-second HDMI stop reserve.
Step 1 takes at most one frame plus the existing HDMI stop, and step 2 is about
1,800 PTE writes and one invalidation. If any step is not observed, the display
bank and the GPU lease are both retained.

**Failure classes** (section 1.8 of the performance architecture):

- Known state, no retention: an invalid batch, pipeline, texture, extent or
  state word, a full slot ring at the request deadline, or an invalid program
  set at OPEN is refused before any device access. A frame with a nonzero
  partial-render count is failed after completion. The input returns to its
  producer and the session stays usable.
- Unknown device state: an uncertain kick, a missing timeline at the deadline,
  a damaged guard line or a GPU MMU fault. The worker retains the rings, slots,
  input, programs and lease, which is the existing behavior.
- Display replay past its deadline: the previous and the pending slot both stay
  LENT, and the display refuses further replays and MMIO, which is the existing
  gpu-display behavior. The GPU may still render into the remaining slots for
  read-back. CLOSE retains the lease and the display bank stays retained.

## Interrupts and polling

**GPU kicks.** Each frame writes geometry plus UPDATE into the geometry ring.
It writes FENCE_PR, the partial-render fragment command and UPDATE, followed by
the final FRAG and UPDATE, into the fragment ring. One combined KCCB kick
covers both rings, as in pinned Linux `pvr_queue.c:626–780`. The frame path
waits on no KCCB return slot. Ring space comes from the firmware read offset.
Completion is detected only from the timelines after the firmware interrupt.
The firmware-executing checks (`checks/pvr-batch-fw.c` and
`pvr-depth-blend-fw.c`) confirm the single-kick sequence before the
frame path switches to it. If the firmware refuses it, the frame keeps two
kicks, written back to back with no return-slot wait between them; Linux issues
its final fragment kick in the same way.

**Worker waits.** While GPU interrupt delivery is unqualified, the worker waits
at most one tick on its queue while a frame renders. After the board evidence
shows every completion arriving by interrupt, the worker blocks until the
frame deadline. This is the timeout form that section 1.3 permits, with its
removal condition stated.

**Display task.** The display task blocks on the PRESENT reply or a task
notification, with no `vTaskDelay(1)` between steps. It runs the DE replay
steps back to back in one burst. It polls at 1 ms only at the two real device
waits, RCQ finish and frame-end, because no display interrupt is bound or
qualified. The polls go when the display owner binds the vertical-blank
interrupt. The IOMMU fault scan runs once at admission. After that it runs
once per present, in the burst that observes a frame-end, as one burst of
register reads. It is no longer a gate before each replay request; it only has
to be exclusive with RCQ polling. The next replay is requested as soon as the
PRESENT reply arrives and the previous replay has passed its readback. The
RCQ then loads at the next frame boundary. While frames are presented, the
display task wakes about 1,000 times a second, once per tick, and each wake
reads one status register.

## Memory placement and cache policy

| Region | GPU page size and mapping | CPU writes | Cache maintenance per frame |
| --- | --- | --- | --- |
| Small region | 4 KiB pages, read-only to the GPU, about 48 leaf PTEs | Programs once at OPEN; commands by the worker; indices and batches by the producer | Clean the bytes written |
| Vertex area | One 2 MiB page, read-only | By the producer | Clean the byte ranges written |
| Texture area | 2 MiB pages, read-only | By the producer, once per upload | Clean the byte ranges written |
| Output slots | 2 MiB pages, read-write, each followed by an unmapped 2 MiB gap | Guard lines once at OPEN | Invalidate the two guard lines of the rendered slot; invalidate a read-back frame once before lending it |
| Depth and stencil | None; ZLS base unmapped | None | None |
| Firmware contexts, rings, timelines | Existing non-cacheable pool | Existing | None |

Within a 2 MiB GPU address region all mappings share one page size
(`pvr_gpu_vm.c:43`). The pinned Linux driver supports device pages from 4 KiB
to 2 MiB (`pvr_mmu.c:32–57`), so 2 MiB pages are within the reference driver's
range, but they are unqualified on this board. The DE reads slots through
system-IOMMU master 8, which has only 4 KiB pages. The pinned
`sunxi-iommu-pgtable.c:173–199` writes 4 KiB PTEs under 1 MiB directory
entries, whatever size the IOMMU core passes. A 720p RGB565 frame is 450 pages,
so the DE fetch costs at most 450 page walks per scanned frame. That is 27,000
a second at 60 Hz, about one every 35 µs or every 1.6 scanlines.

No CPU stores reach slot pixels after OPEN. A line that the CPU prefetches
speculatively stays clean, so it cannot overwrite GPU output, and the display
reads DRAM directly. The firmware's job-coherency flush precedes the final
timeline update, and the display reads a slot only after that update.

**Barriers.** These are the targets once weaker barriers are qualified in
`a733_mmio.h`. Until then the existing `DSB SY` forms stay.

- CCCB command words are plain stores into non-cacheable memory, in place of
  today's store-release per 32-bit word in `transfer()`.
- One `DMB OSHST` after the command words and before the ring write offsets.
  One more before the KCCB doorbell, which is the Linux `writel` ordering.
- One cache-maintenance pass over every range cleaned for the frame, then one
  `DSB SY` to complete it. Today each `aarch64_device_cache` call issues a
  leading and a trailing `DSB SY`.

## Synchronization across cores

The GPU worker is the only writer of render state, slot states, batch copies
and command pages. Producers and consumers communicate with it only through the
existing service request queue, whose critical sections order a producer's
stores before the worker's receive. Data-cache maintenance by virtual address
to the point of coherency is broadcast to every core in the shareable domain.
The worker can therefore clean bytes that a producer on another core wrote. The
worker reads the timelines and the partial-render count with acquire loads,
the fragment timeline first, as it does today. The interrupt handler only posts
a wake message. The display returns a slot only after the retire rule above,
so that hand-off needs no CPU-side ordering beyond the request queue.

**Placement.** Once the multicore work admits the A76 cores for the graphics
images, the GPU worker runs on core 6 and the display task on core 7. Until
then they run on A55 core 1, because USB and the service tasks stay on core
0. GPU SPI 95 is routed to the worker's core, so a completion wake needs no
inter-processor interrupt (IPI). The worker's PRESENT notification to the
display task costs at most one IPI per frame.

## Reuse and removal

The design reuses the paired geometry and fragment rings, the timelines, the
second render-target record, the PRESENT and RELEASE tickets, the Mesa
producer scripts, the TV0 RCQ replay, the authenticated envelope, the graphics
lease and the existing free-list grow path.

It removes:

- the draft, the snapshot copy, the 64 KiB upload and download slots and the
  per-reply pixel output, for every draw mode;
- the poison fill of the output. The background clear writes every pixel of
  every frame;
- the zero-fill and copy of the maximum-size texture and geometry areas;
- the DRAM depth attachment, its guard pages, its CPU initialization (32 MiB
  plus 8 KiB per frame at 1080p) and its store;
- the per-page `a733_gpu_identity_mapped` translations. They run once at OPEN,
  and the mappings cannot change before DISPOSE;
- the per-word guard loop over the frame, replaced by two guard lines;
- the per-vertex position check. Programmable vertex shaders compute positions
  on the GPU, so no CPU check can see them. The PBE render-target bounds and
  the GPU MMU confine writes. A hang or garbage geometry reaches the timeline
  deadline and is retained. Clipped-triangle conformance stays pending board
  evidence;
- the per-index check, covered by the three index rules above;
- the RGBA8-to-RGB565 conversion and the frame copies in `apps/gpu-display`;
- the second KCCB kick's return-slot wait and the per-word store-release.

Guard coverage is reduced in one place. A write into a slot's page-rounding
padding after the trailing guard line is not detected. That padding belongs to
the same slot and holds no other data. Anything beyond it reaches the unmapped
gap and faults in the GPU MMU.

## Budgets

The baselines are counted from the code at the integration base, for a
1920 × 1080 profile-4 frame and the 1280 × 720 profile-2 display frame. They
become measured when the existing harnesses gain the counters.
`checks/gpu-texture-runtime.py` already runs the linked image's frame path
under Unicorn. It gains hooks that count:

- instructions, `AT` instructions, `DSB` and `DMB` instructions, cache
  maintenance calls, and `DC` operations multiplied by 64 bytes;
- bytes stored by region;
- KCCB kicks, return-slot waits, worker wake-ups and IPIs;
- instructions and context switches between completion and the next kick.

`checks/gpu-display-replay.py` gains counters for display-task ticks, register
accesses and IOMMU scans per present, and for the master-8 PTEs that the DE
fetch touches. No new harness is added.

| Metric | Baseline | Target |
| --- | --- | --- |
| CPU bytes stored by the worker per frame | 60.2 MB at 1080p; 17.4 MB at 720p | At most 17 KiB: command words plus batch copies |
| CPU cache maintenance per frame | 68.5 MB at 1080p; 21.1 MB at 720p | Bytes written plus 128 bytes of guards; zero slot bytes on scanout |
| `AT` translations per frame | About 38,000 at 1080p, plus about 8,100 for read-back | Zero |
| Worker tick waits caused by CPU work | About 65 at 1080p; about 20 at 720p | Zero |
| Guard-loop iterations per frame | 2,074,624 at 1080p, plus 2,048 for depth guards | 32 |
| KCCB kicks and return-slot waits per frame | Two kicks, two waits | One kick, no wait (two kicks if the firmware refuses one) |
| Worker wake-ups per frame beyond interrupts | One per tick of rendering | At most two once delivery is qualified (SUBMIT, RELEASE) |
| Client wake-ups per frame | One per tick of rendering (STATUS poll) | At most two |
| Worker instructions per frame, 16 batches | Not measured | At most 100,000 |
| `DSB` and `DMB` per frame | Not measured; two `DSB SY` per maintenance call plus one at each ring publish | Once qualified: one `DSB SY` and two `DMB OSHST` per kick |
| GPU idle gap per frame, completion to next kick | Not measured | No tick wait; counted in instructions and context switches |
| Copies per input byte after transport | Three; the texture is copied every frame | At most one; the texture once per upload |
| Copies per displayed pixel byte | Three, plus two SHA-256 passes | Zero |
| Copies per read-back byte | Three, including the USB FIFO write | One, the USB FIFO write |
| Queue round trips to read back a frame | 254 at 1080p; 114 at 720p | Two |
| GPU interrupt to RCQ submit | Not applicable; frames are presented after CLOSE | No tick wait, at most two context switches and one IPI |
| Display-task ticks per present outside device waits | About 40: 22 scan steps and 18 replay steps, one per tick | Zero; RCQ-finish and frame-end waits within one frame period |
| Register accesses and IOMMU scans per present | About 38 accesses; one scan that gates the replay | About 38 accesses in bursts plus one status read per tick of device wait; one scan, not gating |
| GPU leaf PTEs per session | About 9,000 at 1080p profile 2 (4 KiB pages) | About 60, checked in `checks/pvr-gpu-vm.c` |
| Master-8 page walks per scanned 720p frame | 450 (4 KiB IOMMU pages) | 450; the IOMMU has no larger page |
| CPU depth initialization per frame | 32 MiB plus 8 KiB at 1080p | Zero; no DRAM depth |
| Partial renders per admitted frame | Not checked | Zero; a nonzero count fails the frame |
| Shutdown, stop request to PMIC park | GPU closed before TV0 fetch | Inside the 2-second HDMI stop reserve |
| Lease bytes | 36.7 MB at 1080p profile 2 | 29.4 MB at 1080p read-back; 21.0 MB at 720p scanout |

## Changes needed from other owners

- **GPU runtime** (`pvr_gpu_service.c`, `pvr_cccb.c`,
  `gpu_graphics_memory.c`, `gpu_disposal.c`, `pvr_gpu_service.h`):
  - held PRESENT and READ replies, answered by task notification;
  - a `timed()` with the two wait forms stated under "Worker waits";
  - the single combined kick, ring space from the firmware read offset, and
    plain stores for CCCB words;
  - deletion of the TEXTURE WRITE upload-chunk copy (`pvr_gpu_service.c:1020`)
    and the 64 KiB output accessor, in the change sequence that lands profile 5;
  - the lease acquired with `device_memory_acquire_aligned` at 2 MiB, the VM
    plans of the table above, no depth region, an import count, and DISPOSE
    refused while any extent is imported.
- **GPU checks** (`checks/pvr-batch.c`, `pvr-texture.c`, `pvr-depth-blend.c`,
  `pvr-gpu-vm.c`, `gpu-batch.*`, `gpu-resource-disposal.py`): `pvr-batch.c`
  gains the profile-5 assertions, using the pinned Mesa packers it already
  uses: `clip_mode` and `wclampen`, `zloaden` and `zstoreen` clear,
  `down_scale`, and the PBE pack mode and swizzle. The profile-3 and profile-4
  cases in `pvr-texture.c` and `pvr-depth-blend.c` are deleted with their
  code. `pvr-gpu-vm.c` counts leaf PTEs.
- **Build** (`cmake/gpu.cmake`, `a733-gpu-application.cmake`,
  `a733-accelerators.cmake`): draw-mode selection becomes program-set
  selection.
- **Display and IOMMU** (`a733_display_engine.c`, `a733_display_framebuffer.c`,
  `a733_iommu.c`):
  - a poll result that separates progress from a device wait, so the caller
    can run bursts;
  - a replay that ends at RCQ finish and readback, and a separate frame-end
    step of one read and one write-to-clear;
  - an IOMMU scan that runs as one burst;
  - import of N read-only slot extents into the master-8 tables at admission,
    and their removal with a TLB invalidation at shutdown.
- **Primary agent:**
  - route SPI 95 to the GPU worker's core;
  - admit cores 6 and 7 for the graphics images;
  - a multi-range cache-maintenance call in `arch/aarch64/device_memory.c` with
    one trailing `DSB SY`.

## Left out

- **More than one frame in flight on the GPU.** The second render-target
  record already exists. The blockers are the single input region and the
  parameter-buffer bound, which would double. The GPU idle-gap row measures
  the cost of waiting.
- **Partial renders.** Mesa's scratch path for them would need 14.7 MB for
  unresolved 4× colour at 720p, a DRAM depth attachment, and load programs.
- **4× multisampling above 1280 × 720, and 8×.** No consumer needs them.
- **Double-buffered input.** With resident textures the producer writes a few
  kilobytes per frame.
- **On-board shader compilation, untrusted shaders and a board-side
  instruction scan.** The scan would repeat the host decode with a second
  decoder for the same instruction set.
- **sRGB render targets.** Mesa accumulates them as F16, which would need a
  second fragment variant of every pipeline.
- **Other topologies, instancing, primitive restart, storage resources and the
  optional texture formats.** No consumer uses them.
- **On-board mipmap generation.** The host generates mip chains.
- **Mailbox presentation, framebuffer compression and evidence hashing on the
  scanout path.** The DE cannot read compressed frames, and hashing stays in
  the evidence image.
- **Display modes other than 1280 × 720 RGB565, and the vertical-blank
  interrupt.** They belong to the display owner.

## Board evidence that confirms the design

- HDMI capture of a 60 Hz sequence of alternating high-contrast frames, with
  the GPU rendering while the DE scans, shows no torn or stale lines.
- Retirement isolation: the GPU writes a frame-unique marker into a slot's
  first and last rows as soon as the slot returns. The HDMI capture never shows
  a marker in a frame presented before that return. One run uses the
  conservative rule and one a one-frame-end candidate.
- DE underflow status stays clear during the sequence.
- Every completion arrives by interrupt, and none is detected first by the
  one-tick poll.
- Guard lines stay intact. The partial-render count stays zero at the admitted
  limits. No GPU MMU fault occurs, which shows that 2 MiB pages work and that
  the unmapped ZLS base is never accessed.
- The shutdown sequence completes inside the HDMI stop reserve and the lease
  returns to the pool.
- Frames from each feature (clipped triangles, mip levels, ETC2 and EAC
  textures, 4× multisampling, the blend variants) match the host software
  renderer within the conformance tolerances.
- Frame times are measured under [validation.md](../validation.md).
