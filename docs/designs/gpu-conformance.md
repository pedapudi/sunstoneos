# Design: GPU output conformance against a host software renderer

Status: **host-tested** for the harness and its self-checks; **open** for board
captures, which no board has yet been compared against.

The GPU driver implements a fixed clear, a colored-triangle profile, three
indexed-texture profiles (2, 3 and 4, in [gpu-render.md](../gpu-render.md))
and caller-selected compute programs
([gpu-compiled-programs.md](../gpu-compiled-programs.md)). The native service
checks write a modeled pattern into device output, such as `pixel ^ 0x00ffffff`,
and no host program executes PowerVR unified shader
core (USC) code. Only board captures establish GPU conformance. The harness
compares them with a graphics reference built from the pinned Mesa llvmpipe
driver and a closed-form compute reference, using per-feature tolerances.

The harness is `tools/gpu-conformance.py` with the reference renderer
`tools/gpu-conformance/reference.c`; its host check is
`checks/gpu-conformance.py`. It changes no board source, image or
configuration, and it opens no USB device.

## Data flow and ownership

1. **Plan.** `tools/gpu-conformance.py` holds a fixed scene list. Each scene
   builds its canonical device input with the existing builders (`vertices()`
   in `apps/gpu-render/draw.py`, `input_bytes()` in `texture.py`, the package
   input of `apps/gpu-program`). The plan records the SHA-256 of those bytes
   and the frame's state word, which together identify the scene within its
   launch, and refuses scenes the comparison cannot judge.
2. **Reference.** One reference process per launch parses the same bytes the
   board receives and renders every frame on every run. No scene is described
   twice and no reference frame is cached.
3. **Capture.** The capture functions run under the board coordinator. The
   capturing host hashes the bytes it passes to them and records the hash with
   the state word, the reply's frame sequence and result, the input generation
   for texture launches (the triangle protocol has none) and, for a failed
   compute job, the job phase and error. The board does not attest the hash.
4. **Comparison.** The comparator refuses a capture whose recorded hash or
   state differs from the plan, whose sequences do not advance by one per
   frame or whose texture frames lack a generation. It records a frame with a
   failed reply result, then compares pixels or words.

Inputs and captures are read-only. Only comparison results are kept, under
`~/.cache/sunstoneos/evidence/gpu-conformance-<date>/`, and they are summarized
in `docs/gpu-conformance.md`.

## Graphics reference

The build extracts `mesa-86c7176d2dd0065555521bf8c61f208bf53c75d6.tar.gz`,
checked against the SHA-256 in [gpu-render.md](../gpu-render.md), into the
package build directory. It uses the producer's clang-21 and LLVM 21.1.8 with
`-Dgallium-drivers=llvmpipe -Dvulkan-drivers=[] -Dopengl=true -Degl=enabled`,
no GLX or GLES, `nice -n 10` and `-j 6`. A stamp of archive hash, options,
compiler versions and `LP_NUM_THREADS=4` skips unchanged rebuilds. The
reference renders through OpenGL 4.5 on surfaceless EGL; Mesa's Vulkan software
driver would need hand-written SPIR-V and uses the same rasterizer.

`glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` gives Vulkan's depth range and
maps NDC y = -1 to the first row `glReadPixels` returns, which is framebuffer
row 0 on the board. Driver state maps as follows:

- Profile 4 `VkCompareOp` n is `GL_NEVER + n`. The profile 3 depth field maps
  0 to disabled, 1 to `GL_LESS`, 2 to `GL_LEQUAL` and 3 to `GL_ALWAYS`.
- `VkStencilOp` 0-7 are `GL_KEEP`, `GL_ZERO`, `GL_REPLACE`, `GL_INCR`,
  `GL_DECR`, `GL_INVERT`, `GL_INCR_WRAP` and `GL_DECR_WRAP`.
- Blend equations 1-4 are `glBlendFuncSeparate` with (SRC_ALPHA,
  ONE_MINUS_SRC_ALPHA, ONE, ONE_MINUS_SRC_ALPHA), (ONE, ONE_MINUS_SRC_ALPHA,
  ONE, ONE_MINUS_SRC_ALPHA), (ONE, ONE, ONE, ONE) and (DST_COLOR, ZERO,
  DST_ALPHA, ZERO); equation 0 disables blending and colour-write disable
  clears `glColorMask`.
