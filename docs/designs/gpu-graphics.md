# Design: GPU graphics features and zero-copy presentation

Status: **design**. No code implements it yet. Every item ends at implemented
and host-modeled; physical behavior stays pending until the board evidence at
the end exists.

Graphics profile 5 extends the retained renderer of
[gpu-render.md](../gpu-render.md). It adds trusted shader programs compiled on
the host by the pinned Mesa compiler, general blend factors and equations,
hardware clipping, several textures per session with mipmaps and more formats,
and 4× multisampling resolved on chip. The display engine (DE) scans the rendered
frame in place, with one ownership hand-off and no copy.

The fixed-clear, triangle and texture renderers (draw modes 0 to 4) become
program sets of profile 5, and the change that lands profile 5 deletes their
per-frame code from `pvr_render_service.c`. The images used for the first
physical GPU qualification stay pinned by the source commit and image SHA-256
recorded in [gpu-render.md](../gpu-render.md) and
[gpu-display.md](../gpu-display.md). The board session checks that SHA-256
before it loads the preserved bytes. If provenance has to be reconfirmed, the
image is rebuilt once at the recorded commit under the package build directory
and the result is recorded in gpu-render.md.

## Data flow and ownership

The graphics owner's write-back lease is acquired with 2 MiB alignment. Its
sizes are fixed at OPEN, and each limit names its consumer: the gpu-display
demo (1280 × 720 RGB565 scanout), the gpu-render evidence frames (read-back),
and the gpu-conformance scenes, which are compared with host Mesa software
rendering.

- **Small region**, one 2 MiB block: unified shader core (USC) programs (at
  most 32 KiB), programmable data sequencer (PDS) programs (at most 8 KiB), one
  end-of-tile program per output slot, command pages (at most 16 KiB), the
  128 KiB index area, and a table of at most 16 batches.
- **Vertex area**, one 2 MiB block: 65,536 vertices of at most 32 bytes with
  three attributes, position, texture coordinate and colour.
- **Texture area**, at most 8 MiB and 4 textures. It holds the 1920 × 1080
  RGBA8 texture of the profile-2 to profile-4 program sets. Formats: R8G8B8A8
  UNORM and sRGB, B5G6R5 UNORM, ETC2 RGB8 and EAC R11 UNORM, all sampleable in
  the pinned Mesa table (`pvr_arch_formats.c:89–105, 160–169`).
- **Output slots**, two for read-back and four for scanout. Each slot is a run
  of 2 MiB blocks, and the frame ends at the end of its run. A 720p RGB565
  frame is 450 pages and a 1080p RGBA8 frame is 2,025 pages, so both start on
  a page boundary.

Depth and stencil live only in on-chip tile memory. The background object
clears them, and both the partial-render and the final fragment commands carry
`zloaden` and `zstoreen` clear. The ZLS base addresses point at an unmapped GPU
address. Multisampled colour resolves on chip through the pixel back end (PBE)
downscale (`down_scale`). The per-slot end-of-tile program carries the PBE pack
mode. Mesa accumulates both R8G8B8A8 and B5G6R5 as U8
(`pvr_arch_formats.c:199, 211`), so one fragment program serves both outputs.

**Program sets.** The authenticated session envelope carries one program set of
at most 8 pipelines that share one sample count. Each pipeline is a vertex and
fragment program pair with metadata: attribute layout, at most 128 uniform
bytes, at most 4 texture slots, register counts and destination-colour reads.
Blends are compiled into fragment variants on the host. At OPEN the worker
validates the metadata once, copies the programs into the small region, builds
the end-of-tile programs, and cleans the small region once. The appendix gives
the trust argument.

**Input lending.** The index, batch, vertex and texture areas are lent to their
producer (the USB task, or the application task in the display image) while
CPU-owned. The producer writes each byte once, in place, and SUBMIT returns the
areas to the worker. The worker copies each batch record once into its own
memory and validates and encodes from the copy. It builds texture descriptors
from validated fields and checks each texture's extent, including the mip
chain, twiddle padding and 4 × 4 block rounding, against its slot. It then
cleans the written ranges. Indices need no per-index check: the vertex stride
bounds every attribute, batch records have no base-vertex field, and indices
are 16-bit, so every index addresses the 2 MiB vertex block, which an unmapped
2 MiB GPU gap follows.

**Output slots.** Only the worker writes a slot's state:

