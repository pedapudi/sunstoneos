# GPU output conformance against a host software renderer

Status: **implemented and host-modeled**. The harness renders every scene the
board will draw with the pinned Mesa llvmpipe driver and compares captured
frames with that rendering within per-pixel tolerances. No board capture has
been compared yet; physical conformance remains unqualified until the twelve
launches below pass on the board. Board comparison is blocked until the
capture functions return the provenance that the comparison checks (see
Remaining work).

The [design](designs/gpu-conformance.md) gives the reasoning behind each
tolerance and plan check. This page describes the implemented harness, how to
run it and what it has shown.

## Components

- `tools/gpu-conformance.py` holds the scene list, the plan checks, the
  USB time model, the compute reference and the capture comparison. Each scene
  builds its device input with `vertices()` in `apps/gpu-render/draw.py` or
  `input_bytes()` in `apps/gpu-render/texture.py`, so the reference and the
  board receive the same bytes. The plan records the SHA-256 of those bytes
  and the frame's state word, and a capture must record the same two values.
- `tools/gpu-conformance/reference.c` is the reference renderer. It renders
  through OpenGL 4.5 on surfaceless EGL, parses the device input itself and
  compares a capture with its own frame in one pass. It also renders an id
  pass with the same depth, stencil, culling and colour-mask state, which
  names the last triangle that wrote each pixel and so selects that pixel's
  tolerance. On request it writes its unmutated frames to a file, which the
  host self-check reads to check the reference's confirmation pixels.
- `checks/gpu-conformance.py` is the host self-check.

`gpu-conformance.py build` extracts
`mesa-86c7176d2dd0065555521bf8c61f208bf53c75d6.tar.gz` after checking its
SHA-256 against the pin in [gpu-render.md](gpu-render.md), and builds
llvmpipe with OpenGL and EGL only, using the GPU compiler producer's clang-21
and LLVM 21.1.8, `nice -n 10` and six jobs. A stamp of archive hash, options,
compiler versions and `LP_NUM_THREADS=4` skips unchanged rebuilds. A clean
build takes 44 s. The reference process checks at start-up that the renderer
is llvmpipe from Mesa 27.0.0-devel and that every loaded EGL and driver library
lies in the pinned install directory.

## Scenes and tolerances

Vertices lie on a 1/16-pixel lattice that the fp32 viewport transform
reproduces, so a rasterizer with Vulkan's minimum of 4 subpixel bits places
every edge as the reference does. Each pixel has one tolerance, chosen by the
triangle that wrote it last:

| Pixel source | Tolerance per UNORM8 channel |
| --- | --- |
| Clear, nearest texel without sRGB | Exact |
| Interpolated colour, one blended layer, sRGB decode | ±1 |
| Linear filtering | ±(1 + ⌈2Δ(e_b + e_r)⌉): 9 in the profile-4 linear frames, 2 in the 1920 × 384 frame |

Δ is the largest channel difference between two texels that can be bilinear
neighbours under the frame's address modes, border colour included. e_b,
just under 2⁻⁸ + 2⁻¹¹ + 2/255, is the per-axis filter weight error that the
board's confirmation scenes admit, and e_r, 2⁻⁹ + 2⁻¹¹, bounds llvmpipe's
own weight error; the [design](designs/gpu-conformance.md) derives both. A
pixel
centre lying exactly on a triangle edge, called an edge tie, passes when it
matches the reference at itself or one of its eight neighbours. A mismatched
pixel equal to `PVR_RENDER_POISON` is counted as stale output.

The plan refuses a scene that breaks any of these conditions:

- a vertex lies off the lattice, or a profile-4 vertex has w other than 1, 2
  or 4;
- a profile-4 texture coordinate exceeds magnitude 4;
- a nearest-filtered sample at a covered pixel centre lies within 2⁻⁷ texel of
  a texel boundary, computed with exact integer arithmetic;
- two surfaces at one pixel centre differ in depth by less than 2⁻¹⁶ without
  being equal constant-depth planes, the clear value or the same triangle
  drawn twice;