- OpenGL measures orientation with y up, so a triangle clockwise in Vulkan
  framebuffer coordinates is counter-clockwise to OpenGL. Culling mode 1 is
  `glFrontFace(GL_CCW)` with `glCullFace(GL_FRONT)`; mode 2 uses `GL_BACK`.
- Sampling uses nearest or linear filtering, the five address modes (border is
  transparent black), `GL_SRGB8_ALPHA8` for sRGB and `textureLod(..., 0)`.
  Colour and profile 2-3 coordinates use `noperspective`; profile 4 is
  perspective-correct.
- Profile 3 uses `GL_DEPTH_COMPONENT32F` and profile 4 `GL_DEPTH32F_STENCIL8`,
  matching the board's `D32_SFLOAT_S8_UINT`, cleared to depth 1 and stencil 0
  each frame. The colour clear is the value the profile's background program
  stores. Batches are drawn in submission order.

## Compute reference

Each of the seven kernels in `shaders/` (the fixed probe, vector add, affine,
transpose and three-point neighbour sum, with dispatch variants) has a
closed-form Python expression over the package geometry. The returned result
words are compared with it exactly. The readback holds only the valid element
range, because `pvr_compute_package_valid()` requires output bytes to equal
count times element size. The READ_OUTPUT step of
`drivers/gpu/pvr_compute_job.c` checks every guard word and fails the job with
`PVR_JOB_RESULT` before any readback; `checks/pvr-compute-job.c` covers that
check, and the harness reports a capture whose recorded job error is
`PVR_JOB_RESULT` as an out-of-range write. The board runs the real kernel
source, so a disagreement between source and expression also fails.

## Plan checks

The plan works in exact rational arithmetic and refuses a scene that fails any
of these:

- **Vertex lattice.** Every window-space vertex lies on a multiple of 1/16
  pixel that an fp32 viewport transform and perspective division reproduce
  exactly. A rasterizer with at least Vulkan's minimum of 4 subpixel bits, the
  figure the pinned PowerVR driver reports, then keeps every vertex in place
  under any rounding mode. Qualifying NDC coordinates are multiples of 2^-9,
  2^-10 and 2^-11 on 64-, 128- and 256-pixel axes, 2^-10 on 1920- and
  384-pixel axes, 2^-6 on 1080-pixel axes and 1/8 on 255- and 129-pixel axes.
  Profile 4 uses w of 1, 2 or 4.
- **Texture coordinates.** |u| and |v| are at most 4 at every vertex.
- **Nearest samples.** No nearest-filtered sample at a covered pixel centre
  lies within 2^-7 texel of a texel boundary (2^-8 for subtexel quantization
  plus the coordinate error below). The 2^-8 figure is an assumption about the
  hardware; if the board samples a different texel, the comparison fails.
- **Depth.** Two surfaces covering one pixel centre differ in depth by zero or
  by at least 2^-16. A zero difference is allowed only for the same vertex
  positions drawn twice, which Vulkan's invariance rules require to give
  identical depth, or for a constant-depth plane, against another plane or the
  clear value, whose exactness a closed-form board scene confirms.
- **Ambiguous pixels.** At most 5% of covered pixels are edge ties.
- **Sentinels.** No reference pixel lies within its tolerance of
  `PVR_RENDER_POISON` (`0x73951a6e`), and no expected compute word equals
  `0xa5a5a5a5`, so an unwritten value cannot pass.
- **Timing.** The checks in the board-launch section.

## Comparison and tolerances

Vulkan lets a float-to-UNORM8 conversion return either neighbouring value, so a
board conversion lies less than 1 from the value it converts. Blending is
assumed to run at destination precision or better with one final rounding.

Each covered pixel passes through at most one inexact stage, so tolerances do
not compose. Linear-filtered pixels are opaque and not sRGB-decoded; sRGB uses
nearest filtering; a blended pixel has one blended layer over an exact value,
and later batches over it are opaque or write no colour.

