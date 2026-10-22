# Design: GPU graphics features and zero-copy presentation

Status: **design**. No code implements it yet. Each item ends at implemented
and host-modeled; physical behavior stays pending until the board items at the
end pass.

Graphics profile 5 extends the retained renderer of
[gpu-render.md](../gpu-render.md). It adds trusted shader programs compiled on
the host by the pinned Mesa compiler, general blend factors and equations,
hardware clipping, mipmaps, the B5G6R5 texture format and 4× multisampling
resolved on chip. The display engine (DE) scans the rendered frame in place,
with one ownership hand-off and no copy. Draw modes 0 to 4 become profile-5
program sets, and the change that lands profile 5 deletes their per-frame code
from `pvr_render_service.c`. The images used for the first physical GPU
qualification stay pinned by the commit and SHA-256 recorded in gpu-render.md
and [gpu-display.md](../gpu-display.md).

## Consumer scenes set every limit

`checks/gpu-render-scenes.py` defines five scenes. The emulation checks drive
them, and the gpu-conformance package compares them with host Mesa software
rendering. Every limit below is the largest value a scene uses.

| Scene | Output | Samples, clipping | Pipelines | Textures | Indices | Batches |
| --- | --- | --- | --- | --- | --- | --- |
| Display demo (`apps/gpu-display`) | 1280 × 720 B5G6R5 scanout | 1×, off | 1 | 1 RGBA8 | 6 | 1 |
| Profile 2–4 evidence frames (`apps/gpu-render`) | 1920 × 1080 RGBA8 read-back | 1×, off | 6: five blend equations and colour writes off | 1 RGBA8 or sRGB, up to 1920 × 1080 | 65,535 | 16 |
| Blend factors | 1920 × 1080 RGBA8 read-back | 1×, off | 8 | 1 RGBA8 | 96 | 16 |
| Clipping and 4× | 1280 × 720 scanout and read-back | 4×, on | 2 | 1 RGBA8 | 3,000: 1,000 triangles crossing the near and side planes | 16 |
| Mipmaps and formats | 1280 × 720 RGBA8 read-back | 1×, off | 2 | RGBA8 1024² with 11 levels; B5G6R5 512² with 10 levels | 12 | 2 |

Limits: 8 pipelines per program set, 2 textures per session, formats R8G8B8A8
UNORM (with sRGB decode) and B5G6R5 UNORM, 65,536 vertices of at most 32
bytes, 16 batches, and 4× only up to 1280 × 720.

## Data flow and ownership

The graphics owner's write-back lease is fixed at OPEN and 2 MiB aligned:

- **Small region**, 2 MiB: unified shader core (USC) programs up to 32 KiB,
  programmable data sequencer (PDS) programs up to 8 KiB, one end-of-tile
  program per output slot, command pages, the 128 KiB index area and the batch
  table.
- **Vertex area**, 2 MiB. **Texture area**, 8 MiB, which holds the 1920 ×
  1080 RGBA8 texture.
- **Output slots**: two read-back slots, or four scanout slots. Scanout slots
  are physically contiguous with the existing padded frame stride, so the DE
  ring code (`replay_shape`, `replay_words`) applies unchanged.

Depth and stencil live only in on-chip tile memory: the background object
clears them, both fragment commands clear `zloaden` and `zstoreen`, and the ZLS
base points at an unmapped GPU address. The pixel back end (PBE) resolves 4×
colour on chip (`down_scale`). Mesa accumulates both output formats as U8
(`pvr_arch_formats.c:199, 211`), so one fragment program serves both.

**Program sets.** The authenticated envelope carries one program set. Each
pipeline is a vertex and fragment program pair with its attribute layout, up
to 128 uniform bytes, texture slots, register counts and clip mode; blends are
compiled into fragment variants. At OPEN the worker validates the metadata,
copies the programs into the small region, builds the end-of-tile programs and
cleans the region once. The appendix gives the trust argument.

**Input lending.** The index, batch, vertex and texture areas are lent to the
producer while CPU-owned; it writes each byte once, in place, and SUBMIT returns
them. The worker copies each batch record into its own memory, validates and
encodes from the copy, checks texture extents including the mip chain, and
cleans the written ranges. Indices need no per-index check: they are 16-bit,
there is no base-vertex field and the stride bounds every attribute, so every
index addresses the 2 MiB vertex area, which an unmapped GPU gap follows.

