# Design: GPU output conformance against a host software renderer

Status: **designed**, not implemented. No board run has produced GPU output,
so every comparison in this design ends at host-modeled, with physical
behavior pending until scheduled board captures pass the same comparison.

The GPU driver implements a fixed clear, a colored-triangle profile, three
indexed-texture profiles (2, 3 and 4, described in
[gpu-render.md](../gpu-render.md)) and caller-selected compute programs
([gpu-compiled-programs.md](../gpu-compiled-programs.md)). No check compares
their output with an independent renderer. The native service checks write a
modeled pattern into the device output, for example `pixel ^ 0x00ffffff` in
`checks/gpu-batch.c` and a scaled texel copy in
`checks/gpu-texture-runtime.py`. Those checks cover the copy, guard and
transport paths, and nothing about rasterization, sampling or kernel
execution. No host program executes PowerVR unified shader core (USC) code.
On the host, the harness can therefore check only its own reference and the
data path; GPU conformance is established by board captures.

The harness has three parts: a graphics reference built from the pinned Mesa
llvmpipe driver, a compute reference that executes the kernel sources on the
host, and a comparator with per-feature tolerances.

## Data flow and ownership

1. **Scene plan.** `tools/gpu-conformance.py` holds a fixed scene list. Each
   scene produces its canonical device input with the existing builders:
   `vertices()` in `apps/gpu-render/draw.py`, `input_bytes()` in
   `apps/gpu-render/texture.py` and the package input of `apps/gpu-program`.
   A scene's identity is the SHA-256 of those bytes plus the profile, state
   word and framebuffer size from the session envelope.
2. **Reference.** The reference program parses the same canonical bytes that
   the board receives. No scene is described twice, so the reference and the
   board cannot render different inputs.
3. **Board capture.** The existing capture functions (`capture_frames()` in
   `draw.py` and `texture.py`, `capture()` in `apps/gpu-program/runtime.py`)
   run under the board coordinator and write frames to the evidence
   directory. The harness opens no USB device.
4. **Host-model path.** `checks/gpu-conformance.py` loads the runtime model
   classes of `checks/gpu-texture-runtime.py` and `checks/gpu-draw-runtime.py`
   and replaces their modeled `render()` with the reference frame. Reference
   pixels then pass through the linked native application, the TinyUSB
   framing and the host decoder before comparison, which detects row-order,
   stride-padding and 64 KiB slot errors on the host.
5. **Comparison.** The comparator refuses a captured frame whose recorded
   input hash differs from the plan, then compares pixels or words.

Canonical inputs and captured frames are read-only inputs. Reference frames
are written once per key to `~/.cache/sunstoneos/build/gpu-conformance/`.
Results go to `~/.cache/sunstoneos/evidence/gpu-conformance-<date>/` and are
summarized in `docs/gpu-conformance.md`.

## Graphics reference

The build reuses the extracted Mesa tree at
`~/.cache/sunstoneos/gpu-producer-build/mesa`, revision
`86c7176d2dd0065555521bf8c61f208bf53c75d6`, after checking the archive
SHA-256 recorded in [gpu-render.md](../gpu-render.md). It uses the producer's
clang-21 and LLVM 21.1.8 environment with `-Dgallium-drivers=llvmpipe
-Dopengl=true -Degl=enabled`, no Vulkan, GLX or GLES drivers, `nice -n 10`
and `-j 6`. A stamp holding the archive hash and option list skips the build
when both are unchanged. `tools/gpu-conformance/reference.c` renders through
OpenGL 4.5 on the EGL surfaceless platform. Mesa compiles its GLSL shaders
itself; Mesa's Vulkan software driver would need hand-written SPIR-V and uses
the same llvmpipe rasterizer.

`glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE)` gives the Vulkan depth range
and a top-row origin, which the driver's state uses. The driver state maps to
OpenGL as follows:

- `VkCompareOp` n becomes `GL_NEVER + n`; both enumerations list the eight
  comparisons in the same order.
- `VkStencilOp` values 0–7 become `GL_KEEP`, `GL_ZERO`, `GL_REPLACE`,
  `GL_INCR`, `GL_DECR`, `GL_INVERT`, `GL_INCR_WRAP` and `GL_DECR_WRAP`.
- Blend equations 1–4 become `glBlendFuncSeparate` with (SRC_ALPHA,
  ONE_MINUS_SRC_ALPHA, ONE, ONE_MINUS_SRC_ALPHA), (ONE, ONE_MINUS_SRC_ALPHA,
  ONE, ONE_MINUS_SRC_ALPHA), (ONE, ONE, ONE, ONE) and (DST_COLOR, ZERO,
  DST_ALPHA, ZERO). Equation 0 disables blending; colour-write disable sets
  `glColorMask` to false.