1. FREE: the GPU may be given the slot.
2. RENDERING: the slot belongs to one submitted frame.
3. COMPLETE: both timelines reached the frame, both freshness markers were
   overwritten, and no partial-render signal is set.
4. LENT: one consumer holds the slot under a PRESENT or READ ticket. Returning
   the ticket moves the slot back to FREE.

**Present and retire.** The display task holds a PRESENT request open, and the
worker answers it with a task notification when a frame reaches COMPLETE. The
display task runs the TV0 register command queue (RCQ) replay for the slot's
address. It returns the previous slot only after RCQ finish, a source-address
readback that shows the new slot in the same burst as the frame-end clear, and
two later frame-end observations. A read-back consumer receives the whole
completed slot in one reply; the worker invalidates the frame once before
lending it.

**Parameter buffer.** The global free list has initial pages equal to maximum
pages, 16,384 (64 MiB), and zero grow pages. The backing is reserved at OPEN
today (`gpu_graphics_memory.c:229`), so growing would save no memory. The bound for
the largest consumer frames assumes 16 × 16-pixel tiles, doubled in each axis
at 4× (Mesa `pvr_arch_job_render.c:156–157`), and 128-byte control-stream
blocks (Mesa `ipf.xml:29`). A primitive block holds at most 128 primitives
(`cs_prim_total`, `ipf.xml:241`). The bound also assumes at most 24 bytes per
tile entry of a block, which no pinned source states:

- 16 full-screen batches at 720p 4× (14,400 tiles): 16 × 14,400 × 24 bytes =
  5.5 MB of entries plus 1.8 MB of first blocks, about 7.4 MB.
- 16 full-screen batches at 1080p (8,160 tiles): about 4.2 MB.
- 1,000 non-overlapping triangles at 1920 × 384: under 1 MB.

The 64 MiB maximum is about 9 times the largest of these. The admitted index
limit has no bound that fits. 65,535 indices are 21,845 triangles. Clipping
against near, far and four guard-band planes adds at most one vertex per plane,
so each triangle yields at most 7 triangles, giving 152,915 primitives and at
least 1,195 blocks. If every block reaches every tile at 720p 4×, that is
1,195 × 14,400 × 24 bytes, about 413 MB. A frame larger than the consumer frames
can therefore cause a partial render. It stays memory-safe, because both fragment
commands leave ZLS untouched and the PBE writes only the slot. It is visibly
wrong, because the final render clears colour with no load program. After each
completion the worker reads three firmware words: the geometry context's
out-of-memory and partial-render statistics (`pvr_rogue_fwif.h:693, 695`) and a
one-entry partial-render array registered with `max_rts` = 1
(`pvr_rogue_fwif.h:2049–2052`). Pinned Linux registers no array for one layer
(`pvr_hwrt.c:388–406`), and nothing yet shows that this firmware writes any of
the three for a single-layer target. Any nonzero word, and any free-list GROW
event, is unknown device state. An absent signal proves nothing until the board
item below qualifies one; until then conformance comparison is the only
detection.

**Shutdown.** The display stops issuing replays. Then, in order:

1. Stop TV0 and the DE, and observe the stop.
2. Remove the master-8 IOMMU translations of the slot extents and invalidate
   the IOMMU TLB.
3. Return every LENT slot.
4. Close the GPU. CLOSE is refused while a slot is LENT, and DISPOSE while any
   extent is imported.
5. Park the PMIC.

Steps 1 and 2 take one frame plus the HDMI stop, then about 1,800 PTE writes
and one invalidation, inside the existing 2-second HDMI stop reserve. If any
step is not observed, the display bank and the GPU lease are both retained.

**Failure classes** (section 1.8 of the performance architecture):

- Known state, no retention: an invalid batch, pipeline, texture, extent or
  state word, a full slot ring at the request deadline, or an invalid program
  set at OPEN. Each is refused before device access; the input returns to its
  producer and the session stays usable.
- Unknown device state: an uncertain kick, a missing timeline at the deadline
  (which also covers a rejected or unexecuted kick), a freshness marker still
  present after completion, any partial-render signal or GROW event, or a GPU
  MMU fault. The worker retains rings, slots, input, programs and lease.
- Display replay past its deadline, or an IOMMU fault: every slot not yet
  retired under the retire rule stays LENT, the display refuses further replays
  and MMIO, and the display bank, the imported master-8 extents and the GPU
  lease are retained. The GPU may still render into FREE slots for read-back.