**Parameter-buffer bound.** The free list is fixed at 16,384 pages (64 MiB),
with initial pages equal to maximum pages and no growth, because its backing
is reserved at OPEN today. Admission keeps every frame inside it. Let T be the
tile count (16 × 16 pixels, each axis doubled at 4×), E the primitives per
triangle (7 with clipping against six planes, 1 without) and N ≤ 16 the batch
count. A block holds 128 primitives (`ipf.xml:241`), so a frame of P triangles
uses at most B = ⌈E·P/128⌉ + N blocks.
The design requires B·T·24 + N·T·8 ≤ 48 MiB. The 24-byte tile entry and 8-byte
first-block term come from no pinned source. The other 16 MiB covers
transformed vertex data (at most about 15 KiB per block) and error in those two
figures. At OPEN the worker turns the program set's sample count and clip
mode and the output size into an index limit. SUBMIT refuses a frame whose
batch index counts sum past that limit, before device access:

- 1× without clipping, at 720p or 1080p: 65,535, the index area's capacity.
- 720p 4× with clipping: 6,801 (2,267 triangles), above the 3,000 the scene uses.
- 1080p 1× with clipping: 12,891.

As a backstop the worker reads the geometry context's out-of-memory and
partial-render statistics after each frame (`pvr_rogue_fwif.h:693, 695`).
Either counter nonzero, or a GROW event, is unknown device state. A board item
measures actual page use.

**Output slots.** Only the worker writes slot state: FREE, then RENDERING,
then COMPLETE once both timelines reached the frame and both freshness markers
were overwritten, then LENT under one PRESENT or READ ticket. Returning the
ticket makes the slot FREE.

**Present and retire.** The display task holds a PRESENT request open; the
worker answers it with a task notification when a frame reaches COMPLETE. The
display task runs the TV0 register command queue (RCQ) replay for that slot.
It returns the previous slot only after RCQ finish, a source-address readback
showing the new slot in the same burst as the frame-end clear, and two later
frame-ends (appendix). A read-back consumer receives the completed slot in one
reply after one invalidation.

**RCQ pages.** The four RCQ command pages stay in the noncacheable display
bank, which shrinks to the 32 KiB IOMMU tables, a 4 KiB guard and these pages.
The display owner writes and validates them once at admission
(`a733_de_ring_build`, `a733_de_ring_valid`). The master-8 IOVA places them
directly after the slots, as `replay_words` requires. No GPU mapping covers the
display bank, and the VM check asserts this.

**Master-8 tables.** The display owner builds the tables once, after GPU OPEN
reports the scanout extents and before IOMMU admission. They hold two
read-only spans, one for the slots and one for the command pages, inside two
4 MiB windows. The display IOMMU profile admits no table change or release
(`a733_iommu.h`, `a733_iommu_init_display`), so the tables, the scanout extents
and the display bank stay owned until PMIC park.

**Shutdown.** The display task stops issuing replays. It stops TV0 and the DE
and observes the stop. It then returns every LENT slot, and no engine reads
them after that. The worker closes the GPU; CLOSE is refused while a slot is
LENT. The PMIC parks last. Any unobserved step retains the display bank and the
GPU lease.

**Failure classes** (performance architecture, section 1.8):

- Known state, no retention: an invalid program set, batch, texture, extent or
  state word; a frame over the index limit; a full slot ring at the request
  deadline. Each is refused before device access, and the input returns to its
  producer.
- Unknown device state: an uncertain kick, a timeline missing at its deadline,
  a surviving freshness marker, a nonzero statistics counter, a GROW event or a
  GPU MMU fault. The worker retains rings, slots, input, programs and lease.
- A display replay past its deadline, or an IOMMU fault: unretired slots stay
  LENT, the display refuses further replays and MMIO, and the display bank,
  scanout extents and GPU lease are retained. The GPU may still render into
  FREE slots for read-back.

## Interrupts and polling

**GPU kicks** follow the pinned Linux sequence (`pvr_job.c:697–741`,
`pvr_queue.c:744–760`): one combined geometry and partial-render-fragment kick,
then the final-fragment kick, back to back. Ring space comes from the firmware
read offset, with no KCCB return-slot wait, so a rejected kick shows up only at
the timeline deadline and is retained.

**Worker waits.** Until GPU interrupt delivery is qualified, the worker waits
at most one tick on its queue while a frame renders. Afterwards it blocks until
the frame deadline.