- Culling modes 1 and 2 cull clockwise and counter-clockwise triangles in
  framebuffer coordinates. A closed-form scene fixes the `glFrontFace` sense.
- The sampler uses nearest or linear filtering, the five address modes
  (clamp to border with a transparent black border), `GL_SRGB8_ALPHA8` for
  sRGB decoding and `textureLod(…, 0)`.
- Colour and profile 2–3 texture coordinates use `noperspective`
  interpolation; profile 4 uses perspective-correct interpolation.
- Profile 3 uses a 32-bit float depth attachment and profile 4 a packed
  depth-stencil attachment, cleared to depth 1 and stencil 0 each frame. The
  colour clear is the value that the profile's background program stores,
  read from its producer output. Batches are drawn in submission order.

## Compute reference

The kernels in `shaders/` call PCO intrinsics such as `nir_load_vtxin_pco`,
`nir_dma_ld_pco` and `nir_uadd64_32`, which only the PowerVR compiler lowers.
No Mesa software driver can run them, including llvmpipe through Mesa's
OpenCL frontend. The harness compiles each kernel with clang-21 in OpenCL C
2.0 mode for the host. `-I tools/gpu-conformance` makes
`pco/usclib/libcl.h` resolve to a header that implements those intrinsics
and the work-item functions over host buffers placed at the package's GPU
addresses. Every lane of every workgroup runs in sequence. The package
contract forbids barriers, shared memory and atomics, so lane order changes
no result when no two lanes write the same word. The header records the
writer of each output word and refuses a run in which two lanes write one
word or any access leaves the declared input or output extent. The clang
front end is shared with `mesa_clc`, so a front-end defect would appear in
both outputs and is not detectable by this comparison.

## Comparison and tolerances

| Output | Tolerance per UNORM8 channel or word |
| --- | --- |
| Clear, unblended nearest texels without sRGB, compute words | Exact |
| Interpolated colour, blending, sRGB decoding | ±1 |
| Linear filtering | ±(1 + ⌈Δ/32⌉), where Δ is the largest channel difference among the four filtered texels; Vulkan's minimum of 4 subtexel bits allows a weight error of 1/32 |

The reference program also computes an ambiguity mask in double precision
from the binary32 inputs. A pixel is ambiguous when its centre lies within
2⁻⁴ pixel of a triangle edge, because Vulkan requires only 4 subpixel bits and
leaves edge tie-breaking to the implementation. It is also ambiguous when a
nearest-filter texel coordinate lies within 2⁻⁶ texel of a texel boundary, or
when two covering surfaces differ in depth by less than 2⁻¹⁶. Scenes keep
texture coordinates within [−4, 4] and textures at most 256 texels wide, so
binary32 interpolation error stays well below those margins. An ambiguous
pixel passes if it matches the reference within tolerance at the same pixel
or at one of its eight neighbours, as in dEQP's position-deviation
comparison. A uniform half-pixel shift still fails, because most pixels it
changes lie outside the 2⁻⁴ band. A scene in which more than 5% of covered
pixels are ambiguous is refused when the plan is built.

A mismatched pixel equal to `PVR_RENDER_POISON` is reported as stale output,
which points at completion or cache maintenance rather than rendering. The
report gives counts of exact, tolerant, ambiguous-accepted and failed pixels,
and the first 16 failures with both values.

## Scenes and board launches

| Launch | Session | Frames | USB payload |
| --- | --- | --- | --- |
| Triangle profile | 64 × 64 | Background only; three flat triangles sharing edges; 126-vertex interpolated fan; a degenerate triangle | 64 KiB |
| Profile 2 | 255 × 129, which exercises stride padding | Six: texture copy, mirrored copy, 64 random triangles and three texture sizes | 0.8 MB |
| Profile 3 | 256 × 256 | Eight: three comparisons, each with and without depth writes, over overlapping quads, and two source-over blending scenes | 2.1 MB |
| Profile 4 | 256 × 256 | 24 frame states (two filters, sRGB on and off, six address-mode pairs); each frame carries up to 16 batches that cover the depth, stencil, blend, cull and colour-write fields | 6.3 MB |
| Profile 4 maximum | 1920 × 1080 | Two frames of 1,000 random textured triangles | 16.6 MB |
| Compute, one per kernel | The fixed job's probe kernel and the six caller-selected kernels in `shaders/` | Three inputs each, within the package geometry | 12 KB each |

The fixed-clear image already compares every pixel with `0xffc08020` before
copying it, so the harness checks its captured bytes against that value and
adds no launch.