| Pixel or word | Tolerance per UNORM8 channel or word |
| --- | --- |
| Clear, opaque nearest texel without sRGB, compute word | Exact |
| Interpolated colour, one blended layer, sRGB decode | +-1 |
| Linear filtering | +-(1 + ceil(2D(e_b + e_r) + 1/2)): 9 in the profile 4 linear frames, 2 in the 1920 x 384 frame |

D is the largest channel difference between two texels that can be filtering
neighbours under the scene's address modes, computed once per texture. A
bilinear result changes by at most D per unit change of either axis's filter
weight, so weights that differ by at most d on each axis give results within
2dD. The per-axis bound d is e_b + e_r:

- e_b, the board's weight error, is what a passing confirmation scene admits:
  just under 2^-8 + 2^-11 + 2/255, about 3.1 x 2^-8.
- e_r, llvmpipe's weight error, is at most 2^-9 + 2^-11. When both address
  modes are repeat or clamp to edge, llvmpipe filters 8-bit texels on an
  integer path that rounds the texel coordinate to 1/256
  (`lp_bld_sample_aos.c` in the pinned source); otherwise it filters in fp32.
  With |u| <= 4 and at most 256 texels, the texel coordinate is below 1024 in
  magnitude, and four fp32 roundings bound its error by 2^-11 texel.

The board's final conversion adds less than 1. The board may also round its two
inner lerps (the vertical interpolations between texel rows) to 8 bits, as
llvmpipe does; that adds up to 1/2. The confirmation scenes cannot show this
rounding, because texels 0 and 8, and texels 1 and 9, are equal pairs. The
integer channels therefore differ by less than 2 + 2dD + 1/2, at most
1 + ceil(2dD + 1/2). The 2dD term is about 7.48 in the profile 4 linear frames
and about 0.24 in the 1920 x 384 frame.

The tolerance rests on two assumptions about the board's sampler that the
confirmation scenes do not test:

- **Both axes alike.** The scenes vary the texel coordinate along u only; the
  tolerance applies e_b to v as well.
- **Weight error independent of coordinate magnitude.** The scenes sample
  texel coordinates between 0.5 and 1.5. The profile 4 frames reach about +-16
  texels and the 1920 x 384 frame about +-192.

If the board breaks either assumption, the linear-filter comparison fails
although the confirmation passed. The output may still conform; a board scene
that tests the broken assumption is then needed before the tolerance is
revised.

### Confirmation scenes

The last batch of each nearest and linear profile 4 frame holds confirmation
scenes, whose pixels the comparator checks against exact rational values
rather than the reference; the sRGB frames have none. Texels 0 and 1 of the
profile 4 texture are black and white, and so are texels 8 and 9 below them, so
a linear-filtered pixel between them reads its weight directly: a weight error
e moves each channel by 255e. Each linear frame draws a ramp from texel 0 to
texel 1 on a quad with w = 1 at one edge and w = 4 at the other, and four
constant-w bands that each span a quarter texel over 64 pixels. The four bands
place their 256 pixel centres midway between consecutive multiples of 2^-8
texel, which a single ramp cannot do in a 128-pixel frame.

A band pixel passes when each channel is within 1 + 255 x 2^-8 of 255 times its
exact weight. A ramp pixel also allows 2^-11 texel for perspective division and
passes within 1 + 255 x (2^-8 + 2^-11). The board's conversion can move a pixel
by up to 1, so a passing pixel shows only that its weight lies within
(bound + 1)/255 of the exact value; that ramp figure is e_b. UNORM8 output
cannot resolve a 2^-8 weight step against this allowance, so the scenes do not
establish the number of subtexel bits or the perspective-division error. Both
7-bit weights rounded to nearest and 7-bit weights truncated pass the scenes
and the linear tolerance; weights truncated to 2^-6 fail them.

