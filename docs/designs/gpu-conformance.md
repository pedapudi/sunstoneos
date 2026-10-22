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
(USC) code. The host can check the references and the data path; only board
captures establish GPU conformance.

The harness has a graphics reference built from the pinned Mesa llvmpipe
driver, a closed-form compute reference and a comparator with per-feature
tolerances. It departs from the package plan in one respect: compute uses a
closed-form reference.

## Data flow and ownership

1. **Plan.** `tools/gpu-conformance.py` holds a fixed scene list. Each scene
   builds its canonical device input with the existing builders (`vertices()`
   in `apps/gpu-render/draw.py`, `input_bytes()` in `texture.py`, the package
   input of `apps/gpu-program`). Its key is the SHA-256 of those bytes plus
   profile, state word and framebuffer size.
2. **Reference.** The reference parses the same bytes the board receives, so
   no scene is described twice.
3. **Capture.** The existing capture functions run under the board
   coordinator; the harness opens no USB device. The capturing host computes
   SHA-256 over the bytes it passes to the capture function and records it
   with frame sequence and generation. The board does not attest this hash.
4. **Comparison.** The comparator refuses a capture whose recorded hash
   differs from the plan, then compares pixels or words.

Inputs and captures are read-only. Reference frames are written once per key
under `~/.cache/sunstoneos/build/gpu-conformance/`; results go to
`~/.cache/sunstoneos/evidence/gpu-conformance-<date>/` and are summarized in
`docs/gpu-conformance.md`.

## Graphics reference