## Interrupts and polling

**GPU kicks.** The pinned Linux sequence (`pvr_job.c:697–741`,
`pvr_queue.c:744–760`), which the renderer already follows: one combined kick
for the geometry command and the paired partial-render fragment command, then
the final-fragment kick, which carries the render-target cleanup reference.
The two kicks are written back to back with no KCCB return-slot wait. Ring
space comes from the firmware read offset. A rejected kick is therefore
detected only at the timeline deadline and falls in the retained class.

**Worker waits.** While GPU interrupt delivery is unqualified, the worker waits
at most one tick on its queue while a frame renders. Once board evidence shows
every completion arriving by interrupt, it blocks until the frame deadline
(section 1.3 timeout form).

**Display task.** The display task blocks on the PRESENT reply and runs each
replay as one burst. No display interrupt is bound, so it polls only near frame
boundaries. After each observed frame-end it blocks with `vTaskDelayUntil`
until the tick that starts at most 1 ms before the next predicted boundary. The
prediction is the last observed frame-end plus 16.667 ms. It then reads the
status word once per tick until it sees RCQ finish or frame-end. A missed
observation only delays retirement. Each frame costs about 3 wakes: the
PRESENT notification and two status reads around the boundary.

The status word is `RTMX_GLB_STS` of display 1, DE + `0x8144`: bit 0 frame-end,
bit 2 RCQ finish, bit 3 RCQ accept (pinned `de_top.c:131–141`). On DE352
(`de_top.c:270, 299`) one write-one-to-clear clears it (`de_top.c:1030–1042`).
The write-1-then-0 sequence at `de_top.c:1060–1068` applies only to DE210. The
frame-end clear writes 1 to bit 0 and 0 to bits 2 and 3, as
`a733_display_engine.c:196` does today, so it leaves RCQ finish and accept
untouched.

**IOMMU fault check.** The display image admits only master 8 (access mask
`0x100`), so the full fault scan is 22 reads: 18 bank registers and 4 micro
registers. In each burst that observes a frame-end, before any slot return,
the task reads the bank, L1 and L2 status of both banks and master 8's micro
status, 7 reads. It runs the full scan only when one is nonzero. A fault stops
further replays and MMIO and retains as stated above. A replay issued between
a fault and its detection is safe: master 8 is read-only and the slots stay
LENT. The full scan also runs at admission and shutdown.

## Memory placement and cache policy

| Region | GPU mapping | CPU writes | Cache maintenance per frame |
| --- | --- | --- | --- |
| Small region | 4 KiB pages, read-only, about 48 PTEs | Programs at OPEN; commands; indices and batches | Clean bytes written |
| Vertex area | One 2 MiB page, read-only | Producer | Clean ranges written |
| Texture area | 2 MiB pages, read-only | Producer, once per upload | Clean ranges written |
| Output slots | 4 KiB pages over the frame only, read-write | Two marker lines per frame | Clean markers before the kick; invalidate them after; invalidate a read-back frame once |
| Depth and stencil | None; ZLS base unmapped | None | None |
| Firmware contexts, rings, timelines | Existing non-cacheable pool | Existing | None |

Every region and slot is preceded and followed by unmapped GPU address space of
at least 2 MiB, so any GPU write outside a frame faults. Within one 2 MiB GPU
region all mappings share a page size (`pvr_gpu_vm.c:43`). Pinned Linux pegs
the device page to the host page size (`pvr_mmu.c:25–30`), so 2 MiB device pages
are unexercised by the reference driver. Writable slots therefore keep 4 KiB
pages, written once at OPEN. The read-only vertex and texture areas use 2 MiB
pages after the board item below passes. The fallback is 64 KiB pages, also
listed in `pvr_mmu.c`, with the same unmapped gaps. Slots move to 2 MiB pages
only after that item passes. The DE reads slots through system-IOMMU master 8
with 4 KiB pages (`sunxi-iommu-pgtable.c:173–199`): 450 page walks per 720p
frame, about 27,000 a second.

**Freshness markers.** Before each kick the worker writes a 64-byte marker that
includes the frame sequence number into the first and last lines of the frame,
and cleans both lines. After completion it invalidates them and requires that
the GPU overwrote both. A CPU line prefetched later stays clean, so it cannot
overwrite GPU output. Read-back sessions also keep today's full poison fill
(`pvr_render_service.c:187, 320`), so an unwritten tile shows as poison in the
host comparison. They drop it only after board evidence shows full tile
coverage. Scanout skips it.

