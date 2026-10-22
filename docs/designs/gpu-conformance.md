# Design: GPU output conformance against a host software renderer

Status: **designed**, not implemented. No board run has produced GPU output,
so every comparison ends at host-modeled, with physical behavior pending
until scheduled board captures pass the same comparison.

The GPU driver implements a fixed clear, a colored-triangle profile, three
indexed-texture profiles (2, 3 and 4, in [gpu-render.md](../gpu-render.md))
and caller-selected compute programs
([gpu-compiled-programs.md](../gpu-compiled-programs.md)). No check compares
their output with an independent reference: the native service checks write a
modeled pattern into device output, such as `pixel ^ 0x00ffffff` in
`checks/gpu-batch.c`, and no host program executes PowerVR unified shader core
(USC) code. The host can check the references and the plan; only board
captures establish GPU conformance.

The harness has a graphics reference built from the pinned Mesa llvmpipe
driver, a closed-form compute reference and a comparator with per-feature
tolerances. It departs from the package plan in one respect: compute uses a
closed-form reference.

## Data flow and ownership

1. **Plan.** `tools/gpu-conformance.py` holds a fixed scene list. Each scene
   builds its canonical device input with the existing builders (`vertices()`
   in `apps/gpu-render/draw.py`, `input_bytes()` in `texture.py`, the package
   input of `apps/gpu-program`). The scene identity is the SHA-256 of those
   bytes plus profile, state word and framebuffer size. The plan also refuses
   scenes that the comparison cannot judge (sections below).
2. **Reference.** One reference process per launch parses the same bytes the
   board receives and renders every frame on every run, so no scene is
   described twice and no reference frame is cached.
3. **Capture.** The existing capture functions run under the board
   coordinator; the harness opens no USB device. The capturing host computes
   SHA-256 over the bytes it passes to the capture function and records it
   with frame sequence, generation, the reply result and, for a failed
   compute job, the job phase and error. The board does not attest the hash.
4. **Comparison.** The comparator refuses a capture whose recorded hash
   differs from the plan, then compares pixels or words.

Inputs and captures are read-only. Only comparison results are kept, under
`~/.cache/sunstoneos/evidence/gpu-conformance-<date>/`, and they are
summarized in `docs/gpu-conformance.md`.

## Graphics reference