**Display task.** No display interrupt is bound. After each frame-end the task
blocks with `vTaskDelayUntil` until 1 ms before the next predicted boundary,
then reads `RTMX_GLB_STS` (DE + `0x8144`; bit 0 frame-end, bit 2 RCQ finish)
once per tick until it sees the event. The frame-end clear writes 1 to bit 0
only, as `a733_display_engine.c:196` does today. In each frame-end burst,
before any slot return, the task runs the existing 22-read fault scan
(`a733_iommu_scan`) to completion. A fault stops replays and MMIO. A replay
issued before detection is safe because master 8 is read-only.

## Memory placement and cache policy

| Region | GPU page size; PTEs at 4 KiB | CPU writes | Cache maintenance per frame |
| --- | --- | --- | --- |
| Small region | 4 KiB, read-only; 48 | Programs at OPEN; commands, indices, batches | Clean bytes written |
| Vertex area | 4 KiB until the large-page item passes; 512 | Producer | Clean ranges written |
| Texture area | 4 KiB until the large-page item passes; 2,048 | Producer, per upload | Clean ranges written |
| Output slots | 4 KiB, read-write, frame pages only; 450 (720p), 2,025 (1080p) | Two marker lines | Clean markers before the kick, invalidate after; invalidate a read-back frame once |
| Parameter buffer | 4 KiB until the large-page item passes; 16,412 | None | None |
| Depth and stencil | None | None | None |
| Firmware records, rings, timelines | Existing non-cacheable pool | Existing | None |
| RCQ pages | Not GPU-mapped; display bank | Once at admission | None (non-cacheable) |

Every region and slot has at least 2 MiB of unmapped GPU address space on each
side, so any GPU write outside it faults; all mappings in one 2 MiB GPU region
share a page size (`pvr_gpu_vm.c:43`). Pinned Linux uses the host page size
for device pages (`pvr_mmu.c:25–30`), and the board has exercised only 4 KiB,
so 4 KiB is the baseline. One board item qualifies 64 KiB and later 2 MiB pages
for the vertex, texture and parameter-buffer regions. Slots keep 4 KiB because
neither frame size is a multiple of a larger page, and a larger page would
expose a neighbouring slot to GPU writes. Master 8 reads slots through 4 KiB
IOMMU pages, the only size it has: 450 walks per 720p frame.

**Freshness markers.** Before each kick the worker writes a 64-byte marker with
the frame sequence number into the first and last lines of the frame and cleans
them; after completion it invalidates them and requires both overwritten.
Read-back sessions keep today's poison fill (`pvr_render_service.c:187, 320`)
until the tile-coverage board item passes; scanout skips it.

**Barriers.** The existing `DSB SY` forms stay until `include/a733_mmio.h` gains
`DMB OSHST`. The targets are plain stores for CCCB words, one `DMB OSHST`
before the ring write offsets and one before the KCCB doorbell, the Linux
`writel` ordering. The header has no package owner in this tranche, so the
primary agent schedules the change, and the kick board item below qualifies it.

## Synchronization across cores

The worker alone writes render state, slot state, batch copies and command
pages. Producers and consumers reach it through the service request queue,
whose critical sections order a producer's stores before the worker's receive.
Cache maintenance by address is broadcast in the shareable domain, so the
worker can clean bytes that another core wrote. The worker reads timelines and
statistics with acquire loads. The interrupt handler only posts a wake.

**Placement.** Once the A76 cores are admitted, the worker runs on core 6 with
GPU SPI 95 routed there, and the display image's application task, the
producer, runs on core 7 (default 7). The display task does register replay and
polling, which is service work, so it runs on A55 core 1. The USB task stays on
core 0 and remains the producer in the read-back image, because its rate is set
by the transport. Until the A76 cores are admitted, the worker, application and
display tasks run on core 1. A presented frame costs at most four
inter-processor interrupts (IPIs): SUBMIT from core 7, the input-return reply to
core 7, the PRESENT notification to core 1 and the RELEASE to core 6.

## Reuse and removal

Reused: paired rings and timelines, the second render-target record, PRESENT
and RELEASE tickets, the Mesa producer scripts, the TV0 RCQ replay and ring
builder, the 22-read IOMMU scan, the authenticated envelope, the graphics
lease, free-list reconstruction and the read-back poison fill.

Removed: the draft, snapshot, 64 KiB upload and download copies; free-list
growth and its shutdown drain; zero-fill and copy of the texture and geometry
areas; the DRAM depth attachment and its CPU initialization; per-page `AT`
translations after OPEN; the per-word frame guard loop; the scanout poison;
the per-vertex position and per-index checks; RGBA8-to-RGB565 conversion and
frame copies in `apps/gpu-display`, with the display bank's two frames; KCCB
return-slot waits and per-word store-release; per-tick display polling between
boundaries; ETC2 and EAC formats.