Each nearest frame's blended quad is the blending confirmation: each channel
within 1 of the exact blended value. If any confirmation pixel fails, the
profile 4 launch is reported as unconfirmed and none of its frames is compared.
Every other launch with a linear-filtered or blended frame (the profile 3
source-over frames, the 1920 x 384 frame and the 1920 x 1080 frame once
admitted) rests on the same assumptions. Its comparison requires the
`comparison.json` of a passed profile 4 launch with no failed or uncaptured
confirmation scene and a plan digest that matches the current plan. The plan
digest is the SHA-256 over the profile 4 frames' planned input hashes and state
words. Before reading a capture, the comparator applies every plan check to the
launch, timing included.

An edge tie is a pixel centre whose edge function is exactly zero in the plan's
arithmetic, so the implementation's tie-break rule decides coverage. Such a
pixel passes if it matches the reference at the same pixel or one of its eight
neighbours, as in dEQP's position-deviation comparison. Every other pixel has
one correct coverage under any conforming rasterizer.

A mismatched pixel equal to `PVR_RENDER_POISON` is reported as stale output,
pointing at completion or cache maintenance. The report counts exact, tolerant,
tie-accepted and failed pixels and lists the first 16 failures.

## Board launches and their modeled time

In `apps/gpu-render/usb.c`, `gpu_application_usb` runs one `tud_task`, one
polled `dcd_int_handler` and one exchange per `vTaskDelay(1)`; the DCD is
always polled because USB0 interrupt delivery is unqualified. The pinned
TinyUSB vendor stream moves at most one 512-byte packet per transfer and starts
the next only from `tud_task`, so a packet takes one tick or, when the poll
sees its completion an iteration late, two. The plan takes its tick figures as
parameters: 2 ticks per packet in either direction, 4 per 64 KiB slot for READ,
delivery and RELEASE, and 1 per completion. In every launch image the GPU
interrupt wakes the service worker, but the client observes completion and each
service reply by a one-tick STATUS poll (section 1.3 of
[performance-architecture.md](../performance-architecture.md)).

| Launch: image, envelope | Session | Frames | Readback | Worst-case modeled time |
| --- | --- | --- | --- | --- |
| Triangle profile: `sunstone-a733-gpu-draw-single-core` | 64 x 64 | Background only; three flat triangles sharing edges; 126-vertex fan; a degenerate triangle | 64 KiB | 1.3 s |
| Profile 2: texture image, version 1 | 255 x 129, stride 256 | Six: copy, mirrored copy and three texture sizes as full-screen quads, and triangles on the 1/8 NDC lattice | 0.8 MB | 5.1 s |
| Profile 3: texture image, version 2 | 256 x 256 | Eight: three comparisons with and without depth writes, and two source-over scenes | 2.1 MB | 8.5 s |
| Profile 4: texture image, version 3 | 128 x 128 | 18 frame states: nearest with and without sRGB, and linear, each with six address-mode pairs; up to 16 batches covering depth, stencil, blend, cull and colour write | 1.2 MB | 5.3 s |
| Profile 4 maximum width: texture image, version 3 | 1920 x 384 | One frame of 1,000 non-overlapping linear-filtered triangles | 2.9 MB | 12.1 s, frame stream 11.7 s |
| Compute, one per kernel: program application with runtime input | Package geometry | Three inputs each | 12 KB each | At most 0.52 s |

A session adds the 15-second OPEN budget and the one-second CLOSE allowance to
these modeled times, which cover uploads, submissions, completions and
readback. The plan asserts each of these at half its board limit:

- every frame stream at most 15 s (limit 30 s, [gpu-render.md](../gpu-render.md));
- OPEN, the launch and the one-second CLOSE allowance at most 30 s (session
  deadline 60 s); OPEN counts at its 15-second budget until the QEMU texture
  check supplies its tick count;
- each 64 KiB slot's delivery at most 0.5 s (limit 1 s in `delivered()` of
  `apps/gpu-render/texture.c`); 128 packets at 2 ticks plus 4 is 0.26 s;
- each request's upload at most 0.5 s (limit 1 s, `receive_deadline`); a
  64 KiB payload with its header is 0.26 s.