**Barriers.** Until weaker forms are qualified in `a733_mmio.h`, the existing
`DSB SY` forms stay. The targets: plain stores for CCCB words in place of a
store-release per word; one `DMB OSHST` before the ring write offsets and one
before the KCCB doorbell.

## Synchronization across cores

The GPU worker alone writes render state, slot states, batch copies and
command pages. Producers and consumers reach it only through the service request
queue, whose critical sections order a producer's stores before the worker's
receive. Cache maintenance by virtual address to the point of coherency is
broadcast in the shareable domain, so the worker can clean bytes another core
wrote. The worker reads timelines and firmware words with acquire loads,
fragment timeline first. The interrupt handler only posts a wake message.

**Placement.** Once the multicore work admits the A76 cores, the GPU worker
runs on core 6 with GPU SPI 95 routed there, so a completion wake needs no
inter-processor interrupt (IPI). The display task does register replay and
polling, which is service work, and runs on A55 core 1. The PRESENT
notification crosses from core 6 to core 1 at one IPI per frame. Until the A76
cores are admitted both run on core 1, and USB and the service tasks stay on
core 0.

## Reuse and removal

Reused: the paired rings and timelines, the second render-target record, the
PRESENT and RELEASE tickets, the Mesa producer scripts, the TV0 RCQ replay, the
authenticated envelope, the graphics lease, free-list reconstruction, and the
read-back poison fill.

Removed:

- the draft, snapshot copy, 64 KiB upload and download slots and per-reply
  pixel output, for every draw mode;
- free-list grow dispatch and its shutdown drain, and the `initial_pages`,
  `grow_pages` and `threshold` fields of the profile-5 lease profile;
- the zero-fill and copy of the maximum-size texture and geometry areas;
- the DRAM depth attachment, its guard pages, its CPU initialization and store;
- the per-page `a733_gpu_identity_mapped` translations, which run once at OPEN;
- the per-word guard loop over the frame; unmapped neighbours confine
  overruns, and the markers detect unwritten output;
- the scanout poison fill;
- the per-vertex position check, which cannot see shader-computed positions;
  the PBE render-target bounds and the GPU MMU confine writes;
- the per-index check, covered by the three index rules;
- the RGBA8-to-RGB565 conversion and the frame copies in `apps/gpu-display`;
- the KCCB return-slot waits and the per-word store-release;
- the per-replay IOMMU scan gate and per-tick display polling between
  boundaries.

## Budgets

Baselines are counted from the integration base for a 1920 × 1080 profile-4
frame and the 1280 × 720 profile-2 display frame. They become measured
baselines when the existing harnesses gain counters; no new harness is added.

- **Frame-path emulation**, `checks/gpu-texture-runtime.py`, runs the linked
  frame path under Unicorn and gains hooks for instructions, `AT`, `DSB`,
  `DMB`, cache-maintenance calls and bytes, bytes stored by region, kicks,
  waits, wake-ups, IPIs and context switches.
- **Display replay model**, `checks/gpu-display-replay.py`, gains counters for
  task wakes, register accesses and IOMMU reads, an assertion that frame-end
  clears never set bits 2 or 3, and a slot-ring model. The model replays the
  retire rule over frame-end timestamps. It reports how long after each
  frame-end a slot becomes FREE and the largest render time that sustains
  60 Hz.
- **VM check**, `checks/pvr-gpu-vm.c`, counts leaf PTEs and checks that each
  slot's neighbours are unmapped.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| CPU bytes stored by the worker per frame | 60.2 MB at 1080p; 17.4 MB at 720p | Scanout: at most 17 KiB plus 128 bytes of markers. Read-back adds the 8.3 MB poison at 1080p until tile coverage is shown | Frame-path emulation |
