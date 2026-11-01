# Video, image codecs and deinterlacing

**Status: implemented.**

Native JPEG, H.264, HEVC and DI301 paths have host,
instruction or device-model evidence. Numerical device output is unqualified.

**Board-qualified scope:** None.

Physical arithmetic, cache visibility,
confinement, interrupts, drain, output and recovery remain unqualified.

Evidence paths below are relative to `~/.cache/sunstoneos/evidence/` unless
a build path is stated.

JPEG decode supports baseline YCbCr420 through 512 KiB input and padded
640x480 luma capacity. Version-3 sessions support eight manifest images;
the supervised capture tool and bundle still carry one image. Full HD needs more uncached capacity.
Sparse-coefficient CPU oracles do not establish general JPEG conformance.
Some malformed-input evidence lacks preserved runner/inputs.
Evidence: `jpeg-runtime-input-20261018/` under `~/.cache/sunstoneos/evidence/`.

H.264 decode supports progressive 8-bit 4:2:0 Baseline/Main/High,
CAVLC/CABAC, I/P/B including reference B pictures, scaling matrices, weighted
prediction and long-term P references. The current High-profile layout has four full-HD
slots, three full-HD short-term references or four HD references, and 2 MiB
input per picture. Fields, wider formats, long-term B references and unrestricted
streaming remain open. Frozen High/full-HD/VGA inputs remain distinct in
`h264-high-20261019/`, `h264-fullhd-20261019/` and `h264-vga-20261019/`.
Current layout and wraparound tests under
`~/.cache/sunstoneos/build/audit-video-consolidation/checks/` and
`~/.cache/sunstoneos/build/audit-codec-streaming/checks/` lack a new evidence
freeze. Older output/launcher limitations remain in the worklog.

HEVC decodes one uploaded two-picture 640x480 Main/8-bit IDR/P clip into copied
NV12. Preparation refuses physical bundles without allocation/page-table
evidence. The vendor trace covers IDR programming; P control bits lack that
trace. Comparator tests use CPU-generated pixels. Main10, larger geometry,
slices, tiles, wavefront parallel processing and B pictures remain required.
Retained initialization and comparator checks pass under
`~/.cache/sunstoneos/build/integrate-streams-1031/`.

JPEG encode supplies one retained 640x480 NV12/quality-75 job into a 1 MiB
JPEG result. `jpeg-encode-native-20261020/root/integration.json` binds its
modeled evidence. Wider formats, quality, geometry and repetition remain open.

H.264 encode accepts one-to-thirty Baseline/CAVLC/QP26 pictures alternating
two uploaded 640x480 NV12 frames, with IDR then P references. Programming
matches the pinned vendor-library reference; entropy, completion and reference
contents remain modeled. `h264-encode-20261020/root/integration.json` freezes
the older two-picture image. The thirty-picture image is not frozen, and capture
still handles two. Rate control, unique per-picture inputs and wider modes remain.

DI301 implements WEAVE, motion interpolation and fixed/selected-strength temporal
noise reduction for 640x480 NV12, up to four jobs per sequence and adaptive gain
feedback. Revision-5 repeated-job checks are not frozen. Earlier frozen evidence
is in `deinterlace-native-20261020/`, `deinterlace-motion-20261020/` and
`deinterlace-tnr-20261020/`. Reuse requires physical proof that finish ends buffer
access and hardware clears START. All resources remain retained.

See [JPEG](../jpeg-application.md), [H.264](../h264-native.md),
[HEVC](../hevc-native.md), [JPEG encode](../jpeg-encode-native.md),
[H.264 encode](../h264-encode-native.md) and [deinterlace](../deinterlace-native.md).
Cedrus/GStreamer adaptations execute as FreeRTOS CPU code; opaque encoder
libraries supply development references and are not linked into native images.