Before any board time is spent, `checks/gpu-conformance.py` runs:

- **Closed-form scenes.** One scene per feature, with rectangle edges on pixel
  boundaries and texture samples at texel centres. Python computes the
  expected frame with integer arithmetic, and the reference must match it.
  These scenes fix the OpenGL state mapping, including the cull sense, row
  order and stencil operations.
- **Mutations.** The reference renders each scene with one altered state
  and submits the result as the GPU frame. Each mutation must fail at least
  one scene. The mutations are:
  - a half-pixel offset, inverted culling and bottom-up rows;
  - each compare and stencil operation replaced by its neighbour;
  - premultiplied factors for equation 1 and skipped sRGB decoding;
  - mirror instead of mirror-clamp addressing, the other filter and the
    other interpolation mode;
  - stride padding read as pixels, and one exact-tolerance pixel changed
    by 2;
  - for compute, one changed word and one write by a padded lane.
- **Determinism.** Reference frames are byte-identical with
  `LP_NUM_THREADS=0` and `LP_NUM_THREADS=4`.
- **Compute self-check.** The host execution matches a closed-form Python
  expression of each of the seven kernels on the same inputs.
- **Host-model path.** Reference frames pass through the runtime models as
  described in Data flow and ownership.

## Interrupts, memory, caches and cores

The harness changes no board source, image or configuration. Board captures
use the existing images and their existing completion paths: the GPU
interrupt, or the tick poll that section 1.3 of
[performance-architecture.md](../performance-architecture.md) records for
the accelerator client. Frames come from the copied snapshot that the
driver cleans to the point of coherency after acquired completion. All
launches use the single-core GPU applications, so no cross-core
synchronization is involved. The GPU rows of section 3 keep their current
baselines and belong to the GPU driver work.

On the host, one reference process renders every frame of one launch,
including its mutations, so EGL setup and shader compilation happen once per
launch. Each frame ends with `glFinish` and `glReadPixels` into one buffer,
with no polling. llvmpipe worker threads are the only concurrency; the
harness sets `LP_NUM_THREADS=4`, within the six-job limit. The comparator runs
in the same C process over the frame in memory, in one pass.

## Reuse and removal

The harness reuses the pinned Mesa tree and its hash, the producer's
compiler environment, the scene input builders, the capture functions, the
runtime model classes, the evidence layout and `checks/check-index.py`. It
removes nothing, because no output comparison exists to replace, and it adds
no reporting system.

## Budgets

| Metric | Baseline | Budget | Measurement |
| --- | --- | --- | --- |
| Implemented features with an independent output comparison | 0 (counted; existing checks inject modeled output) | Every feature row in `docs/gpu-conformance.md` has a scene | Coverage table generated by the check |
| Listed mutations detected | None | All | Check result |
| Board launches for full coverage | No plan | 12 | Scene plan |
| USB payload for all launches | No plan | At most 32 MiB, about 6.7 s at the 5.0 MB/s of `checks/usb0-service-burst.py` | Sum of frame extents in the plan |
| Changes to board images | 0 | 0; no CMake file references an owned file | Check asserts it |
| Reference processes per check run | None | One per launch, at most 12 | Counted by the check |
| Comparator time per 1920 × 1080 frame | None | At most 100 ms, one pass | `clock_gettime` in the report |
| Host check wall time, reference already built | None | At most 60 s | `time` around the check |
| Mesa rebuilds with unchanged hash and options | None | 0 | Build stamp |
| Ambiguous pixels per scene | None | At most 5% of covered pixels; 0 in closed-form scenes | Report |

## Left out

- **Board captures.** Board access is not available to this work; the
  scheduled board session runs the twelve launches. A launch confirms
  conformance when its frames carry the planned input hashes, the driver's
  guard and timeline checks pass, every scene passes the comparison and the
  ambiguous-accepted counts are recorded.
- **A USC simulator.** None exists in Mesa, and its behavior could only be
  inferred from the hardware it would stand in for.
- **The Vulkan conformance suite.** It needs a Vulkan driver; the board
  driver accepts fixed profiles. The harness takes only dEQP's
  position-deviation comparison.
- **Image-difference files.** The report lists failing pixels, and raw frames
  stay in the evidence directory.
- **Display presentation.** Lending a frame changes no pixels; HDMI output
  belongs to the display work.
- **Unimplemented features.** Clipping, mipmaps, other formats, multisampling
  and arbitrary shaders are not implemented by the driver, so the harness has
  no scenes for them.
- **GPU timing on the board.** Section 8 of [goal.md](../goal.md) governs
  physical performance measurement.

No change to code reserved for the primary agent is needed.