| CPU cache maintenance per frame | 68.5 MB at 1080p; 21.1 MB at 720p | Bytes written plus 256 bytes for markers; a read-back frame once | Frame-path emulation |
| `AT` translations per frame | About 38,000 at 1080p, plus about 8,100 for read-back | Zero | Frame-path emulation |
| Worker tick waits caused by CPU work | About 65 at 1080p; about 20 at 720p | Zero | Frame-path emulation |
| Output check iterations per frame | 2,074,624 guard words at 1080p, plus 2,048 depth guards | Two 64-byte marker comparisons | Frame-path emulation |
| KCCB kicks and return-slot waits per frame | Two kicks, two waits | Two kicks, no wait | Frame-path emulation |
| Worker wake-ups per frame beyond interrupts | One per tick of rendering | At most two once delivery is qualified | Frame-path emulation |
| Client wake-ups per frame | One per tick of rendering | At most two | Frame-path emulation |
| Worker instructions per frame, 16 batches | Not measured | At most 100,000 | Frame-path emulation |
| GPU idle gap, completion interrupt to next kick, excluding producer time | Not measured | At most 5,000 instructions to the input return, at most 100,000 from SUBMIT to kick, at most 2 worker context switches, no tick wait | Frame-path emulation |
| `DSB` and `DMB` per frame | Not measured; two `DSB SY` per maintenance call, one per ring publish | Measured first; then two `DMB OSHST` per kick and one `DSB SY` per maintenance call | Frame-path emulation |
| Copies per input byte after transport | Three; texture every frame | At most one; texture once per upload | Frame-path emulation |
| Copies per displayed pixel byte | Three, plus two SHA-256 passes | Zero | Frame-path emulation |
| Copies per read-back byte | Three, including the USB FIFO write | One | Frame-path emulation |
| Queue round trips to read back a frame | 254 at 1080p; 114 at 720p | Two | Frame-path emulation |
| GPU interrupt to RCQ submit | Not applicable; frames are presented after CLOSE | No tick wait, at most two context switches and one IPI | Frame-path emulation |
| Display-task wakes per second while presenting | About 1,000 | At most 180 (3 per frame) | Display replay model |
| Display-task ticks per present outside device waits | About 40 | Zero | Display replay model |
| Register accesses per present | 14 replay accesses, one status read per tick of each wait, a 22-read scan before each replay | 14 replay accesses, 7 IOMMU reads, at most 2 status reads per boundary; 22-read scan only on a nonzero status | Display replay model |
| Delay from frame-end to slot return; largest render time at 60 Hz with four slots | Not modeled | At most 1.2 ms; at least 15 ms | Display replay model |
| GPU leaf PTEs per session, written at OPEN | About 9,000 at 1080p profile 2 | About 4,100 at 1080p read-back, 1,850 at 720p scanout; about 4,260 and 2,010 with 64 KiB fallback pages | VM check |
| Master-8 page walks per 720p frame | 450 | 450; the IOMMU has no larger page | VM check |
| CPU depth initialization per frame | 32 MiB plus 8 KiB at 1080p | Zero | Frame-path emulation |
| Partial-render signal reads per frame | None | Three words; a nonzero word retains | Frame-path emulation |
| Parameter-buffer backing reserved at OPEN | (maximum + 28) × 4 KiB, grown inside it | (16,384 + 28) × 4 KiB, 67.2 MB; no grow event | Frame-path emulation |
| Lease bytes, excluding the parameter buffer | 36.7 MB at 1080p profile 2 | 29.4 MB at 1080p read-back; 21.0 MB at 720p scanout | VM check |
| Shutdown, stop request to PMIC park | GPU closed before TV0 stops | Inside the 2-second HDMI stop reserve | Display replay model |

## Changes needed from other owners

- **GPU runtime** (`pvr_gpu_service.c`, `pvr_cccb.c`, `pvr_free_list.c`,
  `gpu_graphics_memory.c`, `gpu_disposal.c`):
  - held PRESENT and READ replies answered by task notification, and the two
    worker wait forms;
  - ring space from the firmware read offset, no return-slot wait, and plain
    stores for CCCB words;
  - deletion of the TEXTURE WRITE upload-chunk copy (`pvr_gpu_service.c:1020`)
    and the 64 KiB output accessor;
  - the lease at 2 MiB alignment with the VM plans above, no depth region, an
    import count, and DISPOSE refused while an extent is imported;
  - the global free list at initial = maximum with zero grow, the three profile
    fields dropped, GROW treated as unknown state, and grow dispatch and its
    shutdown drain removed;
  - the one-entry partial-render array and the three signal reads.
- **GPU checks**: `pvr-batch.c` asserts `clip_mode`, `wclampen`,
  `down_scale`, the PBE pack mode, and `zloaden` and `zstoreen` clear in both
  fragment commands; the profile-3 and profile-4 cases in `pvr-texture.c` and
  `pvr-depth-blend.c` are deleted with their code; `pvr-gpu-vm.c` gains the
  counts above.