- edge ties exceed 5% of covered pixels;
- a pixel passes through two inexact stages: linear filtering of sRGB texels,
  blending a non-exact source, or blending over a value that is not exact;
- a reference pixel lies within its tolerance of `PVR_RENDER_POISON`, or an
  expected compute word equals the output fill `0xa5a5a5a5`;
- a launch exceeds half of any board time limit in the model below.

### Hardware assumptions and their confirmation scenes

Two tolerances rest on hardware behaviour that only the board can show:

- after perspective division, each bilinear filter weight lies within e_b of
  the exact value;
- blending runs at destination precision or better, with one final rounding.

The last batch of each nearest and linear profile-4 frame holds confirmation
scenes; the sRGB frames have none. The comparison checks their pixels
against exact rational values computed in Python, independently of the
reference:

- **Filter weights.** Texels 0 and 1 of the 8 × 8 texture are black and
  white, as are texels 8 and 9 below them, so a linear-filtered pixel between
  them reads its filter weight directly: a weight error e moves each colour
  channel by 255e. Each linear frame draws a 64 × 32 perspective ramp from
  texel 0 to texel 1 with w from 1 to 4. It also draws four constant-w bands
  of 64 × 8 pixels, each spanning a quarter texel, whose 256 pixel centres
  lie midway between consecutive multiples of 2⁻⁸ texel. A band pixel passes
  when every channel is within 1 + 255 × 2⁻⁸ of 255 times the exact weight;
  a ramp pixel also allows 2⁻¹¹ texel for perspective division. The board's
  conversion may move a pixel by up to 1, so a passing pixel shows that its
  weight lies within e_b of the exact value along u. The scenes cannot
  resolve the number of subtexel bits or the perspective-division error on
  their own, and they do not vary v; the tolerance assumes that v behaves
  as u, and a less accurate v weight makes the comparison fail.
- **Blending.** The blended 32 × 32 quad of each nearest frame passes when
  every channel is within 1 of the exact blended value.

If any confirmation pixel fails, or a frame holding confirmation scenes was
not captured, the comparison reports the profile-4 launch as unconfirmed and
compares none of its frames: the tolerances must be revised first. The
profile-3 source-over frames rest on the same blending assumption, and the
1920 × 384 launch on the same filter bound. Their comparisons require the
`comparison.json` of a passed profile-4 launch whose recorded plan digest,
the SHA-256 over the planned profile-4 input hashes and states, matches the
current plan, and otherwise report the launch as unconfirmed.

The self-check evaluates three modeled board filters in closed form. Filter
weights truncated to 2⁻⁶ fail the confirmation in every linear frame. 7-bit
weights rounded to nearest and 8-bit weights after a 2⁻⁹ texel coordinate
bias both pass it, and both stay within 1 of the reference, inside the
linear tolerance of 9. The pinned llvmpipe reference meets the confirmation
bounds.

### Closed-form scenes

Closed-form scenes use rectangles with pixel-boundary edges and texture
coordinates that vary along one axis each. Python computes their frames with
rational and integer arithmetic, and the self-check requires the reference to
match them within each pixel's tolerance. Each
rectangle is split into triangles whose edges cross no pixel centre, so these
scenes have no edge ties.

## Board launches

All graphics launches use single-core images and the existing capture
functions under the board coordinator. The tool opens no USB device.

| Launch | Image and envelope | Frames | Readback | Modeled time | Session |
| --- | --- | --- | --- | --- | --- |
| `triangle` | `sunstone-a733-gpu-draw-single-core`, 64 × 64 | Background only, three flat triangles sharing edges, a 126-vertex fan, a degenerate triangle | 64 KiB | 1.3 s | 17.3 s |
| `profile2` | Texture image, envelope version 1, 255 × 129 | Copy, mirrored copy, 15 × 3, 51 × 43 and 85 × 129 textures as full-screen quads, eight triangles on the 1/8 NDC lattice | 0.79 MB | 5.1 s | 21.1 s |
| `profile3` | Texture image, envelope version 2, 256 × 256 | Six depth frames (LESS, LEQUAL and ALWAYS, with and without writes) and two source-over frames | 2.1 MB | 8.5 s | 24.5 s |
| `profile4` | Texture image, envelope version 3, 128 × 128 | 18 frame states: nearest, nearest sRGB and linear, each with six address-mode pairs; 16 batches each | 1.2 MB | 5.3 s | 21.3 s |
| `profile4-wide` | Texture image, envelope version 3, 1920 × 384 | 1,000 non-overlapping linear-filtered triangles | 2.9 MB | 12.1 s | 28.1 s |
| `compute-<kernel>` | Compute image with the program application and runtime input | Package input and two runtime replacements, for each of seven kernels | at most 12 KB | at most 0.52 s | at most 16.5 s |