The build checks `mesa-86c7176d2dd0065555521bf8c61f208bf53c75d6.tar.gz`
against the SHA-256 in [gpu-render.md](../gpu-render.md) and extracts it into
the package build directory, so it shares no source tree with other work. It
uses the producer's clang-21 and LLVM 21.1.8 with `-Dgallium-drivers=llvmpipe
-Dvulkan-drivers=[] -Dopengl=true -Degl=enabled`, no GLX or GLES, `nice -n 10`
and `-j 6`. A stamp of archive hash, options, compiler versions and
`LP_NUM_THREADS=4` skips unchanged rebuilds. `tools/gpu-conformance/reference.c`
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
- Profile 3 uses a 32-bit float depth attachment and profile 4 a packed
  depth-stencil one, cleared to depth 1 and stencil 0 each frame. The colour
  clear is the value the profile's background program stores. Batches are
  drawn in submission order.

## Compute reference

Each of the seven kernels in `shaders/` (the fixed probe, vector add, affine,
transpose and three-point neighbour sum, with dispatch variants) has a
closed-form Python expression over the package geometry, and board words are
compared with it exactly. Every word outside the valid element range must
keep the value the driver wrote before submission: `PVR_COMPUTE_JOB_GUARD`,
or `0xa5a5a5a5` for a result word. A lane that writes beyond the valid count
therefore fails on the board. The board runs the real kernel source, so a
disagreement between source and expression also fails.

## Exact vertex placement

The plan computes window coordinates in exact rational arithmetic and refuses
a scene unless every vertex lies on a multiple of 1/16 pixel that an fp32
viewport transform and perspective division reproduce exactly. A rasterizer
with at least Vulkan's minimum of 4 subpixel bits, the figure the pinned
PowerVR driver reports, then keeps every vertex in place under any rounding
mode; llvmpipe uses 8 bits. Qualifying NDC coordinates are multiples of 2⁻⁹,
2⁻¹⁰ and 2⁻¹¹ on 64-, 128- and 256-pixel axes, 2⁻¹⁰ on 1920- and 384-pixel
axes and 1/8 on 255- and 129-pixel axes. Profile 4 uses w of 1, 2 or 4.

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
| Linear filtering | ±(1 + ⌈17Δ/2048⌉), Δ the largest channel difference among the four texels |

For linear filtering, board and reference each quantize both bilinear weights
to 2⁻⁸, so each weight differs by less than 2⁻⁸ under truncation or rounding.
The result changes by at most Δ per unit of either weight, giving Δ/128. The
fp32 texel coordinate error, below 2⁻¹² texel for textures up to 256 texels,
adds Δ/2048, and the final conversion adds 1.

Ambiguous pixels have their own acceptance sets:

- **Edge.** The pixel centre lies within 2⁻⁸ pixel of an edge, which with
  exact vertices is an exact tie decided by the implementation's tie-break
  rule. The pixel passes if it matches the reference at the same pixel or one
  of its eight neighbours, as in dEQP's position-deviation comparison.
- **Texel boundary.** With nearest filtering, the addressed texel coordinate
  lies within 2⁻⁷ texel of a boundary (2⁻⁸ for subtexel quantization plus the
  fp32 error). The reference also computes the adjacent texel on each such
  axis and accepts any of up to four candidates.
- **Depth.** None. The plan refuses a scene in which two surfaces covering one
  pixel centre differ in depth by more than zero and less than 2⁻¹⁶. Exact
  ties come only from the same vertex positions drawn twice, which Vulkan's
  invariance rules require to give identical depth, so stencil depth-fail and
  pass operations never depend on rounding.

The plan refuses a scene with more than 5% of covered pixels ambiguous. A
mismatched pixel equal to `PVR_RENDER_POISON` is reported as stale output,
pointing at completion or cache maintenance. The report counts exact,
tolerant, ambiguous-accepted and failed pixels and lists the first 16
failures.

## Board launches and their modeled time

In `apps/gpu-render/usb.c`, `gpu_application_usb` runs one `tud_task`, one
polled `dcd_int_handler` and one exchange per `vTaskDelay(1)`; the DCD is
always polled because USB0 interrupt delivery is unqualified. The pinned
TinyUSB vendor stream sends at most one 512-byte packet per transfer and
starts the next only from `tud_task`, so a packet takes one tick or, when the
poll sees its completion an iteration late, two (counted). The tick model uses
two ticks per packet, four per 64 KiB slot for READ, delivery and RELEASE, and
one per completion. In every launch image the GPU interrupt wakes the service
worker, but the client observes completion and each service reply by a
one-tick STATUS poll (section 1.3 of
[performance-architecture.md](../performance-architecture.md)).

| Launch: image, envelope | Session | Frames | Readback | Worst-case modeled time |
| --- | --- | --- | --- | --- |
| Triangle profile: `sunstone-a733-gpu-draw-single-core` | 64 × 64 | Background only; three flat triangles sharing edges; 126-vertex fan; a degenerate triangle | 64 KiB | 1.4 s |
| Profile 2: texture image, version 1 | 255 × 129, stride 256 | Six: copy, mirrored copy and three texture sizes as full-screen quads, and triangles on the 1/8 NDC lattice | 0.8 MB | 5.5 s |
| Profile 3: texture image, version 2 | 256 × 256 | Eight: three comparisons with and without depth writes, and two source-over scenes | 2.1 MB | 8.9 s |
| Profile 4: texture image, version 3 | 128 × 128 | 18 frame states: nearest with and without sRGB, and linear, each with six address-mode pairs; up to 16 batches covering depth, stencil, blend, cull and colour write | 1.2 MB | 5.5 s |
| Profile 4 maximum width: texture image, version 3 | 1920 × 384 | One frame of 1,000 textured triangles | 2.9 MB | 12.9 s, frame stream 11.7 s |
| Compute, one per kernel: program application with runtime input | Package geometry | Three inputs each | 12 KB each | 0.7 s each |

The modeled time covers uploads, submissions, completions and readback. The
plan check asserts that every frame stream is at most 15 s, half the
30-second limit in [gpu-render.md](../gpu-render.md), and that OPEN, the
launch and the one-second CLOSE allowance total at most 30 s, half the
60-second session deadline. OPEN counts at its 15-second budget until the
QEMU texture check supplies its tick count. A 1920 × 1080 frame needs 16,200
packets and 32.9 s in the worst case, so it waits until the GPU image reaches
the USB0 rate of section 3. The times are modeled until board captures record
wall time per launch. The fixed-clear image already compares every pixel with
`0xffc08020`, so its captured bytes are checked against that value and it
needs no launch.

Each launch needs its image, envelope version and existing external
prerequisites: extended-memory ownership for texture launches, the depth
allocation selected by the authenticated geometry for profiles 3 and 4, and
for compute the GPU memory, entry, recovery and ownership evidence of
[gpu-compiled-programs.md](../gpu-compiled-programs.md). A refused, failed or
uncertain capture is recorded with its stage and not retried; the driver's
rules retain every owner after uncertain completion.

## Host self-checks

`checks/gpu-conformance.py` runs these before board time is spent:

- **Closed-form scenes.** One per feature, with edges on pixel boundaries and
  samples at texel centres; Python computes the frame with integer
  arithmetic and the reference must match. Each scene separates every value
  of its state field: depth bands below, at and above the stored value give
  the eight compare operations distinct patterns, and starting stencil values
  0, 0x5a and 255 separate the eight stencil operations.
- **Mutations.** Each must fail at least one scene. Five set one reference
  render parameter: a half-pixel viewport offset, the other filter, the other
  interpolation mode, mirrored repeat for mirror clamp to edge, and skipped
  sRGB decoding. Four alter the submitted data: stride padding read as
  pixels, one exact pixel changed by 2, one compute word changed, and one
  compute word beyond the valid range overwritten.
- **Transport model.** A subclass of the model in
  `checks/gpu-texture-runtime.py` keeps its own `render()` snapshot and runs a
  255 × 129 session (three slots, the last partial) and a 128 × 128 session
  (one packet-aligned slot). It asserts that the decoded frame equals the
  snapshot's first stride × height × 4 bytes split at the stride and cropped
  to the width. This covers the linked native application, the pinned TinyUSB
  FIFO and transfer path and the host decoder. The GPU service there is a
  Python hook, so the driver's output invalidation, guard checks, padded
  snapshot copy and 64 KiB chunking are not covered. The subclass accepts one
  IN completion per loop iteration and counts iterations per slot, upload and
  completion, the one-tick-per-packet case of the tick model.
  `checks/gpu-draw-runtime.py` already checks the fixed 64 × 64 draw
  transport.
- **Driver path.** The QEMU texture check runs the native owner, including
  `drivers/gpu/pvr_render_service.c`. With the change listed at the end, it
  writes the 255 × 129 reference copy frame at the device-output boundary,
  and the harness compares the copied snapshot with the reference.

## Interrupts, memory, caches and cores

The harness changes no board source, image or configuration. Frames come from
the snapshot the driver copies after both acquired completions, output
invalidation in batches of at most sixteen pages and guard checks. Compute
output is pre-filled and cleaned before submission and invalidated after
completion. All launches use single-core GPU applications, so no cross-core
synchronization is involved.

On the host, one reference process renders every frame of a graphics launch,
including its parameter mutations, so EGL setup and shader compilation happen
once per launch. Each frame ends with `glFinish` and one `glReadPixels`, with
no polling. llvmpipe's four worker threads are the only concurrency, within
the six-job limit. The comparator runs in the same process over the frame in
memory, in one pass.

## Reuse and removal

The harness reuses the pinned Mesa archive and hash, the producer's compiler
environment, the input builders, the capture functions, the texture runtime
model, the QEMU texture check, the evidence layout and
`checks/check-index.py`. It removes nothing, because no output comparison
exists, and adds no reporting system.

## Budgets

| Metric | Baseline | Budget | Measurement |
| --- | --- | --- | --- |
| Implemented features with an independent output comparison | 0 (counted) | Every feature row in `docs/gpu-conformance.md` | Coverage table from the check |
| Mutations detected | None | All nine | Check result |
| Board launches | No plan | 12 | Plan |
| Readback for all launches | No plan | At most 8 MiB; 7.2 MB planned | Sum of frame extents |
| Ticks per 512-byte IN packet, GPU image | 1 to 2 (counted); the existing model delivers up to 8 per iteration | Plan uses 2 | Subclass counts 1 |
| Worst-case frame stream | None | At most 15 s; 11.7 s planned | Plan check |
| Worst-case session, OPEN 15 s and CLOSE 1 s included | None | At most 30 s; 28.9 s planned | Plan check |
| Board wall time per launch | Not recorded | Recorded beside the modeled time | Board evidence |
| Transport emulation time | 26.8 s for two 1920 × 1080 frames in the existing model (this host, 2026-10-22) | At most 5 s for both sessions | `time` in the report |
| Changes to board images | 0 | 0; no CMake file references an owned file | Check asserts it |
| Reference processes per run | None | At most 5 | Counted |
| Comparator time, 1920 × 384 frame | None | At most 50 ms | `clock_gettime` |
| Host check wall time, reference built | None | At most 60 s | `time` |
| Mesa rebuilds with unchanged stamp | None | 0 | Stamp |
| Ambiguous pixels per scene | None | At most 5% of covered pixels; 0 in closed-form scenes | Report |

## Left out

- **Board captures.** This work has no board access; the scheduled board
  session runs the twelve launches. A launch confirms conformance when its
  recorded input hashes match the plan, the driver's guard and timeline checks
  pass, every scene passes, and ambiguous-accepted counts and wall time are
  recorded.
- **A 1920 × 1080 launch.** Its worst-case readback exceeds the 30-second
  frame-stream limit at the GPU image's current USB rate.
- **Pixels with two inexact stages**, such as sRGB before linear filtering or
  blending over a blended value. A composed tolerance would hide an error in
  either stage; single-stage scenes cover each.
- **Host execution of compute kernels.** The closed-form reference and the
  untouched-word rule detect the same defects on the board.
- **A USC simulator and the Vulkan conformance suite.** Mesa has no USC
  simulator, and the suite needs a Vulkan driver; the harness takes only
  dEQP's position-deviation comparison.
- **Display presentation, unimplemented features and board GPU timing.**
  Lending a frame changes no pixels; the driver implements no clipping,
  mipmaps, other formats, multisampling or arbitrary shaders; section 8 of
  [goal.md](../goal.md) governs physical performance measurement.

## Changes needed outside owned files

`checks/gpu-texture.c` selects its frame size from `gpu_render_case` and fills
device output with `result_pixel()`. Driver-path coverage needs one further
case with a 255 × 129 frame whose result pixels come from a symbol the Python
runner fills, as it already fills `gpu_render_case`. The GPU driver work owns
that file. No change to code reserved for the primary agent is needed.