## Budgets

Baselines are counted from the integration base for a 1920 × 1080 profile-4
frame and the 1280 × 720 display frame. Three existing harnesses gain counters:
the frame-path emulation `checks/gpu-texture-runtime.py` (Unicorn), the display
replay model `checks/gpu-display-replay.py`, and the VM check
`checks/pvr-gpu-vm.c`.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| CPU bytes stored by the worker per frame | 60.2 MB at 1080p; 17.4 MB at 720p | Scanout 17 KiB plus 128 marker bytes; read-back adds 8.3 MB poison at 1080p until coverage passes | Frame-path emulation |
| CPU cache maintenance per frame | 68.5 MB; 21.1 MB | Bytes written plus 256 marker bytes; a read-back frame once | Frame-path emulation |
| `AT` translations per frame | About 38,000, plus 8,100 for read-back | Zero | Frame-path emulation |
| Output check iterations per frame | 2,074,624 guard words plus 2,048 depth guards | Two marker comparisons, two statistics words | Frame-path emulation |
| Copies per input, displayed and read-back byte | 3 (texture every frame); 3 plus two SHA-256 passes; 3 | 1 (texture per upload); 0; 1 | Frame-path emulation |
| Queue round trips per read-back frame | 254 at 1080p; 114 at 720p | 2 | Frame-path emulation |
| Worker instructions per 16-batch frame; completion to input return | Not measured | 100,000; 5,000, no tick wait, 2 context switches | Frame-path emulation |
| Worker and client wake-ups per frame beyond interrupts | One per tick of rendering | 2 each | Frame-path emulation |
| KCCB kicks and return-slot waits | 2 and 2 | 2 and 0 | Frame-path emulation |
| `DSB` and `DMB` per frame | Not measured | Measured first; then 2 `DMB OSHST` per kick | Frame-path emulation |
| IPIs per presented frame | Not applicable; frames are presented after CLOSE | 4 | Frame-path emulation |
| GPU interrupt to RCQ submit | Not applicable | No tick wait, 2 context switches, 1 IPI | Frame-path emulation |
| Display wakes per second; register accesses per present | About 1,000; 14 replay, 1 status read per tick, 22-read scan per replay | 180; 14 replay, 22-read scan, 2 status reads per boundary | Display replay model |
| Frame-end to slot return; largest render time at 60 Hz, four slots | Not modeled | 1.2 ms; 15 ms | Display replay model |
| GPU leaf PTEs per session, excluding the parameter buffer | About 9,000 at 1080p profile 2 | 6,660 at 1080p read-back, 4,410 at 720p scanout; 4,260 and 2,010 with 64 KiB pages; 4,100 and 1,850 with 2 MiB pages | VM check |
| Parameter buffer | (maximum + 28) × 4 KiB, grown inside it | 16,412 PTEs, 67.2 MB at OPEN, no growth | VM check |
| Indices admitted per frame | 65,535 at every configuration | Formula above; every scene fits | Frame-path emulation |
| Lease bytes excluding the parameter buffer; display bank | 36.7 MB at 1080p profile 2; 4 MiB | 29.2 MB at 1080p read-back, 20.0 MB at 720p scanout; 52 KiB | VM check |
| Shutdown, stop request to PMIC park | GPU closed before TV0 stops | Inside the 2-second HDMI stop reserve | Display replay model |

Targets are maxima, except the largest render time, which is a minimum.

## Changes needed from other owners

- **GPU runtime** (`pvr_gpu_service.c`, `pvr_cccb.c`, `pvr_free_list.c`,
  `platform/a733/gpu_graphics_memory.c`, `gpu_disposal.c`, `checks/pvr-*`):
  PRESENT and READ replies by task notification and the two worker waits; ring
  space from the firmware read offset and plain CCCB stores; deletion of the
  TEXTURE WRITE copy (`pvr_gpu_service.c:1020`) and the 64 KiB output accessor;
  the 2 MiB-aligned lease with the VM plans above, no depth region and DISPOSE
  refused while an extent is imported; a fixed free list with growth removed
  and GROW as unknown state; the two statistics reads; `pvr-batch.c` assertions
  for `clip_mode`, `down_scale`, the PBE pack mode and clear `zloaden` and
  `zstoreen`; the PTE and display-bank assertions in `pvr-gpu-vm.c`.