These figures hold only while the capture host keeps a bulk IN read
outstanding throughout a stream. A 1920 x 1080 profile 4 frame of the same
scene needs 16,200 packets and 32.9 s at 2 ticks per packet, so the plan
refuses it. At the 13 ticks per 64 KiB of the shared USB0 loop (section 3 of
performance-architecture.md), its 127 slots take about 1.7 s and the plan
admits it with no other change. The fixed-clear image compares every pixel with
`0xffc08020`, so its captured bytes are checked against that value and it needs
no launch.

Each launch needs its image, envelope version and existing external
prerequisites: extended-memory ownership for texture launches, the depth
allocation selected by the authenticated geometry for profiles 3 and 4, and for
compute the GPU memory, entry, recovery and ownership evidence of
[gpu-compiled-programs.md](../gpu-compiled-programs.md). A refused, failed or
uncertain capture is recorded with its stage and not retried; the driver's
rules retain every owner after uncertain completion. A launch or slot slower
than its modeled worst case is reported as a model discrepancy, and the plan's
tick parameters are updated from the measured value before the next board
session.

## Host self-checks

`checks/gpu-conformance.py` runs these before board time is spent:

- **Closed-form scenes.** One per feature, with edges on pixel boundaries and
  samples at texel centres; Python computes the frame with integer arithmetic
  and the reference must match. Depth bands below, at and above the stored
  value give the eight compare operations distinct patterns, and starting
  stencil values 0, 0x5a and 255 separate the eight stencil operations. The
  255 x 129 scenes exercise the stride crop.
- **Stencil separation.** The closed-form evaluation records each stencil
  compare and operation whose stored value a later stencil test reads. Each of
  the eight operations on each of the three paths must have run on start values
  where its result differs from each other operation. Each compare operation
  must differ from each other one on some recorded comparison, and a compare
  mask and a write mask must each change some recorded outcome. A fixed table
  of stencil test states across the 18 profile 4 frames meets this.
- **Confirmation.** The exact frames and the reference meet the confirmation
  bounds. Three modeled board filters are evaluated in closed form: weights
  truncated to 2^-6 must fail the confirmation in every linear frame; 7-bit
  weights rounded to nearest and 7-bit weights truncated pass it and stay
  within the linear tolerance of the reference frame.
- **Mutations.** Each of ten must fail at least one scene. Five set one
  reference render parameter: a half-pixel viewport offset, the other filter,
  the other interpolation mode, mirrored repeat for mirror clamp to edge, and
  skipped sRGB decoding. Five alter a capture: filter weights truncated to
  2^-6 (the profile 4 launch must become unconfirmed), stride padding read as
  pixels, one exact pixel changed by 2, one compute word changed, and a compute
  capture whose recorded job error is `PVR_JOB_RESULT`.
- **Plan refusals.** One scene violating each plan check is refused. The inputs
  and compare commands both refuse the 1920 x 1080 launch through the timing
  check.
- **Capture comparison.** On synthesized captures, exact profile 4 frames pass,
  coarse filter weights leave the launch unconfirmed, a failed frame is recorded
  with its result, and a wrong input hash, state or sequence or a texture frame
  without its generation is refused. The profile 3 comparison is unconfirmed
  without a profile 4 comparison, with one from another plan digest and with an
  unconfirmed one, and passes with the passed one. The 1920 x 384 comparison is
  unconfirmed without a profile 4 comparison.

Transport and driver paths stay with their owners: `checks/gpu-texture.c`
checks the driver's output invalidation, padded snapshot copy and 64 KiB
chunking against `result_pixel()`. No host check models the draw and
1920 x 1080 texture transports, including a partial final slot and a
packet-aligned end.

## Memory, caches and cores

Frames come from the snapshot the driver copies after both acquired
completions, output invalidation in batches of at most sixteen pages and guard
checks. Compute output is pre-filled and cleaned before submission and
invalidated after completion. All launches use single-core GPU applications, so
no cross-core synchronization is involved.

On the host, one reference process renders every frame of a launch, including
its parameter mutations, so EGL setup and shader compilation happen once per
launch. Each frame is rendered twice: once for colour and once with an id
program that writes the index of the last colour-writing triangle at each pixel
under the same depth, stencil, culling and colour-mask state; the id pass
selects each pixel's tolerance class. Each pass ends with `glFinish` and one
`glReadPixels`, with no polling. llvmpipe's four worker threads are the only
concurrency, within the six-job limit. The comparator runs in the same process
over the frame in memory, in one pass.