The planned readback is 7.1 MB. The session column adds the 15-second OPEN
budget and the one-second CLOSE allowance to the modeled time; the largest,
28.1 s, is within the 30-second plan limit, half the 60-second session
deadline.

Each profile-4 frame divides the framebuffer into regions:

- a 64 × 63 quad sampling an 8 × 8 texture over u from −2 to 2, two pixels per
  texel, with w of 1, 2 or 4 by filter;
- a depth region where two batches compare bands at depths 0.25, 0.5 and 0.75
  against a stored plane at 0.5;
- a stencil region whose columns start at 0, `0x5a` and `0xff` and whose lower
  row fails the depth test. The reference `0x5a` is greater than, equal to
  and less than the three start values. One test batch applies its compare,
  stencil-fail, depth-fail and pass operations, and six observation batches
  then draw only where the stored value equals the expected one;
- a clockwise and a counter-clockwise quad under the frame's culling mode;
- the confirmation scenes: a blended quad over an opaque underlay and the
  background in nearest frames, or the filter ramps in linear frames.

Across the 18 frames every depth compare operation, blend equation, culling
mode and address mode on each axis appears at least once. The stencil test
batches follow a fixed table of compare and stencil operations. One frame
compares through mask `0xa5`, which makes start value 0 equal to the
reference and so changes the EQUAL outcome, and three frames write through
mask `0x0f`. The closed-form evaluation records every stencil compare and
operation whose stored value a later stencil test reads. From that record,
the self-check requires each of the eight stencil operations, on each of the
three paths, to run on start values where its result differs from each of
the other seven operations. For example, DECR and DECR_WRAP differ only at
start value 0, and INCR and INCR_WRAP only at `0xff`. It also requires each
compare operation to give a different outcome from each other one on some
recorded comparison, and each mask to change some recorded outcome. The
stencil coverage rows come from this record.

The compute reference is a closed-form Python expression of each kernel over
the package geometry, compared word for word with the valid output range. A
capture whose recorded job error is `PVR_JOB_RESULT` is reported as an
out-of-range write: the driver's guard check fails the job before readback.

The fixed-clear image needs no launch: `compare --launch fixed-clear`
compares its 64 × 64 capture with `0xffc08020`.

## USB time model

`gpu_application_usb` in `apps/gpu-render/usb.c` runs one `tud_task`, one
polled `dcd_int_handler` and one exchange per one-tick `vTaskDelay`, and the
FreeRTOS tick is 1 ms. The model therefore charges 2 ticks per 512-byte packet
in either direction, 4 ticks per 64 KiB slot for READ, delivery and RELEASE, 1
tick per completion and 1 tick for each service reply. The plan requires every
frame stream to take at most 15 s, every slot and every request upload at most
0.5 s, and every session at most 30 s. The largest values are 11.7 s for the
1920 × 384 frame stream and 0.26 s for a slot and for a 64 KiB upload. A
1920 × 1080 frame of the same scene would stream for 32.9 s, so the plan
refuses that launch until the GPU image uses the shared USB0 loop. These
figures hold only while the capture host keeps a bulk IN read outstanding
throughout a stream.

## Running a board comparison

1. `gpu-conformance.py inputs --launch NAME --output DIR` applies every plan
   check to the launch, timing included, and writes each frame's device
   input, shape, state and input SHA-256. It refuses the 1920 × 1080 launch
   through the plan's timing check.