- **Build** (`cmake/gpu.cmake` and the image files): draw-mode selection
  becomes program-set selection.
- **Display and IOMMU**:
  - a replay that ends at RCQ finish and readback, with a separate frame-end
    step of one read and one clear of bit 0;
  - the 7-read IOMMU status check, and import and removal of slot extents in
    the master-8 tables;
  - binding and qualification of the TCON TV vertical-blank interrupt, which
    removes the remaining tick poll;
  - qualification of IOMMU interrupts SPI 170 and 171, which removes the
    per-present status reads.
- **Primary agent**: route SPI 95 to core 6 and admit cores 6 and 7 for the
  graphics images. A multi-range cache-maintenance call with one trailing
  `DSB SY` is requested only if the frame-path counters show the existing
  `aarch64_device_cache` calls within the frame budget.

## Left out

- **More than one frame in flight on the GPU.** It needs a second input region
  and doubles the parameter buffer; the idle-gap row measures the cost.
- **Partial-render support.** Mesa's path needs 14.7 MB of unresolved 4× colour
  at 720p, DRAM depth and load programs.
- **4× above 1280 × 720, 8×, sRGB render targets** (a second F16 fragment
  variant per pipeline), other topologies, instancing, primitive restart,
  storage resources, optional formats, on-board mip generation and on-board
  shader compilation. No consumer uses them.
- **Double-buffered input.** Resident textures leave a few kilobytes per frame.
- **Mailbox presentation, framebuffer compression and scanout hashing.** The DE
  cannot read compressed frames, and hashing stays in the evidence image.

## Board evidence that confirms the design

- HDMI capture of a 60 Hz sequence of alternating high-contrast frames, with
  the GPU rendering while the DE scans, shows no torn or stale lines, and DE
  underflow status stays clear.
- Retirement isolation: the GPU writes a frame-unique marker into a slot's first
  and last rows as soon as the slot returns, and the capture never shows it in
  a frame presented before that return. One run uses the conservative rule and
  one a one-frame-end candidate, which would reduce scanout to three slots.
- Large pages: a texture drawn from a known pattern at a known physical offset
  inside a 2 MiB read-only page renders that pattern, and canary values placed
  just outside the page never appear in the frame. Slots move to 2 MiB pages
  only after this passes, and a second run then confirms that canaries just
  past each slot's physical run are unchanged.
- Partial-render signals: a session with a 28-page free list and a 16-batch
  full-screen frame records which of the three words or a GROW event changes.
- Full tile coverage: every conformance frame read back over poison contains
  no poison, which removes the read-back poison fill.
- Every completion arrives by interrupt before the one-tick poll sees it. No
  freshness marker survives. No GPU MMU fault occurs, so the unmapped ZLS base
  is never accessed.
- Shutdown completes inside the HDMI stop reserve and the lease returns to the
  pool.
- Frames from each feature match the host software renderer within the
  conformance tolerances. Frame times are measured under
  [validation.md](../validation.md).

## Appendix: reasons for selected decisions

**Program trust.** The board does not verify shader behavior. The envelope
admits only host-signed program sets. Before signing, the host producer decodes
every USC and PDS instruction with the pinned Mesa disassembler and refuses
store, atomic, scratch and DMA-write instructions. At OPEN the worker refuses
metadata that declares scratch memory, spills, barriers, atomics or storage
writes, an attribute past the stride, or a PDS data-segment template with a
nonzero address field. The worker writes every address in a PDS data segment
from validated batch fields. A program that wrote memory anyway could reach the
general-heap fence and static data (`0x8000000000`), the render-target state
(`0x8004000000` onward), the context-switch state and border table
(`0x8008000000`), and the output slots.

**Retire rule.** The pinned display driver issues an RCQ update and polls for
RCQ finish (`sunxi_de.c:715–775`, `de_top.c:1077–1082`) before it signals the
vertical-blank event. Neither it nor gpu-display.md establishes when the DE
stops fetching the old address, or whether the source-address readback returns
the pending or the active register. Two frame-ends after the readback cover
both cases. With one present per 16.667 ms period, slot X loaded at boundary
B(k) returns at B(k+3) plus the observation delay, and is next presented at
B(k+4). The render window is the period minus that delay and the replay burst,
which is the 15 ms budget.