- **Display** (`a733_display_engine.c`): a replay that ends at RCQ finish and
  readback, with a separate frame-end step; ring and tables built after GPU
  OPEN; binding of the TCON TV vertical-blank interrupt, which removes the
  remaining tick poll.
- **IOMMU**: qualification of SPI 170 and 171, which removes the per-present
  scan. Table building needs no change.
- **gpu-conformance**: comparison of the five scenes.
- **Build** (`cmake/gpu.cmake`): draw-mode selection becomes program-set
  selection.
- **Primary agent**: route SPI 95 to core 6 and admit cores 6 and 7 for the
  graphics images; qualify the cross-cluster task-notification wake between the
  A76 and A55 cores, whose latency counts in the interrupt-to-RCQ budget; add
  `DMB OSHST` to `a733_mmio.h`. A multi-range cache-maintenance call with one
  trailing `DSB SY` is needed only if the frame-path emulation counts more than
  16 `DSB SY` from `aarch64_device_cache` per scanout frame.

## Left out

- **More than one frame on the GPU.** Overlap costs a second input region of
  about 4 MiB and splits the one free list between two frames, which halves the
  index limits. A board item measures geometry and fragment time per scene to
  show what serializing them costs.
- **Partial-render support.** Mesa's path needs 14.7 MB of unresolved 4× colour
  at 720p, DRAM depth and load programs. The admission bound makes it
  unnecessary for the scenes.
- **Compressed formats, sRGB render targets, 4× above 720p, 8×, other
  topologies, instancing, primitive restart, storage resources, on-board mip
  generation and on-board shader compilation.** No scene uses them.
- **Double-buffered input.** Resident textures leave a few kilobytes per frame.
- **Mailbox presentation, framebuffer compression, scanout hashing and IOMMU
  table removal at shutdown.** The DE cannot read compressed frames, hashing
  stays in the evidence image, and the image ends at PMIC park.

## Board evidence that confirms the design

- HDMI capture at 60 Hz of alternating high-contrast frames, rendered while the
  DE scans, shows no torn or stale lines, and DE underflow status stays clear.
- Retirement isolation: the GPU writes a frame-unique marker into a returned
  slot's first and last rows, and the capture never shows it in an earlier
  frame. A second run tests a one-frame-end rule, which would allow three slots.
- Large pages, one size per run, 64 KiB first: a pattern at a known physical
  offset renders correctly, and canaries just outside each page stay unchanged.
- Parameter buffer: the free-list allocated page count
  (`pvr_rogue_fwif.h:2060`), read at geometry completion for each scene's
  largest admitted frame, stays inside the bound, and both statistics stay zero.
  A larger count shrinks the index limits.
- Geometry and fragment time per scene frame, from counter samples at the two
  timeline interrupts.
- Kick ordering: a long run of kicks with `DMB OSHST` before the doorbell
  overwrites every marker and records no firmware command fault.
- Full tile coverage: no read-back frame contains poison, which removes the
  read-back poison fill.
- Every completion arrives by interrupt, no marker survives, no GPU MMU fault
  occurs, shutdown fits the HDMI stop reserve, and the scenes match the host
  renderer within the conformance tolerances. Frame times follow
  [validation.md](../validation.md).

## Appendix: reasons for selected decisions

**Program trust.** The board does not verify shader behavior, so the envelope
admits only host-signed program sets. Before signing, the host decodes every
USC and PDS instruction with the pinned Mesa disassembler and refuses store,
atomic, scratch and DMA-write instructions. At OPEN the worker refuses metadata
that declares scratch, spills, barriers, atomics or storage writes, or an
attribute past the stride, and a PDS data-segment template with a nonzero
address field. The worker writes every PDS address from validated fields. A
program that wrote memory anyway could reach the fence and static data
(`0x8000000000`), render-target state (`0x8004000000` onward), the
context-switch state and border table (`0x8008000000`) and the output slots.

**Retire rule.** The pinned display driver polls for RCQ finish before it
signals vertical blank (`sunxi_de.c:715–775`, `de_top.c:1077–1082`). Neither
source establishes when the DE stops fetching the old address, or whether the
source-address readback returns the pending or the active register. Two
frame-ends after the readback cover both cases. With one present per
16.667 ms, a slot loaded at boundary k returns at boundary k + 3 plus the
observation delay and is presented again at k + 4. The render window is the
period minus that delay and the replay burst, which gives the 15 ms budget.