2. The board coordinator passes those inputs to `capture_frames()` in
   `draw.py` or `texture.py`, or to `runtime.sequence()` for compute. It
   records `capture.json` beside the captured files. Per frame it records the
   SHA-256 of the bytes passed to the capture function, the state word, the
   reply's frame sequence and result, the file name and, for texture
   launches, the input generation. Per compute input it also records the
   service phase and compute job error. A capture stops at the first failed frame. It records wall
   time per launch and per slot. The capture functions do not yet return the
   sequence, generation or result, or the frames captured before a failure,
   so this step waits for the change listed under Remaining work.
3. `gpu-conformance.py compare --launch NAME --capture DIR --output DIR`
   refuses a capture whose recorded hash or state differs from the plan,
   whose sequences do not advance by one per frame, whose texture frames
   lack a generation, or that records frames after a failed one. It records a frame whose reply
   result is not 1 as failed with that result, and frames after it as not
   captured. For the profile-4 launch it checks the confirmation scenes
   first. It then renders the captured frames in one reference process and
   writes `comparison.json` with the launch status (passed, failed or
   unconfirmed), each frame's status, exact, tolerant, tie-accepted, failed
   and stale pixel counts and the first 16 failures; for profile 4 it also
   records the plan digest. The comparisons of the profile-3 and 1920 × 384
   launches take `--confirmation` with the `comparison.json` of the passed
   profile-4 launch. The output directory is created only when
   `comparison.json` is written.

A launch confirms conformance when its input hashes and states match, its
confirmation scenes pass, the driver's guard and timeline checks pass, every
frame passes, and its wall times are within the modeled values. A slower launch or slot is a model discrepancy: update
`TICKS` in the tool from the measured value before the next board session.
A refused, failed or uncertain capture is recorded with its stage and not
retried.

## Host self-check

```sh
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python checks/gpu-conformance.py --output DIR
```

The check runs five reference processes and verifies the following:

- every closed-form scene matches the reference with no edge ties, and no
  reference pixel lies within its tolerance of `PVR_RENDER_POISON`;
- the reference and the exact frames meet the confirmation bounds; filter
  weights truncated to 2⁻⁶ fail them; 7-bit weights rounded to nearest and
  8-bit weights after a 2⁻⁹ texel coordinate bias pass them and stay within
  the linear tolerance of the reference;
- the recorded stencil executions separate every stencil operation on every
  path, and every compare operation, from the other seven;
- each of ten mutations fails at least one scene. Five change the reference:
  a half-pixel viewport offset, the other filter, the other interpolation
  mode, mirrored repeat in place of mirror clamp to edge, and skipped sRGB
  decoding. Five change a capture: filter weights truncated to 2⁻⁶, stride
  padding read as pixels, one exact pixel changed by 2, one compute word
  changed, and a compute job error of `PVR_JOB_RESULT`;
- one scene breaking each plan check is refused, and the inputs command
  refuses the 1920 × 1080 launch through the plan's timing check;
- the capture comparison passes exact profile-4 frames, reports the launch
  as unconfirmed when filter weights are truncated to 2⁻⁶, records a frame
  that failed with a reply result, and refuses a wrong input hash, state or
  sequence and a texture frame without its generation; a triangle record
  needs no generation;
- the profile-3 comparison is unconfirmed without a profile-4 comparison,
  with one from another plan digest and with an unconfirmed one, and passes
  with the passed one;
- the twelve launches meet their budgets;
- the comparator handles the 1920 × 384 frame within 50 ms;
- every row of the coverage table below has a comparing scene;
- the checks after the Mesa build take at most 60 s. `result.json` records
  the build time separately.

## Coverage

Closed-form rows compare the reference with an exact Python frame on the
host; on the board the same scenes compare the capture with the reference.
Rows marked reference-only use general triangles and are compared with the
reference on the board.