The build checks `mesa-86c7176d2dd0065555521bf8c61f208bf53c75d6.tar.gz`
against the SHA-256 in [gpu-render.md](../gpu-render.md) and extracts it into
the package build directory, so it shares no source tree with other work. It
uses the producer's clang-21 and LLVM 21.1.8 with `-Dgallium-drivers=llvmpipe
-Dvulkan-drivers=[] -Dopengl=true -Degl=enabled`, no GLX or GLES, `nice -n 10`
and `-j 6`. A stamp of archive hash, options, compiler versions and
`LP_NUM_THREADS=4` skips unchanged Mesa rebuilds. `tools/gpu-conformance/reference.c`
renders through OpenGL 4.5 on surfaceless EGL; Mesa's Vulkan software driver
would need hand-written SPIR-V and uses the same rasterizer.

`glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` gives Vulkan's depth range and
maps NDC y = −1 to the first row `glReadPixels` returns, which is framebuffer
row 0 on the board. Driver state maps as follows:

- Profile 4 `VkCompareOp` n is `GL_NEVER + n`. The profile-3 depth field maps
  0 to disabled, 1 to `GL_LESS`, 2 to `GL_LEQUAL` and 3 to `GL_ALWAYS`.
- `VkStencilOp` 0–7 are `GL_KEEP`, `GL_ZERO`, `GL_REPLACE`, `GL_INCR`,
  `GL_DECR`, `GL_INVERT`, `GL_INCR_WRAP` and `GL_DECR_WRAP`.
- Blend equations 1–4 are `glBlendFuncSeparate` with (SRC_ALPHA,
  ONE_MINUS_SRC_ALPHA, ONE, ONE_MINUS_SRC_ALPHA), (ONE, ONE_MINUS_SRC_ALPHA,
  ONE, ONE_MINUS_SRC_ALPHA), (ONE, ONE, ONE, ONE) and (DST_COLOR, ZERO,
  DST_ALPHA, ZERO); equation 0 disables blending and colour-write disable
  clears `glColorMask`.
- OpenGL measures orientation with y up, so a triangle clockwise in Vulkan
  framebuffer coordinates is counter-clockwise to OpenGL. Culling mode 1 is
  `glFrontFace(GL_CCW)` with `glCullFace(GL_FRONT)`; mode 2 uses `GL_BACK`.
- Sampling uses nearest or linear filtering, the five address modes (border
  is transparent black), `GL_SRGB8_ALPHA8` for sRGB and `textureLod(…, 0)`.
  Colour and profile 2–3 coordinates use `noperspective`; profile 4 is
  perspective-correct.
- Profile 3 uses `GL_DEPTH_COMPONENT32F` and profile 4 `GL_DEPTH32F_STENCIL8`,
  matching the board's `D32_SFLOAT_S8_UINT`, cleared to depth 1 and stencil 0
  each frame. The colour clear is the value the profile's background program
  stores. Batches are drawn in submission order.

## Compute reference

Each of the seven kernels in `shaders/` (the fixed probe, vector add, affine,
transpose and three-point neighbour sum, with dispatch variants) has a
closed-form Python expression over the package geometry, and the returned
result words are compared with it exactly. The readback holds only the valid
element range, because `pvr_compute_package_valid()` requires output bytes to
equal count times element size. Writes beyond that range are detected on the
board: the READ_OUTPUT step of `drivers/gpu/pvr_compute_job.c` checks every
guard word and fails the job with `PVR_JOB_RESULT` before any readback, and
`checks/pvr-compute-job.c` covers that check. The harness reports a capture
whose recorded job error is `PVR_JOB_RESULT` as an out-of-range write. The
board runs the real kernel source, so a disagreement between source and
expression also fails.

## Plan checks

The plan works in exact rational arithmetic and refuses a scene that fails
any of these:

- **Vertex lattice.** Every window-space vertex lies on a multiple of 1/16
  pixel that an fp32 viewport transform and perspective division reproduce
  exactly. A rasterizer with at least Vulkan's minimum of 4 subpixel bits,
  the figure the pinned PowerVR driver reports, then keeps every vertex in
  place under any rounding mode. Qualifying NDC coordinates are multiples of
  2⁻⁹, 2⁻¹⁰ and 2⁻¹¹ on 64-, 128- and 256-pixel axes, 2⁻¹⁰ on 1920- and
  384-pixel axes, 2⁻⁶ on 1080-pixel axes and 1/8 on 255- and 129-pixel axes. Profile 4 uses w of 1, 2
  or 4.
- **Texture coordinates.** |u| and |v| are at most 4 at every vertex.
- **Nearest samples.** No nearest-filtered sample at a covered pixel centre
  lies within 2⁻⁷ texel of a texel boundary (2⁻⁸ for subtexel quantization
  plus the coordinate error below). Exact vertices make every texel
  coordinate an exact rational, so the reference needs one sampling path.
- **Depth.** Two surfaces covering one pixel centre differ in depth by zero or
  by at least 2⁻¹⁶. A zero difference is allowed only for the same vertex
  positions drawn twice, which Vulkan's invariance rules require to give
  identical depth, or for a constant-depth plane, against another plane or the
  clear value, whose exactness a closed-form board scene confirms.
- **Ambiguous pixels.** At most 5% of covered pixels are edge ties.
- **Sentinels.** No reference pixel lies within its tolerance of
  `PVR_RENDER_POISON` (`0x73951a6e`), and no expected compute word equals
  `0xa5a5a5a5`, so an unwritten value cannot pass.
- **Timing.** The checks in the board-launch section.

## Comparison and tolerances

Vulkan lets a float-to-UNORM8 conversion return either neighbouring value; the
reference rounds to nearest, so one conversion differs by at most 1. Blending
is assumed to run at destination precision or better with one final rounding,
and linear filtering to use the 8 subtexel bits the pinned PowerVR driver
reports. Closed-form board scenes confirm both; a linear ramp over a 2 × 1
texture across 256 pixels shows the subtexel step directly.

Each covered pixel passes through at most one inexact stage, so tolerances do
not compose. Linear-filtered pixels are opaque and not sRGB-decoded; sRGB uses
nearest filtering; a blended pixel has one blended layer over an exact value,
and later batches over it are opaque or write no colour.

| Pixel or word | Tolerance per UNORM8 channel or word |
| --- | --- |
| Clear, opaque nearest texel without sRGB, compute word | Exact |
| Interpolated colour, one blended layer, sRGB decode | ±1 |
| Linear filtering | ±(1 + ⌈9Δ/1024⌉) |

Δ is the largest channel difference between two texels that can be filtering
neighbours under the scene's address modes, computed once per texture. Board
and reference each quantize both bilinear weights to 2⁻⁸, so each weight
differs by less than 2⁻⁸, and the result changes by at most Δ per unit of
either weight, giving Δ/128. With |u| ≤ 4 and at most 256 texels, the texel
coordinate is below 1024 in magnitude; four fp32 roundings bound its error by
2⁻¹¹ texel, adding 2 × 2⁻¹¹ Δ = Δ/1024 over two axes. The final conversion
adds 1.

The profile 4 bound assumes that the hardware's perspective division adds no
more than that 2⁻¹¹ texel. A closed-form board scene confirms it: the 2 × 1
ramp drawn on a quad with w = 1 at one edge and w = 4 at the other yields each
pixel's texel coordinate to 2⁻⁸, compared with the exact rational value. If it
fails, the profile 4 launch is reported as unconfirmed and the tolerance is
revised before its other scenes count.

An edge tie is a pixel centre whose edge function is exactly zero in the
plan's arithmetic, so the implementation's tie-break rule decides coverage.
Such a pixel passes if it matches the reference at the same pixel or one of
its eight neighbours, as in dEQP's position-deviation comparison. Every other
pixel has one correct coverage under any conforming rasterizer.

A mismatched pixel equal to `PVR_RENDER_POISON` is reported as stale output,
pointing at completion or cache maintenance. The report counts exact,
tolerant, tie-accepted and failed pixels and lists the first 16 failures.

## Board launches and their modeled time

In `apps/gpu-render/usb.c`, `gpu_application_usb` runs one `tud_task`, one
polled `dcd_int_handler` and one exchange per `vTaskDelay(1)`; the DCD is
always polled because USB0 interrupt delivery is unqualified. The pinned
TinyUSB vendor stream moves at most one 512-byte packet per transfer and
starts the next only from `tud_task`, so a packet takes one tick or, when the
poll sees its completion an iteration late, two (counted from the code). The
plan reads its tick figures as parameters: currently 2 ticks per packet in
either direction, 4 per 64 KiB slot for READ, delivery and RELEASE, and 1 per
completion. In every launch image the GPU interrupt wakes the service worker,
but the client observes completion and each service reply by a one-tick
STATUS poll (section 1.3 of
[performance-architecture.md](../performance-architecture.md)).

| Launch: image, envelope | Session | Frames | Readback | Worst-case modeled time |
| --- | --- | --- | --- | --- |
| Triangle profile: `sunstone-a733-gpu-draw-single-core` | 64 × 64 | Background only; three flat triangles sharing edges; 126-vertex fan; a degenerate triangle | 64 KiB | 1.4 s |
| Profile 2: texture image, version 1 | 255 × 129, stride 256 | Six: copy, mirrored copy and three texture sizes as full-screen quads, and triangles on the 1/8 NDC lattice | 0.8 MB | 5.5 s |
| Profile 3: texture image, version 2 | 256 × 256 | Eight: three comparisons with and without depth writes, and two source-over scenes | 2.1 MB | 8.9 s |
| Profile 4: texture image, version 3 | 128 × 128 | 18 frame states: nearest with and without sRGB, and linear, each with six address-mode pairs; up to 16 batches covering depth, stencil, blend, cull and colour write | 1.2 MB | 5.5 s |
| Profile 4 maximum width: texture image, version 3 | 1920 × 384 | One frame of 1,000 non-overlapping linear-filtered triangles | 2.9 MB | 12.9 s, frame stream 11.7 s |
| Compute, one per kernel: program application with runtime input | Package geometry | Three inputs each | 12 KB each | 0.7 s each |

The modeled time covers uploads, submissions, completions and readback. The
plan check asserts each of these at half its board limit:

- every frame stream at most 15 s (limit 30 s, [gpu-render.md](../gpu-render.md));
- OPEN, the launch and the one-second CLOSE allowance at most 30 s (session
  deadline 60 s); OPEN counts at its 15-second budget until the QEMU texture
  check supplies its tick count;
- each 64 KiB slot's delivery at most 0.5 s (limit 1 s in `delivered()` of
  `apps/gpu-render/texture.c`); 128 packets at 2 ticks plus 4 is 0.26 s;
- each request's upload at most 0.5 s (limit 1 s, `receive_deadline`); a
  64 KiB payload with its header is 0.26 s.

These figures hold only while the capture host keeps a bulk IN read
outstanding throughout a stream, which the capture procedure requires. A
1920 × 1080 profile 4 frame of the same scene needs 16,200 packets and 32.9 s
at the current figures, so the plan refuses it. At the 13 ticks per 64 KiB of
the shared USB0 loop (section 3 of performance-architecture.md), its 127 slots
take about 1.7 s and the plan admits it with no other change. The fixed-clear
image already compares every pixel with `0xffc08020`, so its captured bytes
are checked against that value and it needs no launch.

Each launch needs its image, envelope version and existing external
prerequisites: extended-memory ownership for texture launches, the depth
allocation selected by the authenticated geometry for profiles 3 and 4, and
for compute the GPU memory, entry, recovery and ownership evidence of
[gpu-compiled-programs.md](../gpu-compiled-programs.md). A refused, failed or
uncertain capture is recorded with its stage and not retried; the driver's
rules retain every owner after uncertain completion. The board evidence
records wall time per launch and per slot. A launch or slot slower than its
modeled worst case is reported as a model discrepancy, and the plan's tick
parameters are updated from the measured value before the next board session.

## Host self-checks

`checks/gpu-conformance.py` runs these before board time is spent:

- **Closed-form scenes.** One per feature, with edges on pixel boundaries and
  samples at texel centres; Python computes the frame with integer
  arithmetic and the reference must match. Each scene separates every value
  of its state field: depth bands below, at and above the stored value give
  the eight compare operations distinct patterns, and starting stencil values
  0, 0x5a and 255 separate the eight stencil operations. The 255 × 129
  scenes exercise the stride crop.
- **Mutations.** Each must fail at least one scene. Five set one reference
  render parameter: a half-pixel viewport offset, the other filter, the other
  interpolation mode, mirrored repeat for mirror clamp to edge, and skipped
  sRGB decoding. Four alter a capture: stride padding read as pixels, one
  exact pixel changed by 2, one compute word changed, and a compute capture
  whose recorded job error is `PVR_JOB_RESULT`.
- **Plan refusals.** One scene violating each plan check is refused.

Transport and driver paths stay with their owners: `checks/gpu-draw-runtime.py`
and `checks/gpu-texture-runtime.py` model the draw and 1920 × 1080 texture
transports, including a partial final slot and a packet-aligned end, and
`checks/gpu-texture.c` checks the driver's output invalidation, padded
snapshot copy and 64 KiB chunking against `result_pixel()`.

## Interrupts, memory, caches and cores

The harness changes no board source, image or configuration. Frames come from
the snapshot the driver copies after both acquired completions, output
invalidation in batches of at most sixteen pages and guard checks. Compute
output is pre-filled and cleaned before submission and invalidated after
completion. All launches use single-core GPU applications, so no cross-core
synchronization is involved.

On the host, one reference process renders every frame of a launch,
including its parameter mutations, so EGL setup and shader compilation happen
once per launch. Each frame ends with `glFinish` and one `glReadPixels`, with
no polling. llvmpipe's four worker threads are the only concurrency, within
the six-job limit. The comparator runs in the same process over the frame in
memory, in one pass.

## Reuse and removal

The harness reuses the pinned Mesa archive and hash, the producer's compiler
environment, the input builders, the capture functions, the driver's compute
guard check, the evidence layout and `checks/check-index.py`. It removes
nothing, because no output comparison exists, and adds no transport model or
reporting system.

## Budgets

| Metric | Baseline | Budget | Measurement |
| --- | --- | --- | --- |
| Implemented features with an independent output comparison | 0 (counted) | Every feature row in `docs/gpu-conformance.md` | Coverage table from the check |
| Mutations detected | None | All nine | Check result |
| Board launches | No plan | 12; 13 once the 1920 × 1080 launch is admitted | Plan |
| Readback for all launches | No plan | At most 8 MiB, 7.2 MB planned; 15.5 MB with 1920 × 1080 | Sum of frame extents |
| Ticks per 512-byte packet, GPU image | 1 to 2 (counted from `apps/gpu-render/usb.c`) | Plan parameter 2 | Per-slot board wall time |
| Worst-case frame stream | None | At most 15 s; 11.7 s planned | Plan check |
| Worst-case session, OPEN 15 s and CLOSE 1 s included | None | At most 30 s; 28.9 s planned | Plan check |
| Worst-case slot delivery and request upload | None | At most 0.5 s each; 0.26 s planned | Plan check |
| Board wall time per launch and slot | Not recorded | At most the modeled worst case, else a model discrepancy | Board evidence |
| Changes to board images | 0 | 0 | Review of the diff |
| Reference processes per run | None | At most 5 | Counted |
| Comparator time, 1920 × 384 frame | None | At most 50 ms | `clock_gettime` |
| Host check wall time, Mesa built | None | At most 60 s | `time` |
| Mesa rebuilds with unchanged stamp | None | 0 | Stamp |
| Edge-tie pixels per scene | None | At most 5% of covered pixels; 0 in closed-form scenes | Report |

## Left out

- **Board captures.** This work has no board access; the scheduled board
  session runs the twelve launches. A launch confirms conformance when its
  recorded input hashes match the plan, the driver's guard and timeline checks
  pass, every scene passes, and tie-accepted counts and wall times are
  recorded.
- **A 1920 × 1080 launch at the current USB rate.** Its worst-case readback
  exceeds the 30-second frame-stream limit until the GPU image uses the shared
  USB0 loop.
- **Pixels with two inexact stages**, such as sRGB before linear filtering or
  blending over a blended value. A composed tolerance would hide an error in
  either stage; single-stage scenes cover each.
- **Host execution of compute kernels.** The closed-form reference covers the
  valid range, and the driver's guard check fails out-of-range writes before
  readback.
- **A reference frame cache.** One reference process renders a launch's
  frames well within the 60-second check budget, and a cache would need
  invalidation on Mesa, reference and state-mapping changes.
- **A USC simulator and the Vulkan conformance suite.** Mesa has no USC
  simulator, and the suite needs a Vulkan driver; the harness takes only
  dEQP's position-deviation comparison.
- **Display presentation, unimplemented features and board GPU timing.**
  Lending a frame changes no pixels; the driver implements no clipping,
  mipmaps, other formats, multisampling or arbitrary shaders; section 8 of
  [goal.md](../goal.md) governs physical performance measurement.

## Changes needed outside owned files

No change to code reserved for the primary agent is needed.

- **GPU image on the shared USB0 loop.** `apps/gpu-render/usb.c` moves to the
  loop in `apps/board/usb0_service.h` and meets the section 3 figure of 13
  ticks per 64 KiB. Owner: the USB0 evidence channel work. The plan then
  admits the 1920 × 1080 launch from its updated tick parameters.
- **Compute job error in the reply.** The program application's failure
  reply carries the service's `phase` and `compute_job_error`, which
  `drivers/gpu/pvr_gpu_service.c` already fills, so a capture can record
  `PVR_JOB_RESULT`. Owner: the GPU compiled-programs work.
- **255 × 129 driver case.** `checks/gpu-texture.c` gains one
  `gpu_render_case` with a 255 × 129 frame, using `result_pixel()` and the
  existing per-word assertion. Owner: the GPU driver work.
- **Short final packet.** If that coverage is wanted,
  `checks/gpu-texture-runtime.py` takes a frame-size argument and counts
  loop iterations per slot, and runs at 255 × 129. Owner: the texture
  transport work.