## Qualified figures

| Metric | Budget | Value | Evidence |
| --- | --- | --- | --- |
| Features with an output comparison | Every feature row in `docs/gpu-conformance.md` | 32 of 32 | `docs/evidence/gpu-conformance.jsonl`, `gpu-conformance-host-budgets` |
| Mutations detected | All ten | 10 of 10 | same record |
| Worst-case frame stream | At most 15 s | 11.705 s (1920 x 384) | same record |
| Worst-case session | At most 30 s | 28.104 s | same record |
| Slot delivery, request upload | At most 0.5 s each | 0.26 s, 0.258 s | same record |
| Board launches, readback | 12; at most 8 MiB | 12; 7,143,540 bytes | same record |
| Comparator time, 1920 x 384 frame | At most 50 ms | 2.06 ms | same record |
| Host check wall time after the Mesa build | At most 60 s | 13.16 s | same record |
| Edge-tie pixels | 0 in closed-form scenes; at most 5% of covered pixels elsewhere | 0; 15,250 of 716,250 (2.1%) in the 1920 x 384 scene | same record |
| Board image changes | 0 | 0 (all 13 images byte-identical) | same record |

Board wall time per launch and slot is not measured.
The modeled board predictions are in `gpu-conformance-board-launches`.

## Limitations

- **Pixels with two inexact stages**, such as sRGB before linear filtering or
  blending over a blended value, have no comparison. A composed tolerance would
  hide an error in either stage; single-stage scenes cover each.
- **Compute kernels** do not execute on the host. The closed-form reference
  covers the valid range, and the driver's guard check fails out-of-range writes
  before readback.
- **No reference frame cache.** One reference process renders a launch's frames
  well within the 60-second check budget, and a cache would need invalidation on
  Mesa, reference and state-mapping changes.
- **No USC simulator or Vulkan conformance suite.** Mesa has no USC simulator,
  and the suite needs a Vulkan driver; the harness takes only dEQP's
  position-deviation comparison.
- **Not covered:** display presentation (lending a frame changes no pixels), and
  features the driver does not implement: clipping, mipmaps, other formats,
  multisampling and arbitrary shaders. Section 8 of [goal.md](../goal.md)
  governs physical performance measurement.

## Open work

1. **Capture provenance.** `capture_frames()` in `apps/gpu-render/texture.py`
   and `apps/gpu-render/draw.py` returns only pixels per frame. It must return
   each frame's reply sequence (`h[4]` in texture replies, the decoded sequence
   in draw replies), the input generation for texture frames (`h[13]` of the
   BEGIN reply) and the reply state, and on a failed reply or CLOSE raise with
   the frames captured so far and the failing frame's reply state. Board
   comparison is blocked until then. Owner: the GPU render transport.
2. **Compute job error in the reply.** The program application's failure reply
   must carry the service's `phase` and `compute_job_error`, which
   `drivers/gpu/pvr_gpu_service.c` already fills, so a capture can record
   `PVR_JOB_RESULT`. Owner: the GPU compiled-programs work.
3. **Twelve board launches.** Run the launches in the table above once item 1
   lands. A launch confirms conformance when its recorded input hashes and
   states match the plan, its confirmation scenes pass, the driver's guard and
   timeline checks pass, every scene passes, and tie-accepted counts and wall
   times are recorded. The board evidence records wall time per launch and per
   slot.
4. **1920 x 1080 launch.** `apps/gpu-render/usb.c` moves to the loop in
   `apps/board/usb0_service.h` (13 ticks per 64 KiB); the plan then admits the
   launch from its updated tick parameters. Owner: the USB0 evidence channel.
5. **255 x 129 driver case.** `checks/gpu-texture.c` gains one `gpu_render_case`
   with a 255 x 129 frame, using `result_pixel()` and the existing per-word
   assertion. Owner: the GPU driver work.
6. **Short final packet.** The texture transport has no host model, so a
   short final packet is not covered. Owner: the texture transport work.