| Feature | Comparison |
| --- | --- |
| Fixed clear | Capture against `0xffc08020` |
| Background clear | Closed form |
| Interpolated colour | Closed form on the host; the 126-vertex fan on the board |
| Triangle coverage, shared edges and fans | Reference-only |
| Degenerate triangles | Reference-only |
| Nearest sampling with clamp to edge | Closed form |
| Texture scaling and mirrored coordinates | Closed form |
| Row stride crop | Closed form, 255 × 129 with stride 256 |
| Textured triangles off the pixel grid | Reference-only |
| Depth test, profile 3 | Closed form |
| Source-over blending, profile 3 | Closed form |
| Depth compare operations | Closed form |
| Depth writes | Closed form |
| Stencil compare operations | Closed form |
| Stencil operations on stencil fail, depth fail and pass | Closed form |
| Stencil compare and write masks | Closed form |
| Blend equations | Closed form |
| Culling | Closed form |
| Colour-write disable | Closed form |
| Address modes on U and V | Closed form |
| Linear filtering | Closed form |
| sRGB decoding | Closed form |
| Perspective-correct texture coordinates | Closed form |
| Sixteen index-range batches | Closed form |
| Maximum-width frame | Reference-only |
| Compute: probe | Closed-form expression |
| Compute: vector-add | Closed-form expression |
| Compute: vector-add-dispatch | Closed-form expression |
| Compute: vector-affine | Closed-form expression |
| Compute: vector-affine-dispatch | Closed-form expression |
| Compute: grid-transpose | Closed-form expression |
| Compute: grid-neighbour | Closed-form expression |

## Evidence

Results are kept under `~/.cache/sunstoneos/evidence/gpu-conformance-20261022/`:
`baseline.json` holds the figures recorded before implementation and
`host-check/result.json` the self-check result.

| Metric | Before | Budget | Result |
| --- | --- | --- | --- |
| Features with an independent output comparison | 0 | Every coverage row | 32 of 32 rows |
| Mutations detected | None | All ten | Ten |
| Board launches | No plan | 12 | 12 planned; 1920 × 1080 refused |
| Planned readback | No plan | At most 8 MiB | 7.1 MB |
| Worst-case frame stream | None | At most 15 s | 11.7 s |
| Worst-case session | None | At most 30 s | 28.1 s |
| Worst-case slot delivery and request upload | None | At most 0.5 s each | 0.26 s each |
| Edge-tie pixels | None | At most 5% of covered pixels; none in closed-form scenes | 2.1% in the 1920 × 384 frame; none elsewhere |
| Reference processes per self-check | None | At most 5 | 5 |
| Comparator time, 1920 × 384 frame | None | At most 50 ms | 2.0 ms |
| Host check wall time after the Mesa build | None | At most 60 s | 13.8 s |
| Mesa rebuilds with unchanged stamp | None | 0 | 0 |
| Texture image SHA-256 | `fc39d18f…` | Unchanged | `fc39d18f…` |

Board wall time per launch and slot is not yet recorded.

## Remaining work

- **Capture provenance.** `capture_frames()` in `apps/gpu-render/texture.py`
  and `draw.py` must return each frame's reply sequence, its input
  generation (texture frames only) and its reply state, and on a failure
  raise with the frames captured so far attached. Until the GPU render
  transport work makes this change, a capture record cannot carry the
  values the comparison checks, so board comparison is blocked.
- **Board captures.** The scheduled board session runs the twelve launches;
  each needs its image, envelope version and existing prerequisites:
  extended-memory ownership for texture launches, the depth allocation
  selected by the authenticated geometry for profiles 3 and 4, and for compute
  the GPU memory, entry, recovery and ownership evidence of
  [gpu-compiled-programs.md](gpu-compiled-programs.md).
- **Compute job error in the reply.** The program application's failure reply
  does not yet carry the service's `phase` and `compute_job_error`, so a
  capture cannot record `PVR_JOB_RESULT` until the compiled-programs work adds
  them.
- **Two runtime replacements per compute launch.** `runtime.sequence()` in
  `apps/gpu-program/runtime.py` replaces the input once; the third input of
  each compute launch needs a second replacement through the same native API.
- **The 1920 × 1080 launch** waits for the GPU image to move to the shared
  USB0 loop at 13 ticks per 64 KiB.
- Pixels with two inexact stages, host execution of compute kernels, a USC
  simulator, display presentation and GPU timing are outside this harness, as
  the design explains.
