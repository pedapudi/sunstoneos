#!/usr/bin/env python3
"""Plan GPU conformance launches, render them with pinned Mesa llvmpipe and compare captures.

  gpu-conformance.py build                     build pinned Mesa and the reference renderer
  gpu-conformance.py plan [--output FILE]      check every scene and launch; print the plan
  gpu-conformance.py inputs --launch NAME --output DIR
                                               write the canonical device inputs for a capture
  gpu-conformance.py compare --launch NAME --capture DIR --output DIR [--confirmation FILE]
                                               compare one recorded board capture

The plan builds each scene's device input with the existing builders in
apps/gpu-render, refuses scenes the comparison cannot judge and models each
launch's USB time. One reference process per launch renders every frame and
compares captures in memory. This tool opens no USB device; captures come from
the board coordinator. See docs/gpu-conformance.md.
"""
import argparse
from dataclasses import dataclass
from fractions import Fraction as Q
import hashlib
import importlib.util
import json
import functools
import math
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
BUILD = CACHE / 'build/gpu-conformance'
MESA_REVISION = '86c7176d2dd0065555521bf8c61f208bf53c75d6'
MESA_SHA256 = 'ca5e3ac4b61b8dc659cc489c7eb3c7bcc750f6820fbf39f0b831ac87fb00da58'
MESA_ARCHIVE = CACHE / f'sources/gpu-compute-producer/mesa-{MESA_REVISION}.tar.gz'
PRODUCER_HOST = CACHE / 'gpu-producer-host'
PRODUCER_VENV = CACHE / 'gpu-producer-venv'
MESA_OPTIONS = ['-Dbuildtype=release', '-Dgallium-drivers=llvmpipe', '-Dvulkan-drivers=[]', '-Dopengl=true',
                '-Degl=enabled', '-Dglx=disabled', '-Dgles1=disabled', '-Dgles2=disabled', '-Dplatforms=[]',
                '-Dgbm=disabled', '-Dglvnd=disabled', '-Dllvm=enabled', '-Dshared-llvm=enabled', '-Dtools=[]',
                '-Dbuild-tests=false', '-Dvideo-codecs=[]', '-Dshader-cache=disabled', '-Dvalgrind=disabled',
                '-Dlibunwind=disabled', '-Dlmsensors=disabled', '-Dxmlconfig=disabled', '--wrap-mode=nodownload']
LP_NUM_THREADS = 4
JOBS = 6

CLEAR = 0xffc08020          # background program value, RGBA bytes 20 80 c0 ff
POISON = 0x73951a6e         # PVR_RENDER_POISON in include/pvr_render_programs.h
COMPUTE_FILL = 0xa5a5a5a5   # pre-filled compute output word
# USB time model, counted from apps/gpu-render/usb.c: one exchange per 1 ms
# tick. Board wall time per launch and slot replaces these when it differs.
TICKS = {'packet': 2, 'slot': 4, 'completion': 1, 'reply': 1}
TICK_S = 0.001
OPEN_S, CLOSE_S = 15.0, 1.0
LIMITS = {'frame_stream_s': 15.0, 'session_s': 30.0, 'slot_s': 0.5, 'upload_s': 0.5, 'readback_bytes': 8 << 20,
          'tie_fraction': 0.05}
# Subpixel lattice of window coordinates: Vulkan's minimum of 4 subpixel bits.
LATTICE = 16
# Linear-filter weight bounds, in texels per axis. A board confirmation
# pixel passes within 1 + 255 x (FILTER_STEP, plus PERSPECTIVE_ERROR on the
# w = 1..4 ramp) of 255 times its exact weight. The board's own conversion may
# take either neighbouring UNORM8 value, so a pass admits a weight error just
# below (bound + 1) / 255. llvmpipe rounds the texel coordinate to 1/256 on its
# 8-bit path (lp_bld_sample_aos.c) and filters in fp32 otherwise, after fp32
# coordinate arithmetic bounded by 2^-11 texel. The scenes cannot see whether
# the board rounds its inner lerps, which can add up to 1/2 more
# (BOARD_LERP_ROUNDING).
FILTER_STEP = Q(1, 256)
PERSPECTIVE_ERROR = Q(1, 2048)
RAMP_BOUND = 1 + 255 * (FILTER_STEP + PERSPECTIVE_ERROR)
BOARD_WEIGHT_ERROR = (RAMP_BOUND + 1) / 255
REFERENCE_WEIGHT_ERROR = Q(1, 512) + Q(1, 2048)
BOARD_LERP_ROUNDING = Q(1, 2)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


draw = module('gpu_conformance_draw', ROOT / 'apps/gpu-render/draw.py')
texture = module('gpu_conformance_texture', ROOT / 'apps/gpu-render/texture.py')


def job_error_result():
    """PVR_JOB_RESULT from the driver's enum, so the number cannot drift."""
    text = (ROOT / 'include/pvr_compute_job.h').read_text()
    names = re.search(r'enum pvr_compute_job_error\s*\{([^}]*)\}', text).group(1)
    return [n.strip() for n in names.split(',')].index('PVR_JOB_RESULT')


class Refused(ValueError):
    """A scene or launch the comparison cannot judge."""


# ---------------------------------------------------------------------------
# Scenes

@dataclass
class Quad:
    """Axis-aligned rectangle with pixel-boundary edges; attributes vary along
    one axis each, so the closed-form evaluator can compute it exactly."""
    x0: int
    y0: int
    x1: int
    y1: int
    u: tuple = (Q(1, 2), Q(1, 2))     # u at x0 and x1 edges
    v: tuple = (Q(1, 2), Q(1, 2))     # v at y0 and y1 edges
    z: Q = Q(0)
    w: tuple = (1, 1)                 # clip w at x0 and x1 edges
    colour: tuple = None              # triangle profile: RGBA at x0 and x1 edges
    clockwise: bool = True
    confirm: bool = False             # board confirmation scene, compared with the exact value


@dataclass
class Tri:
    """Triangle in clip coordinates: rows (x, y, z, w, attributes...)."""
    rows: tuple


@dataclass
class Batch:
    prims: list
    state: int = 0
    stencil: int = 0


@dataclass
class Frame:
    name: str
    profile: int                       # 1 triangle colour, 2-4 indexed texture
    width: int
    height: int
    batches: list
    state: int = 0                     # profile 3 render state or profile 4 frame state
    texels: tuple = None               # (width, height, RGBA bytes)

    @property
    def stride(self):
        return (self.width + 1) & ~1 if self.profile > 1 else self.width

    @property
    def closed_form(self):
        return all(isinstance(p, Quad) for b in self.batches for p in b.prims)

    # A scene is fixed once built, so its device input, plan analysis and
    # exact evaluation are computed on first use and kept with the frame.
    @functools.cached_property
    def expanded(self):
        """Draw-ordered (batch, triangle) pairs and the canonical device input."""
        return device_input(self)

    @functools.cached_property
    def analysis(self):
        """The plan checks; raises Refused on the first violation."""
        return plan_checks(self)

    @functools.cached_property
    def exact(self):
        """One exact evaluation of a quad-only scene: its UNORM8 image, the
        stencil executions it traces and its confirmation pixels."""
        trace = set()
        colours = closed_form_colours(self, trace=trace)
        return Exact(unorm_image(self, colours), frozenset(trace), confirmation_pixels(self, colours))

    @property
    def confirmation(self):
        """Exact values and bounds of the confirmation pixels."""
        return self.exact.confirmation


@dataclass
class Exact:
    image: bytes                       # UNORM8 at the device stride
    trace: frozenset                   # closed_form_colours() stencil trace
    confirmation: list                 # confirmation_pixels()


def pattern(n, seed, alpha=None):
    """n deterministic RGBA texels."""
    out = bytearray()
    counter = 0
    while len(out) < 4 * n:
        out += hashlib.sha256(f'{seed}:{counter}'.encode()).digest()
        counter += 1
    out = out[:4 * n]
    if alpha is not None:
        out[3::4] = bytes([alpha]) * n
    return bytes(out)


def ndc(pixel, size):
    return Q(2 * pixel, size) - 1


def texel_centre(i, n):
    return Q(2 * i + 1, 2 * n)


def blend_state(eq):
    return eq << 6


def depth_state(op, write=False):
    return op | (8 if write else 0)


def stencil_state(compare, fail, zfail, passed):
    return 1 << 10 | compare << 11 | fail << 14 | zfail << 17 | passed << 20


NO_COLOUR = 1 << 9
ALWAYS, NEVER, LESS, EQUAL, LEQUAL, GREATER, NEQUAL, GEQUAL = 7, 0, 1, 2, 3, 4, 5, 6
KEEP, ZERO, REPLACE, INCR, DECR, INVERT, INCR_WRAP, DECR_WRAP = range(8)


# Signs of a - b that pass each VkCompareOp.
COMPARE_PASSES = {NEVER: (), LESS: (-1,), EQUAL: (0,), LEQUAL: (-1, 0), GREATER: (1,), NEQUAL: (-1, 1),
                  GEQUAL: (0, 1), ALWAYS: (-1, 0, 1)}


def compare_op(op, a, b):
    return (a > b) - (a < b) in COMPARE_PASSES[op]


def stencil_apply(op, s, ref):
    return [s, 0, ref, min(s + 1, 255), max(s - 1, 0), ~s & 255, (s + 1) & 255, (s - 1) & 255][op]


def stencil_result(s, ref, state, word, depth_pass):
    """Stored stencil after one fragment of a stencil-enabled batch."""
    cmask, wmask = (word >> 8) & 255, (word >> 16) & 255
    if not compare_op((state >> 11) & 7, ref & cmask, s & cmask):
        op = (state >> 14) & 7
    elif not depth_pass:
        op = (state >> 17) & 7
    else:
        op = (state >> 20) & 7
    return (s & ~wmask | stencil_apply(op, s, ref) & wmask) & 255


# Triangle-colour profile, 64 x 64 -----------------------------------------

def colour_frames():
    def flat(points, c):
        return Tri(tuple((x, y, 0.0, 1.0) + c for x, y in points))
    k = 2 ** -9
    p = [(-0.75, -0.75), (0.75, -0.75), (0.75, 0.75), (-0.75, 0.75), (0.0625, -0.09375)]
    frames = [
        Frame('triangle-background', 1, 64, 64, [Batch([flat([(0.0, 0.0)] * 3, (1.0, 0.0, 0.0, 1.0))])]),
        Frame('triangle-flat-shared-edges', 1, 64, 64, [Batch([
            flat([p[0], p[1], p[4]], (64 / 255, 32 / 255, 200 / 255, 1.0)),
            flat([p[1], p[2], p[4]], (250 / 255, 10 / 255, 90 / 255, 1.0)),
            flat([p[2], p[3], p[4]], (17 / 255, 230 / 255, 140 / 255, 1.0))])]),
    ]
    rim = []
    for i in range(42):
        a = 2 * math.pi * i / 42
        rim.append((round(0.875 * math.cos(a) / k) * k, round(0.875 * math.sin(a) / k) * k))
    fan = []
    for i in range(42):
        c0 = (1.0, 1.0, 1.0, 1.0)
        c1 = (i / 41, 1 - i / 41, (i * 7 % 42) / 41, 1.0)
        c2 = ((i + 1) / 42, (41 - i) / 42, 0.5, 1.0)
        a, b = rim[i], rim[(i + 1) % 42]
        fan.append(Tri(((0.0, 0.0, 0.0, 1.0) + c0, (a[0], a[1], 0.0, 1.0) + c1, (b[0], b[1], 0.0, 1.0) + c2)))
    frames.append(Frame('triangle-fan-126', 1, 64, 64, [Batch(fan)]))
    frames.append(Frame('triangle-degenerate', 1, 64, 64,
                        [Batch([flat([(-0.5, -0.5), (0.0, 0.0), (0.5, 0.5)], (0.0, 1.0, 0.0, 1.0))])]))
    return frames


def colour_host_frames():
    """Closed-form interpolated colour, checked on the host only."""
    return [Frame('triangle-gradient', 1, 64, 64, [Batch([
        Quad(8, 8, 56, 56, colour=((0.0, 0.25, 1.0, 1.0), (1.0, 0.75, 0.0, 1.0))),
        Quad(8, 58, 56, 63, colour=((1.0, 0.0, 0.5, 1.0), (0.0, 1.0, 0.5, 1.0)), clockwise=False)])])]


# Profile 2: 255 x 129, stride 256 -----------------------------------------

def profile2_frames():
    w, h = 255, 129
    full = pattern(w * h, 'p2-full')
    frames = [Frame('p2-copy', 2, w, h, [Batch([Quad(0, 0, w, h, (Q(0), Q(1)), (Q(0), Q(1)))])], texels=(w, h, full)),
              Frame('p2-mirror', 2, w, h, [Batch([Quad(0, 0, w, h, (Q(1), Q(0)), (Q(0), Q(1)))])], texels=(w, h, full))]
    for tw, th in ((15, 3), (51, 43), (85, 129)):
        frames.append(Frame(f'p2-scale-{tw}x{th}', 2, w, h, [Batch([Quad(0, 0, w, h, (Q(0), Q(1)), (Q(0), Q(1)))])],
                            texels=(tw, th, pattern(tw * th, f'p2-{tw}'))))
    # Triangles on the 1/8 NDC lattice; texture coordinates follow position,
    # so every sample lies at a texel centre.
    centre = (1 / 8, -1 / 4)
    rim = [(-7 / 8, -7 / 8), (0.0, -1.0), (7 / 8, -6 / 8), (1.0, 1 / 8), (5 / 8, 7 / 8), (-2 / 8, 1.0),
           (-1.0, 4 / 8), (-6 / 8, -2 / 8)]
    tris = []
    for i in range(len(rim)):
        pts = [centre, rim[i], rim[(i + 1) % len(rim)]]
        tris.append(Tri(tuple((x, y, 0.0, 1.0, (x + 1) / 2, (y + 1) / 2) for x, y in pts)))
    frames.append(Frame('p2-lattice-triangles', 2, w, h, [Batch(tris)], texels=(w, h, full)))
    return frames


# Profile 3: 256 x 256 ------------------------------------------------------

def profile3_frames():
    tw = th = 4
    tex = bytearray(pattern(16, 'p3', alpha=255))
    for i, a in ((7, 0x80), (8, 0x40), (9, 0), (10, 0xc0)):
        tex[4 * i + 3] = a
    tex = bytes(tex)

    def solid(x0, y0, x1, y1, t, z=Q(0)):
        return Quad(x0, y0, x1, y1, (texel_centre(t % 4, 4),) * 2, (texel_centre(t // 4, 4),) * 2, z=z)
    frames = []
    for depth in (1, 2, 3):
        for write in (0, 1):
            prims = [solid(16, 16, 240, 128, 0, Q(1, 2)),
                     solid(32, 32, 96, 112, 1, Q(1, 4)), solid(96, 32, 160, 112, 2, Q(1, 2)),
                     solid(160, 32, 224, 112, 3, Q(3, 4)),
                     solid(16, 144, 240, 176, 4, Q(1)),
                     solid(32, 64, 224, 96, 5, Q(3, 8)),
                     solid(16, 192, 240, 240, 6, Q(0))]
            frames.append(Frame(f'p3-depth{depth}-write{write}', 3, 256, 256, [Batch(prims)],
                                state=depth | write << 2, texels=(tw, th, tex)))
    frames.append(Frame('p3-source-over', 3, 256, 256, [Batch([
        solid(16, 16, 112, 112, 7), solid(144, 16, 240, 112, 8), solid(16, 144, 240, 240, 9)])],
        state=8, texels=(tw, th, tex)))
    frames.append(Frame('p3-source-over-depth', 3, 256, 256, [Batch([
        solid(16, 16, 112, 112, 7, Q(1)), solid(144, 16, 240, 112, 10, Q(1, 2)),
        solid(16, 144, 240, 240, 11, Q(1, 4))])],
        state=10, texels=(tw, th, tex)))
    return frames


# Profile 4: 128 x 128 ------------------------------------------------------

ADDRESS_PAIRS = ((0, 1), (1, 2), (2, 3), (3, 4), (4, 0), (1, 4))
FILTERS = ('nearest', 'srgb', 'linear')
# Stencil test batch per frame: (compare, fail, depth fail, pass). Against
# reference 0x5a, the start values 0, 0x5a and 0xff are greater, equal and
# less, so a compare operation sends each column down a fixed subset of the
# three paths. The table is chosen so that, over the 18 frames, each of the
# eight operations runs on every path on start values that separate it from
# the other seven operations (for example DECR from DECR_WRAP only at 0, INCR
# from INCR_WRAP only at 0xff), and every compare operation runs unmasked.
# stencil_separation() derives this from the closed-form evaluation.
STENCIL_TESTS = ((0, 0, 1, 2), (1, 4, 4, 5), (2, 6, 5, 1), (3, 2, 6, 3), (4, 3, 5, 6), (5, 7, 0, 1),
                 (6, 2, 7, 4), (7, 5, 2, 0), (0, 5, 1, 2), (1, 7, 4, 5), (2, 6, 1, 5), (3, 1, 2, 3),
                 (4, 6, 5, 6), (5, 1, 3, 6), (6, 2, 4, 7), (7, 5, 0, 2), (0, 0, 1, 2), (7, 3, 4, 5))
# Frame 2 compares EQUAL under mask 0xa5, which makes start 0 equal to the
# reference; frames 5, 11 and 17 write through mask 0x0f.
STENCIL_COMPARE_MASK = {2: 0xa5}
STENCIL_WRITE_MASK = {5: 0x0f, 11: 0x0f, 17: 0x0f}
STENCIL_REF = 0x5a


def profile4_texture():
    """8 x 8 texels. Columns 0 and 1 of rows 0 and 1 are black and white, so a
    linear ramp between texels 0 and 1 reads its filter weight directly."""
    tex = bytearray(pattern(64, 'p4', alpha=255))
    for t, value in ((0, 0), (1, 255), (8, 0), (9, 255)):
        tex[4 * t:4 * t + 4] = bytes((value, value, value, 255))
    tex[4 * 60 + 3] = 0x80          # translucent texel for blending
    return 8, 8, bytes(tex)


def profile4_frame(index):
    filt, pair = FILTERS[index // 6], ADDRESS_PAIRS[index % 6]
    tw, th, tex = profile4_texture()
    linear, srgb = filt == 'linear', filt == 'srgb'
    state = 16 | linear << 5 | pair[0] << 6 | pair[1] << 9 | srgb << 12

    def solid(x0, y0, x1, y1, t, z=Q(0), clockwise=True, confirm=False):
        return Quad(x0, y0, x1, y1, (texel_centre(t % 8, 8),) * 2, (texel_centre(t // 8, 8),) * 2, z=z,
                    clockwise=clockwise, confirm=confirm)
    w = {'nearest': (1, 1), 'srgb': (2, 2), 'linear': (4, 4)}[filt]
    batches = []
    # Address modes and filtering: 2 pixels per texel over u in [-2, 2].
    batches.append(Batch([Quad(0, 0, 64, 63, (Q(-2), Q(2)), (Q(-2), Q(-2) + Q(63, 32)), w=w)],
                         depth_state(ALWAYS)))
    # Depth reference plane at 0.5, and the blend underlay.
    batches.append(Batch([solid(64, 0, 128, 48, 1, Q(1, 2)), solid(0, 64, 32, 128, 2, Q(1, 2))],
                         depth_state(ALWAYS, True)))
    # Two compare operations against bands below, at and above the plane.
    for k, op in enumerate((index % 8, (index + 4) % 8)):
        x = 64 + 32 * k
        batches.append(Batch([solid(x, 0, x + 32, 16, 3 + k, Q(1, 4)), solid(x, 16, x + 32, 32, 3 + k, Q(1, 2)),
                              solid(x, 32, x + 32, 48, 3 + k, Q(3, 4))], depth_state(op)))
    # Stencil: columns start at 0, 0x5a and 0xff; the lower row fails depth.
    cols = ((64, 85), (85, 106), (106, 128))
    rows = ((48, 72), (72, 96))
    setup = stencil_state(ALWAYS, KEEP, KEEP, REPLACE) | NO_COLOUR | depth_state(ALWAYS)
    batches.append(Batch([solid(85, 48, 106, 96, 0)], setup, 0x5a | 0xff << 16))
    batches.append(Batch([solid(106, 48, 128, 96, 0)], setup, 0xff | 0xff << 16))
    batches.append(Batch([solid(64, 72, 128, 96, 0, Q(1, 4))], depth_state(ALWAYS, True) | NO_COLOUR))
    test = stencil_state(*STENCIL_TESTS[index]) | depth_state(LESS)
    word = STENCIL_REF | STENCIL_COMPARE_MASK.get(index, 0xff) << 8 | STENCIL_WRITE_MASK.get(index, 0xff) << 16
    batches.append(Batch([solid(64, 48, 128, 96, 5, Q(1, 2))], test, word))
    observe = []
    for (x0, x1), start in zip(cols, (0, 0x5a, 0xff)):
        for (y0, y1), depth_pass in zip(rows, (True, False)):
            expected = stencil_result(start, STENCIL_REF, test, word, depth_pass)
            observe.append(Batch([solid(x0, y0, x0 + 10, y1, 6)],
                                 stencil_state(EQUAL, KEEP, KEEP, KEEP) | depth_state(ALWAYS), expected | 0xff << 8))
    batches += observe
    # Culling: one clockwise and one counter-clockwise quad.
    batches.append(Batch([solid(64, 96, 96, 127, 7), solid(96, 96, 128, 127, 8, clockwise=False)],
                         depth_state(ALWAYS) | (index % 3) << 4))
    if filt == 'nearest':
        # Blending confirmation: one translucent layer over the underlay and
        # the background.
        eq = (1, 2, 3, 4, 1, 2)[index % 6]
        batches.append(Batch([solid(16, 80, 48, 112, 60, confirm=True)], depth_state(ALWAYS) | blend_state(eq)))
    elif filt == 'linear':
        # Filter confirmation between the black texel 0 and the white texel 1.
        # A perspective ramp with w from 1 to 4, then four constant-w bands of
        # a quarter texel over 64 pixels each, so the 256 pixel centres of the
        # bands fall midway between consecutive multiples of 2^-8 texel.
        ramp = [Quad(0, 64, 64, 96, (texel_centre(0, 8), texel_centre(1, 8)), (texel_centre(0, 8),) * 2,
                     w=(1, 4), confirm=True)]
        for k in range(4):
            ramp.append(Quad(0, 96 + 8 * k, 64, 104 + 8 * k, ((Q(k, 4) + Q(1, 2)) / 8, (Q(k + 1, 4) + Q(1, 2)) / 8),
                             (texel_centre(0, 8),) * 2, confirm=True))
        batches.append(Batch(ramp, depth_state(ALWAYS)))
    else:
        batches.append(Batch([solid(16, 80, 48, 112, 9)], depth_state(ALWAYS)))
    assert len(batches) == 16
    return Frame(f'p4-{filt}-u{pair[0]}v{pair[1]}', 4, 128, 128, batches, state=state, texels=(tw, th, tex))


def profile4_frames():
    return [profile4_frame(i) for i in range(18)]


# Profile 4 maximum width ---------------------------------------------------

def wide_frame(height=384):
    """1,000 non-overlapping linear-filtered triangles in a 50 x 10 grid."""
    width = 1920
    tw = th = 64
    tex = bytearray()
    for y in range(th):
        for x in range(tw):
            tex += bytes((round(128 + 60 * math.sin(2 * math.pi * x / 64)),
                          round(128 + 60 * math.cos(2 * math.pi * y / 64)),
                          round(128 + 40 * math.sin(2 * math.pi * (x + y) / 64)), 255))
    unit_x = lattice_unit(width)
    unit_y = lattice_unit(height)
    cell_x = int((width - 40) / 50 / (unit_x * width / 2))
    cell_y = int((height - 1) / 10 / (unit_y * height / 2))
    tris = []
    for row in range(10):
        for col in range(50):
            x0, x1 = -1 + col * cell_x * unit_x, -1 + (col + 1) * cell_x * unit_x
            y0, y1 = -1 + row * cell_y * unit_y, -1 + (row + 1) * cell_y * unit_y
            offset = ((row * 50 + col) % 8) / 8
            for pts in (((x0, y0), (x1, y0), (x1, y1)), ((x0, y0), (x1, y1), (x0, y1))):
                tris.append(Tri(tuple((float(x), float(y), 0.0, 1.0, float(3 * x), float(2 * y + offset))
                                      for x, y in pts)))
    state = 1 | 1 << 5 | 0 << 6 | 0 << 9
    return Frame(f'p4-wide-{width}x{height}', 4, width, height, [Batch(tris, depth_state(ALWAYS))],
                 state=state, texels=(tw, th, bytes(tex)))


def lattice_unit(size):
    """Finest power-of-two NDC step whose window positions lie on the 1/16 lattice."""
    step = Q(1)
    while (step / 2 * size / 2 * LATTICE).denominator == 1:
        step /= 2
    return step


# Compute -------------------------------------------------------------------

M32 = 0xffffffff


def words(seed, n, low=0, high=M32):
    raw = pattern(n, seed)
    return [low + int.from_bytes(raw[4 * i:4 * i + 4], 'little') % (high - low + 1) for i in range(n)]


@dataclass
class Kernel:
    name: str
    source: str
    profile: int
    header_bytes: int
    input_element: int
    output_element: int
    inputs: list
    expected: object     # function(input words) -> output words

    def count(self, data):
        return struct.unpack_from('<I', data)[0]


def compute_kernels():
    def probe(w):
        return [((w[0] + (lane + 1) * w[1]) ^ (0x9e3779b9 + (lane << 16))) & M32 for lane in range(32)]

    def add(w):
        n = w[0]
        return [(w[1 + i] + w[1 + n + i]) & M32 for i in range(n)]

    def affine(w):
        n, a, b = w[:3]
        return [(w[3 + i] * a + b) & M32 for i in range(n)]

    def transpose(w):
        n, width, height = w[:3]
        out = [0] * n
        for y in range(height):
            for x in range(width):
                out[x * height + y] = w[3 + y * width + x]
        return out

    def neighbour(w):
        n, width, height = w[:3]
        out = []
        for i in range(n):
            x = i % width
            v = w[3 + i]
            if x > 0:
                v += w[3 + i - 1]
            if x + 1 < width:
                v += w[3 + i + 1]
            out.append(v & M32)
        return out

    def pack(values):
        return struct.pack(f'<{len(values)}I', *values)
    kernels = [Kernel('probe', 'pvr_compute_probe.cl', 1, 4, 4, 128,
                      [pack([1, f]) for f in (3, 0x10001, 0xfffffff1)], probe)]
    for name, source, profile in (('vector-add', 'pvr_vector_add.cl', 1),
                                  ('vector-add-dispatch', 'pvr_vector_add_dispatch.cl', 2)):
        kernels.append(Kernel(name, source, profile, 4, 8, 4,
                              [pack([511] + words(f'{name}{k}', 1022)) for k in range(3)], add))
    for name, source, profile in (('vector-affine', 'pvr_vector_affine.cl', 1),
                                  ('vector-affine-dispatch', 'pvr_vector_affine_dispatch.cl', 2)):
        kernels.append(Kernel(name, source, profile, 12, 4, 4,
                              [pack([992] + words(f'{name}{k}', 2) + words(f'{name}v{k}', 992)) for k in range(3)],
                              affine))
    kernels.append(Kernel('grid-transpose', 'pvr_grid_transpose.cl', 3, 12, 4, 4,
                          [pack([960, 40, 24] + words(f'transpose{k}', 960)) for k in range(3)], transpose))
    kernels.append(Kernel('grid-neighbour', 'pvr_grid_neighbor.cl', 3, 12, 4, 4,
                          [pack([961, 31, 31] + words(f'neighbour{k}', 961, 0, 0x3fffffff)) for k in range(3)],
                          neighbour))
    return kernels


def compute_plan(kernel):
    """Check package geometry and sentinels; return expected output words per input."""
    expected = []
    first = kernel.count(kernel.inputs[0])
    for data in kernel.inputs:
        n = kernel.count(data)
        if n != first or len(data) != kernel.header_bytes + n * kernel.input_element or len(data) > 4096:
            raise Refused(f'{kernel.name}: input differs from its package geometry')
        out_bytes = n * kernel.output_element
        if not 0 < out_bytes <= 3968:
            raise Refused(f'{kernel.name}: output extent')
        if kernel.profile == 2 and n > 992:
            raise Refused(f'{kernel.name}: dispatch extent')
        if kernel.profile == 3:
            _, width, height = struct.unpack_from('<3I', data)
            local = (8, 4) if 'transpose' in kernel.source else (32, 1)
            groups = -(-width // local[0]) * -(-height // local[1])
            if width * height != n or n > 992 or groups > 31:
                raise Refused(f'{kernel.name}: grid extent')
        values = kernel.expected(list(struct.unpack(f'<{len(data) // 4}I', data)))
        if len(values) * 4 != out_bytes:
            raise Refused(f'{kernel.name}: expression extent')
        if COMPUTE_FILL in values:
            raise Refused(f'{kernel.name}: an expected word equals the output fill 0x{COMPUTE_FILL:08x}')
        expected.append(values)
    return expected


def compare_compute(kernel, expected, capture):
    """Compare recorded compute outputs; capture is a list of dicts with
    input_sha256, result, job_error and words."""
    report = {'launch': f'compute-{kernel.name}', 'inputs': []}
    passed = len(capture) == len(kernel.inputs)
    for k, (data, want) in enumerate(zip(kernel.inputs, expected)):
        record = capture[k] if k < len(capture) else None
        entry = {'input': k}
        if record is None:
            entry['status'] = 'missing'
        elif record['input_sha256'] != hashlib.sha256(data).hexdigest():
            entry['status'] = 'input hash differs from the plan'
        elif record.get('job_error') == job_error_result():
            entry['status'] = 'out-of-range write: the driver guard check failed the job with PVR_JOB_RESULT'
        elif record.get('result') != 1:
            entry['status'] = f'failed at phase {record.get("phase")} with job error {record.get("job_error")}'
        else:
            got = record['words']
            bad = [i for i in range(max(len(got), len(want))) if i >= len(got) or i >= len(want) or got[i] != want[i]]
            entry.update(words=len(want), failed=len(bad),
                         failures=[[i, want[i] if i < len(want) else None, got[i] if i < len(got) else None]
                                   for i in bad[:16]])
            entry['status'] = 'pass' if not bad else 'mismatch'
        passed &= entry['status'] == 'pass'
        report['inputs'].append(entry)
    report['passed'] = passed
    return report


# ---------------------------------------------------------------------------
# Device input

def is_f32(q):
    try:
        d = float(q)
        return Q(d) == q and Q(struct.unpack('<f', struct.pack('<f', d))[0]) == q
    except OverflowError:
        return False


def window(frame, x, y, w):
    """Exact window position of a clip-space vertex, checked against the fp32
    viewport transform and the 1/16 lattice."""
    out = []
    for c, size in ((x, frame.width), (y, frame.height)):
        d = Q(c) / Q(w)
        half = Q(size, 2)
        scaled = d * half
        pos = scaled + half
        if not (is_f32(d) and is_f32(scaled) and is_f32(pos)) or (pos * LATTICE).denominator != 1:
            raise Refused(f'{frame.name}: vertex ({float(x)}, {float(y)}, w={float(w)}) is off the '
                          f'1/{LATTICE}-pixel lattice or not reproduced by the fp32 viewport transform')
        out.append(pos)
    return out


def quad_vertex(frame, quad, X, Y):
    """Clip-space row for window point (X, Y) of a quad."""
    t = (X - quad.x0) / (quad.x1 - quad.x0)
    s = (Y - quad.y0) / (quad.y1 - quad.y0)
    if quad.w[0] != quad.w[1] and t not in (0, 1):
        raise ValueError('perspective quads split only at corners')
    w = quad.w[0] if t == 0 else quad.w[1] if t == 1 else quad.w[0]
    x, y = (2 * X / frame.width - 1) * w, (2 * Y / frame.height - 1) * w
    pos = (float(x), float(y), float(quad.z * w), float(w))
    if frame.profile == 1:
        c0, c1 = quad.colour
        return pos + tuple(float(Q(a) + (Q(b) - Q(a)) * t) for a, b in zip(c0, c1))
    u = quad.u[0] if t == 0 else quad.u[1] if t == 1 else quad.u[0] + (quad.u[1] - quad.u[0]) * t
    v = quad.v[0] + (quad.v[1] - quad.v[0]) * s
    return pos + (float(u), float(v))


def quad_triangles(frame, quad):
    """Split a quad into triangles whose edges pass through no pixel centre."""
    a, b = (quad.x0, quad.y0), (quad.x1, quad.y0)
    c, d = (quad.x1, quad.y1), (quad.x0, quad.y1)
    candidates = [[(a, b, c), (a, c, d)], [(a, b, d), (b, c, d)]]
    if quad.w[0] == quad.w[1]:
        step = Q(1, LATTICE)
        for k in range(1, (quad.x1 - quad.x0) * LATTICE):
            e = (quad.x0 + k * step, Q(quad.y0))
            candidates.append([(e, b, c), (e, c, d), (e, d, a)])
    for split in candidates:
        pts = [tuple((Q(p[0]) * 32, Q(p[1]) * 32) for p in tri) for tri in split]
        if any(x.denominator != 1 or y.denominator != 1 for tri in pts for x, y in tri):
            continue
        try:
            for tri in split:
                for X, Y in tri:
                    row = quad_vertex(frame, quad, Q(X), Q(Y))
                    window(frame, row[0], row[1], row[3])
        except Refused:
            continue
        ties = 0
        for tri in pts:
            ints = [(int(x), int(y)) for x, y in tri]
            for _, lo, hi, lo_n, hi_n in spans(*ints, frame.width, frame.height)[1]:
                ties += tie_count(lo, hi, lo_n, hi_n)
        if not ties:
            rows = []
            for tri in split:
                tri_rows = [quad_vertex(frame, quad, Q(X), Q(Y)) for X, Y in tri]
                if orientation(frame, tri_rows) != (1 if quad.clockwise else -1):
                    tri_rows = [tri_rows[0], tri_rows[2], tri_rows[1]]
                rows.append(Tri(tuple(tri_rows)))
            return rows
    raise Refused(f'{frame.name}: no tie-free split of quad {quad.x0},{quad.y0}-{quad.x1},{quad.y1}')


def orientation(frame, rows):
    """+1 for a clockwise triangle in framebuffer coordinates (y down)."""
    p = [window(frame, r[0], r[1], r[3]) for r in rows]
    s = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[1][1] - p[0][1]) * (p[2][0] - p[0][0])
    return (s > 0) - (s < 0)


def device_input(frame):
    """Draw-ordered triangles with their batch, and the canonical device input."""
    triangles = []
    for b, batch in enumerate(frame.batches):
        for prim in batch.prims:
            for tri in (quad_triangles(frame, prim) if isinstance(prim, Quad) else [prim]):
                triangles.append((b, tri))
    if frame.profile == 1:
        rows = [list(r) for _, t in triangles for r in t.rows]
        device = {'data': draw.vertices(rows), 'state': 0}
    else:
        vertices = [list(r) for _, t in triangles for r in t.rows]
        indices = list(range(len(vertices)))
        records, first = [], 0
        for b, batch in enumerate(frame.batches):
            n = 3 * sum(1 for bb, _ in triangles if bb == b)
            records.append((first, n, batch.state, batch.stencil))
            first += n
        tw, th, tex = frame.texels
        result = texture.input_bytes(vertices, indices, tw, th, tex, profile=frame.profile, state=frame.state,
                                     batches=records if frame.profile == 4 else ())
        shape, data = result[:2]
        device = {'data': data, 'shape': shape, 'state': frame.state}
    return triangles, device


def input_sha256(frame):
    return hashlib.sha256(frame.expanded[1]['data']).hexdigest()


# ---------------------------------------------------------------------------
# Plan checks

def spans(a, b, c, width, height):
    """Coverage of a triangle given in 1/32-pixel integer window units.

    Returns (sign, rows) with rows (j, lo, hi, lo_tie, hi_tie): pixels lo..hi
    are strictly inside; lo_tie..hi_tie also include centres on an edge."""
    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    if area == 0:
        return 0, []
    s = 1 if area > 0 else -1
    coefs = []
    for u, v in ((b, c), (c, a), (a, b)):
        coefs.append((-s * (v[1] - u[1]), s * (v[0] - u[0]), s * ((v[1] - u[1]) * u[0] - (v[0] - u[0]) * u[1])))
    ys = (a[1], b[1], c[1])
    j0 = max(0, -((16 - min(ys)) // 32))
    j1 = min(height - 1, (max(ys) - 16) // 32)
    rows = []
    for j in range(j0, j1 + 1):
        py = 32 * j + 16
        lo, hi, lo_n, hi_n = 0, width - 1, 0, width - 1
        for A, B, C in coefs:
            slope, base = 32 * A, 16 * A + B * py + C
            if slope == 0:
                if base < 0:
                    lo_n, hi_n, lo, hi = 1, 0, 1, 0
                elif base == 0:
                    lo, hi = 1, 0
                continue
            if slope > 0:
                lo_n = max(lo_n, -((base) // slope))
                lo = max(lo, (-base) // slope + 1)
            else:
                hi_n = min(hi_n, base // -slope)
                hi = min(hi, -(-base // -slope) - 1)
        if lo_n <= hi_n:
            rows.append((j, lo, hi, lo_n, hi_n))
    return s, rows


def tie_count(lo, hi, lo_n, hi_n):
    if lo > hi:
        return hi_n - lo_n + 1
    return (lo - lo_n) + (hi_n - hi)


def triangle_class(frame, batch):
    """Tolerance class of a colour-writing triangle: 0 exact, 1 +-1, 2 linear."""
    if frame.profile == 1:
        return 1
    if frame.profile == 2:
        return 0
    if frame.profile == 3:
        return 1 if frame.state & 8 else 0
    if (batch.state >> 6) & 7:
        return 1
    if frame.state & 32:
        return 2
    return 1 if frame.state & (1 << 12) else 0


def texel_delta(frame):
    """Largest channel difference between texels that can be bilinear neighbours."""
    tw, th, tex = frame.texels
    modes = ((frame.state >> 6) & 7, (frame.state >> 9) & 7)

    def at(x, y):
        x, y = wrap(x, tw, modes[0]), wrap(y, th, modes[1])
        if x is None or y is None:
            return (0, 0, 0, 0)
        return tex[4 * (y * tw + x):4 * (y * tw + x) + 4]
    delta = 0
    for y in range(-1, th + 1):
        for x in range(-1, tw + 1):
            p = at(x, y)
            for dx, dy in ((1, 0), (0, 1), (1, 1), (1, -1)):
                q = at(x + dx, y + dy)
                delta = max(delta, max(abs(p[c] - q[c]) for c in range(4)))
    return delta


def wrap(i, n, mode):
    if mode == 0:
        return i % n
    if mode == 1:
        m = i % (2 * n)
        return m if m < n else 2 * n - 1 - m
    if mode == 2:
        return min(max(i, 0), n - 1)
    if mode == 3:
        return i if 0 <= i < n else None
    j = i if i >= 0 else -1 - i
    return min(j, n - 1)


@dataclass
class Analysis:
    classes: bytes
    linear_tol: int
    ties: bytes
    covered: int
    tie_pixels: int


def plan_checks(frame):
    """Apply the plan checks to one frame; raise Refused on the first violation."""
    triangles, device = frame.expanded
    W, H = frame.width, frame.height
    p4 = frame.profile == 4
    linear = p4 and frame.state & 32
    srgb = p4 and frame.state & (1 << 12)
    if linear and srgb:
        raise Refused(f'{frame.name}: linear filtering of sRGB texels composes two inexact stages')
    covered = bytearray(W * H)
    ties = bytearray(W * H)
    top = bytearray(W * H)              # class of the value on top: 0 exact, 1 +-1, 2 linear
    blended = (frame.profile == 3 and frame.state & 8) or (p4 and any((b.state >> 6) & 7 for b in frame.batches))
    depth_surfaces = {}
    classes = bytearray()
    for b, tri in triangles:
        batch = frame.batches[b]
        cls = triangle_class(frame, batch)
        classes.append(cls)
        rows = tri.rows
        if p4:
            for r in rows:
                if r[3] not in (1.0, 2.0, 4.0):
                    raise Refused(f'{frame.name}: profile 4 vertex w {r[3]} is not 1, 2 or 4')
                if abs(r[4]) > 4 or abs(r[5]) > 4:
                    raise Refused(f'{frame.name}: texture coordinate ({r[4]}, {r[5]}) exceeds magnitude 4')
        pos = [window(frame, r[0], r[1], r[3]) for r in rows]
        pts = [(int(x * 32), int(y * 32)) for x, y in pos]
        sign, span_rows = spans(*pts, W, H)
        if not sign:
            continue
        cull = (batch.state >> 4) & 3 if p4 else 0
        if cull == 1 and sign > 0 or cull == 2 and sign < 0:
            continue
        colour_write = not (p4 and batch.state & NO_COLOUR)
        blend = (p4 and (batch.state >> 6) & 7) or (frame.profile == 3 and frame.state & 8)
        depth_relevant = (frame.profile == 3 and frame.state & 3) or (p4 and (batch.state & 7 != ALWAYS or
                                                                             batch.state & 8))
        nearest = frame.profile > 1 and not linear
        # Edge-function coefficients for barycentric numerators, signed so
        # every numerator is positive inside.
        a, bb, c = pts
        coefs = []
        for u, v in ((bb, c), (c, a), (a, bb)):
            coefs.append((-sign * (v[1] - u[1]), sign * (v[0] - u[0]),
                          sign * ((v[1] - u[1]) * u[0] - (v[0] - u[0]) * u[1])))
        if nearest:
            q = [4 // int(r[3]) if p4 else 1 for r in rows]
            uvs = [(Q(r[4]), Q(r[5])) for r in rows]
            den = math.lcm(*(x.denominator for uv in uvs for x in uv))
            tw, th, _ = frame.texels
            num_u = [q[k] * int(uvs[k][0] * den) * tw for k in range(3)]
            num_v = [q[k] * int(uvs[k][1] * den) * th for k in range(3)]
        if depth_relevant:
            zs = [Q(r[2]) / Q(r[3]) for r in rows]
            plane = zs[0] if zs[0] == zs[1] == zs[2] else None
            key = ('plane',) if plane is not None else ('tri', tuple(sorted((r[0], r[1], r[2], r[3]) for r in rows)))
        for j, lo, hi, lo_n, hi_n in span_rows:
            base = j * W
            covered[base + lo_n:base + hi_n + 1] = b'\1' * (hi_n - lo_n + 1)
            if lo > hi:
                for i in range(lo_n, hi_n + 1):
                    ties[base + i] = 1
            else:
                for i in list(range(lo_n, lo)) + list(range(hi + 1, hi_n + 1)):
                    ties[base + i] = 1
            if not (nearest or depth_relevant or (blended and colour_write)):
                continue
            py = 32 * j + 16
            for i in range(lo_n, hi_n + 1):
                px = 32 * i + 16
                e = [A * px + B * py + C for A, B, C in coefs]
                if nearest:
                    d = den * sum(e[k] * q[k] for k in range(3))
                    if d > 0:
                        for nums in (num_u, num_v):
                            n = sum(e[k] * nums[k] for k in range(3))
                            r = n % d
                            if 128 * min(r, d - r) < d:
                                raise Refused(f'{frame.name}: nearest sample at pixel ({i}, {j}) lies within '
                                              f'2^-7 texel of a texel boundary')
                if depth_relevant:
                    z = plane if plane is not None else sum(e[k] * zs[k] for k in range(3)) / sum(e)
                    depth_surfaces.setdefault(base + i, []).append((z, key))
                if blended and colour_write:
                    p = base + i
                    if blend:
                        if top[p] != 0 or not source_exact(frame):
                            raise Refused(f'{frame.name}: blended pixel ({i}, {j}) is not one blended layer '
                                          f'over an exact value')
                        top[p] = 1
                    else:
                        top[p] = cls
    for p, surfaces in depth_surfaces.items():
        surfaces.append((Q(1), ('plane',)))
        for x in range(len(surfaces)):
            for y in range(x + 1, len(surfaces)):
                (za, ka), (zb, kb) = surfaces[x], surfaces[y]
                if za == zb and (ka == kb == ('plane',) or ka == kb):
                    continue
                if abs(za - zb) < Q(1, 1 << 16):
                    raise Refused(f'{frame.name}: two surfaces at pixel ({p % W}, {p // W}) differ in depth by '
                                  f'{float(abs(za - zb))}, less than 2^-16 and not a permitted equality')
    n_cov = sum(covered)
    n_tie = sum(ties)
    if n_cov and n_tie > LIMITS['tie_fraction'] * n_cov:
        raise Refused(f'{frame.name}: {n_tie} edge-tie pixels exceed 5% of {n_cov} covered pixels')
    # A weight difference d on each of two axes moves a channel by at most
    # 2 d delta. The board's conversion adds under 1, rounding of its inner
    # lerps up to 1/2, and llvmpipe's rounding at most 1 (three rounded lerps
    # on its 8-bit path), so an integer channel difference is at most
    # 1 + ceil(2 d delta + 1/2).
    tol = 1 + math.ceil(2 * texel_delta(frame) * (BOARD_WEIGHT_ERROR + REFERENCE_WEIGHT_ERROR) +
                        BOARD_LERP_ROUNDING) if linear else 0
    tie_bits = bytearray((W * H + 7) // 8)
    for p in range(W * H):
        if ties[p]:
            tie_bits[p >> 3] |= 1 << (p & 7)
    return Analysis(bytes(classes), tol, bytes(tie_bits), n_cov, n_tie)


def source_exact(frame):
    """Texture samples are exact: nearest filtering without sRGB decoding."""
    return frame.profile in (2, 3) or not (frame.state & 32 or frame.state & (1 << 12))


# ---------------------------------------------------------------------------
# Closed-form frames

def srgb_decode(byte):
    c = byte / 255
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def unorm(x):
    return min(255, max(0, math.floor(x * 255 + 0.5)))


def closed_form_colours(frame, quantize=None, trace=None):
    """Exact colour of every pixel of a quad-only scene, before UNORM8 rounding.

    quantize, when given, maps each linear-filter texel coordinate (after the
    half-texel shift) to the value a modeled board uses, for the self-check's
    filter models. trace, when a set, receives each
    stencil compare and stencil operation whose stored value a later stencil
    test at the same pixel reads, as (frame, 'compare', op, ref, start, mask)
    or (frame, 'op', path, op, start, ref, write mask)."""
    W, H = frame.width, frame.height
    clear = [((CLEAR >> (8 * c)) & 255) / 255 for c in range(4)]
    colour = [list(clear) for _ in range(W * H)]
    depth = [Q(1)] * (W * H)
    stencil = [0] * (W * H)
    pending = {}
    p4 = frame.profile == 4
    for batch in frame.batches:
        st = batch.state
        ref, cmask, wmask = batch.stencil & 255, (batch.stencil >> 8) & 255, (batch.stencil >> 16) & 255
        for quad in batch.prims:
            if p4 and ((st >> 4) & 3) == (1 if quad.clockwise else 2):
                continue
            const = sampler(frame, quad, quantize) if frame.profile > 1 else None
            for j in range(quad.y0, quad.y1):
                for i in range(quad.x0, quad.x1):
                    p = j * W + i
                    z = quad.z
                    if p4:
                        d_pass = compare_op(st & 7, z, depth[p])
                        s_on = st & (1 << 10)
                        s_pass = compare_op((st >> 11) & 7, ref & cmask, stencil[p] & cmask) if s_on else True
                        if s_on:
                            if trace is not None:
                                trace.update(pending.pop(p, ()))
                                path = 'fail' if not s_pass else 'zfail' if not d_pass else 'pass'
                                op = (st >> {'fail': 14, 'zfail': 17, 'pass': 20}[path]) & 7
                                pending[p] = [(frame.name, 'compare', (st >> 11) & 7, ref, stencil[p], cmask),
                                              (frame.name, 'op', path, op, stencil[p], ref, wmask)]
                            stencil[p] = stencil_result(stencil[p], ref, st, batch.stencil, d_pass)
                        if not (s_pass and d_pass):
                            continue
                        if st & 8:
                            depth[p] = z
                        if st & NO_COLOUR:
                            continue
                        eq = (st >> 6) & 7
                    elif frame.profile == 3:
                        mode = frame.state & 3
                        if mode:
                            if not compare_op((ALWAYS, LESS, LEQUAL, ALWAYS)[mode], z, depth[p]):
                                continue
                            if frame.state & 4:
                                depth[p] = z
                        eq = 1 if frame.state & 8 else 0
                    else:
                        eq = 0
                    t = (Q(2 * i + 1, 2) - quad.x0) / (quad.x1 - quad.x0)
                    if frame.profile == 1:
                        src = [float(Q(a) + (Q(b) - Q(a)) * t) for a, b in zip(*quad.colour)]
                    elif const is not None:
                        src = const
                    else:
                        s = (Q(2 * j + 1, 2) - quad.y0) / (quad.y1 - quad.y0)
                        v = quad.v[0] + (quad.v[1] - quad.v[0]) * s
                        src = sample(frame, quad_u(quad, t), v, quantize)
                    colour[p] = blend_values(eq, src, colour[p])
    return colour


def closed_form(frame, quantize=None):
    """Expected frame for quad-only scenes, rounded to UNORM8 and laid out at
    the device stride with PVR_RENDER_POISON in the padding; with quantize,
    the frame a modeled board filter gives."""
    if quantize is None:
        return frame.exact.image
    return unorm_image(frame, closed_form_colours(frame, quantize))


def unorm_image(frame, colour):
    W, H = frame.width, frame.height
    pad = struct.pack('<I', POISON) * (frame.stride - W)
    out = bytearray()
    for j in range(H):
        for c in colour[j * W:(j + 1) * W]:
            out += bytes(unorm(x) for x in c)
        out += pad
    return bytes(out)


def quad_u(quad, t):
    """Perspective-correct u at fraction t across a quad, exact."""
    inv = (1 - t) / quad.w[0] + t / quad.w[1]
    return ((1 - t) * quad.u[0] / quad.w[0] + t * quad.u[1] / quad.w[1]) / inv


def sampler(frame, quad, quantize=None):
    if quad.u[0] == quad.u[1] and quad.v[0] == quad.v[1]:
        return sample(frame, quad.u[0], quad.v[0], quantize)
    return None


def sample(frame, u, v, quantize=None):
    tw, th, tex = frame.texels
    p4 = frame.profile == 4
    modes = ((frame.state >> 6) & 7, (frame.state >> 9) & 7) if p4 else (2, 2)
    srgb = p4 and frame.state & (1 << 12)

    def fetch(x, y):
        x, y = wrap(x, tw, modes[0]), wrap(y, th, modes[1])
        if x is None or y is None:
            return [0.0] * 4
        px = tex[4 * (y * tw + x):4 * (y * tw + x) + 4]
        return [srgb_decode(px[c]) if srgb and c < 3 else px[c] / 255 for c in range(4)]
    tu, tv = u * tw, v * th
    if not (p4 and frame.state & 32):
        return fetch(math.floor(tu), math.floor(tv))
    tu, tv = tu - Q(1, 2), tv - Q(1, 2)
    if quantize:
        tu, tv = quantize(tu), quantize(tv)
    x0, y0 = math.floor(tu), math.floor(tv)
    a, b = tu - x0, tv - y0
    a, b = float(a), float(b)
    t00, t10, t01, t11 = fetch(x0, y0), fetch(x0 + 1, y0), fetch(x0, y0 + 1), fetch(x0 + 1, y0 + 1)
    return [(1 - a) * (1 - b) * t00[c] + a * (1 - b) * t10[c] + (1 - a) * b * t01[c] + a * b * t11[c]
            for c in range(4)]


def confirmation_quads(frame):
    return [q for b in frame.batches for q in b.prims if isinstance(q, Quad) and q.confirm]


def has_confirmation(frame):
    return bool(confirmation_quads(frame))


def confirmation_pixels(frame, exact):
    """(x, y, exact RGBA scaled to 0..255, bound) for every pixel of the
    frame's confirmation quads, which are drawn last and not overdrawn; exact
    holds closed_form_colours(frame).

    Blended pixels pass within 1 of the exact value: one rounding at
    destination precision. Linear-filtered pixels sample the black and white
    texels 0 and 1 along u, so a filter weight error e moves a channel by
    255e. The constant-w bands pass within 1 + 255 x FILTER_STEP; the
    perspective ramp also allows PERSPECTIVE_ERROR."""
    out = []
    for q in confirmation_quads(frame):
        if not frame.state & 32:
            bound = Q(1)
        else:
            bound = RAMP_BOUND if q.w[0] != q.w[1] else 1 + 255 * FILTER_STEP
        out += [(i, j, [255 * min(1.0, max(0.0, x)) for x in exact[j * frame.width + i]], bound)
                for j in range(q.y0, q.y1) for i in range(q.x0, q.x1)]
    return out


def confirm(expected, image, stride):
    """Failures of confirmation pixels, as Frame.confirmation lists them, in
    an RGBA8 image with the given row stride."""
    failures = []
    for i, j, want, bound in expected:
        got = image[4 * (j * stride + i):4 * (j * stride + i) + 4]
        if any(abs(got[c] - want[c]) > bound for c in range(4)):
            failures.append([i, j, [round(x, 3) for x in want], list(got)])
    return failures


def stencil_separation(trace):
    """Stencil behaviour the traced closed-form scenes cannot tell apart.

    Returns the (path, op, other op) and ('compare', op, other op) triples for
    which no traced execution gives the two a different outcome, whether a
    compare mask and a write mask each changed some traced outcome, and the
    names of the traced frames."""
    ops, compares = {}, {}
    for entry in trace:
        if entry[1] == 'op':
            ops.setdefault(entry[2:4], set()).add(entry[4:])
        else:
            compares.setdefault(entry[2], set()).add(entry[3:])

    def stored(op, s, ref, wmask):
        return (s & ~wmask | stencil_apply(op, s, ref) & wmask) & 255
    missing = []
    for path in ('fail', 'zfail', 'pass'):
        for op in range(8):
            runs = ops.get((path, op), ())
            missing += [(path, op, other) for other in range(8) if other != op and
                        not any(stored(op, *r) != stored(other, *r) for r in runs)]
    for op in range(8):
        runs = compares.get(op, ())
        missing += [('compare', op, other) for other in range(8) if other != op and
                    not any(compare_op(op, ref & m, s & m) != compare_op(other, ref & m, s & m) for ref, s, m in runs)]
    compare_mask = any(m != 255 and compare_op(op, ref & m, s & m) != compare_op(op, ref, s)
                       for op, runs in compares.items() for ref, s, m in runs)
    write_mask = any(r[2] != 255 and stored(op, *r) != stored(op, r[0], r[1], 255)
                     for (_, op), runs in ops.items() for r in runs)
    return missing, compare_mask, write_mask, sorted({entry[0] for entry in trace})


def blend_values(eq, s, d):
    if eq == 0:
        return list(s)
    if eq == 1:
        rgb = [s[c] * s[3] + d[c] * (1 - s[3]) for c in range(3)]
        return rgb + [s[3] + d[3] * (1 - s[3])]
    if eq == 2:
        return [s[c] + d[c] * (1 - s[3]) for c in range(3)] + [s[3] + d[3] * (1 - s[3])]
    if eq == 3:
        return [min(1.0, s[c] + d[c]) for c in range(4)]
    return [s[c] * d[c] for c in range(4)]


# ---------------------------------------------------------------------------
# Coverage

def coverage(closed, board, kernels, stencil):
    """Feature row -> scenes that compare it. Graphics rows prefer closed-form
    scenes; rows that need general triangles use board scenes compared with
    the reference. A row whose required values are not all present is empty.
    Stencil rows come from stencil, the stencil_separation() result for the
    executions that the closed-form evaluation of the closed scenes traced."""
    def names(frames):
        return sorted({f.name for f in frames})
    p4 = [f for f in closed if f.profile == 4]
    batches = [(f, b) for f in closed for b in f.batches if f.profile == 4]

    def values(pred, key):
        return {key(b) for f, b in batches if pred(b)}

    def complete(found, want, frames):
        return names(frames) if set(want) <= found else []
    missing, compare_mask, write_mask, traced = stencil
    depth_tested = values(lambda b: True, lambda b: b.state & 7) - {7} | values(lambda b: b.state & 8, lambda b: 7)
    rows = {
        'Fixed clear': ['fixed-clear'],
        'Background clear': names(closed),
        'Interpolated colour': names(f for f in closed if f.profile == 1 and any(
            q.colour[0] != q.colour[1] for b in f.batches for q in b.prims)),
        'Triangle coverage, shared edges and fans': names(f for f in board if f.profile == 1 and not f.closed_form),
        'Degenerate triangles': names(f for f in board if f.profile == 1 and all(
            degenerate(t) for b in f.batches for t in b.prims if isinstance(t, Tri))),
        'Nearest sampling with clamp to edge': names(f for f in closed if f.profile in (2, 3)),
        'Texture scaling and mirrored coordinates': names(f for f in closed if f.profile == 2 and (
            f.texels[:2] != (f.width, f.height) or any(q.u[0] > q.u[1] for b in f.batches for q in b.prims))),
        'Row stride crop': names(f for f in closed if f.width % 2),
        'Textured triangles off the pixel grid': names(f for f in board if f.profile == 2 and not f.closed_form),
        'Depth test, profile 3': complete({(f.state & 7) for f in closed if f.profile == 3 and f.state & 3},
                                          {1, 2, 3, 5, 6, 7}, [f for f in closed if f.profile == 3 and f.state & 3]),
        'Source-over blending, profile 3': names(f for f in closed if f.profile == 3 and f.state & 8),
        'Depth compare operations': complete(depth_tested, range(8), p4),
        'Depth writes': names([f for f, b in batches if b.state & 8] +
                              [f for f in closed if f.profile == 3 and f.state & 4]),
        'Stencil compare operations': [] if any(m[0] == 'compare' for m in missing) else traced,
        'Stencil operations on stencil fail, depth fail and pass':
            [] if any(m[0] != 'compare' for m in missing) else traced,
        'Stencil compare and write masks': traced if compare_mask and write_mask else [],
        'Blend equations': complete(values(lambda b: True, lambda b: (b.state >> 6) & 7), range(5), p4),
        'Culling': complete(values(lambda b: True, lambda b: (b.state >> 4) & 3), range(3), p4),
        'Colour-write disable': names(f for f, b in batches if b.state & NO_COLOUR),
        'Address modes on U and V': complete({(axis, (f.state >> shift) & 7) for f in p4
                                              for axis, shift in (('u', 6), ('v', 9))},
                                             {(axis, m) for axis in 'uv' for m in range(5)}, p4),
        'Linear filtering': names(f for f in p4 if f.state & 32),
        'sRGB decoding': names(f for f in p4 if f.state & (1 << 12)),
        'Perspective-correct texture coordinates': names(f for f, b in batches for q in b.prims
                                                         if isinstance(q, Quad) and q.w[0] != q.w[1]),
        'Sixteen index-range batches': names(f for f in p4 if len(f.batches) == 16),
        'Maximum-width frame': names(f for f in board if f.width == 1920),
    }
    for k in kernels:
        rows[f'Compute: {k.name}'] = [f'compute-{k.name}']
    return rows


def degenerate(tri):
    (ax, ay), (bx, by), (cx, cy) = [(r[0] / r[3], r[1] / r[3]) for r in tri.rows]
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax) == 0


# ---------------------------------------------------------------------------
# Launches and modeled time

@dataclass
class Launch:
    name: str
    image: str
    envelope: str
    kind: str                     # 'triangle', 'texture' or 'compute'
    frames: list = None
    kernel: Kernel = None
    profile: int = 0
    session: tuple = None         # framebuffer width, height


@functools.cache
def launches():
    """The planned launches. Built once per process, so the device input and
    analysis that each frame keeps are computed once."""
    out = [Launch('triangle', 'sunstone-a733-gpu-draw-single-core', 'GPU base session', 'triangle',
                  colour_frames(), profile=1, session=(64, 64)),
           Launch('profile2', 'sunstone-a733-gpu-texture-single-core', 'texture envelope version 1', 'texture',
                  profile2_frames(), profile=2, session=(255, 129)),
           Launch('profile3', 'sunstone-a733-gpu-texture-single-core', 'texture envelope version 2', 'texture',
                  profile3_frames(), profile=3, session=(256, 256)),
           Launch('profile4', 'sunstone-a733-gpu-texture-single-core', 'texture envelope version 3', 'texture',
                  profile4_frames(), profile=4, session=(128, 128)),
           Launch('profile4-wide', 'sunstone-a733-gpu-texture-single-core', 'texture envelope version 3', 'texture',
                  [wide_frame()], profile=4, session=(1920, 384))]
    for k in compute_kernels():
        out.append(Launch(f'compute-{k.name}', 'sunstone-a733-gpu-compute-single-core with '
                          'SUNSTONE_GPU_PROGRAM_APPLICATION and SUNSTONE_GPU_RUNTIME_INPUT',
                          'program envelope with runtime input', 'compute', kernel=k))
    return tuple(out)


@functools.cache
def refused_launches():
    """Launches the plan must refuse at the current USB rate."""
    return (Launch('profile4-1080p', 'sunstone-a733-gpu-texture-single-core', 'texture envelope version 3',
                   'texture', [wide_frame(1080)], profile=4, session=(1920, 1080)),)


def packets(n):
    return max(1, -(-n // 512))


def exchange(up, down):
    return TICKS['packet'] * (packets(up) + packets(down)) + TICKS['reply']


def model(launch):
    """Worst-case modeled seconds for a launch and its per-stage maxima."""
    P = TICKS['packet']
    stats = {'ticks': 0, 'frame_stream': 0, 'slot': 0, 'upload': 0, 'readback': 0}

    def add(t):
        stats['ticks'] += t
    if launch.kind == 'triangle':
        add(exchange(40, 40))                       # READ 0
        for frame in launch.frames:
            data = frame.expanded[1]['data']
            stats['upload'] = max(stats['upload'], P * packets(40 + len(data)))
            add(exchange(40 + len(data), 40) + TICKS['completion'])
            stream = 64 * exchange(40, 296)
            stats['frame_stream'] = max(stats['frame_stream'], stream)
            stats['slot'] = max(stats['slot'], exchange(40, 296))
            stats['readback'] += 64 * 64 * 4
            add(stream)
        add(2 * exchange(40, 40))                   # CLOSE and its final READ
    elif launch.kind == 'texture':
        add(exchange(64, 64))                       # STATUS
        for frame in launch.frames:
            data = frame.expanded[1]['data']
            add(exchange(64, 64))                   # BEGIN
            for off in range(0, len(data), texture.CHUNK):
                n = min(texture.CHUNK, len(data) - off)
                stats['upload'] = max(stats['upload'], P * packets(64 + n))
                add(exchange(64 + n, 64))
            add(exchange(64, 64) + TICKS['completion'])     # SUBMIT
            total = frame.stride * frame.height * 4
            stream = exchange(64, 64)                       # READ_FRAME header
            for off in range(0, total, texture.CHUNK):
                n = min(texture.CHUNK, total - off)
                slot = P * packets(n) + TICKS['slot']
                stats['slot'] = max(stats['slot'], slot)
                stream += slot
            stats['frame_stream'] = max(stats['frame_stream'], stream)
            stats['readback'] += total
            add(stream)
        add(exchange(64, 64))                       # CLOSE
    else:
        k = launch.kernel
        n = k.count(k.inputs[0]) * k.output_element // 4
        read = -(-n // 32) * exchange(32, 40 + 128)
        add(read)                                   # result of the sealed input
        stats['readback'] += 4 * n
        stats['frame_stream'] = read
        stats['slot'] = exchange(32, 40 + 128)
        for data in k.inputs[1:]:
            stats['upload'] = max(stats['upload'], P * packets(32 + len(data)))
            add(exchange(32 + len(data), 40) + TICKS['completion'] + read)
            stats['readback'] += 4 * n
        add(2 * exchange(32, 40))
    result = {key: (value * TICK_S if key != 'readback' else value) for key, value in stats.items()}
    result['launch_s'] = result.pop('ticks')
    result['session_s'] = OPEN_S + result['launch_s'] + CLOSE_S
    return result


def check_timing(launch, timing):
    for key, limit in (('frame_stream', 'frame_stream_s'), ('session_s', 'session_s'), ('slot', 'slot_s'),
                       ('upload', 'upload_s')):
        if timing[key] > LIMITS[limit]:
            raise Refused(f'{launch.name}: modeled {key} {timing[key]:.2f} s exceeds {LIMITS[limit]} s, '
                          f'half its board limit')


def plan(launch_list=None):
    """Check every launch; returns a JSON-serializable plan. Timing is checked
    first, since it needs only the device input."""
    entries = []
    for launch in launch_list or launches():
        timing = model(launch)
        check_timing(launch, timing)
        entry = {'launch': launch.name, 'image': launch.image, 'envelope': launch.envelope}
        if launch.kind == 'compute':
            compute_plan(launch.kernel)
            entry['inputs'] = [hashlib.sha256(d).hexdigest() for d in launch.kernel.inputs]
        else:
            entry['frames'] = []
            for frame in launch.frames:
                a = frame.analysis
                entry['frames'].append({'name': frame.name, 'input_sha256': input_sha256(frame), 'state': frame.state,
                                        'covered': a.covered, 'ties': a.tie_pixels, 'closed_form': frame.closed_form,
                                        'linear_tolerance': a.linear_tol})
        entry['model'] = timing
        entries.append(entry)
    readback = sum(e['model']['readback'] for e in entries)
    if readback > LIMITS['readback_bytes']:
        raise Refused(f'planned readback {readback} bytes exceeds 8 MiB')
    return {'launches': entries, 'readback_bytes': readback, 'ticks': TICKS,
            'worst_frame_stream_s': max(e['model']['frame_stream'] for e in entries),
            'worst_session_s': max(e['model']['session_s'] for e in entries),
            'worst_slot_s': max(e['model']['slot'] for e in entries),
            'worst_upload_s': max(e['model']['upload'] for e in entries)}


# ---------------------------------------------------------------------------
# Reference process

def run(cmd, log, env=None, cwd=None):
    with open(log, 'w') as f:
        subprocess.run(cmd, check=True, stdout=f, stderr=subprocess.STDOUT, env=env, cwd=cwd)


def producer_env():
    env = {'PATH': f'{PRODUCER_HOST}/bin:{PRODUCER_VENV}/bin:/usr/bin:/bin', 'CC': 'clang-21', 'CXX': 'clang++-21',
           'LLVM_CONFIG': str(PRODUCER_HOST / 'bin/llvm-config-21'),
           'PKG_CONFIG_PATH': ':'.join(str(PRODUCER_HOST / p) for p in
                                       ('usr/lib/x86_64-linux-gnu/pkgconfig', 'usr/lib/pkgconfig', 'usr/share/pkgconfig')),
           'CFLAGS': f'-I{PRODUCER_HOST}/usr/lib/llvm-21/include', 'CXXFLAGS': f'-I{PRODUCER_HOST}/usr/lib/llvm-21/include',
           'LDFLAGS': f'-L{PRODUCER_HOST}/usr/lib -Wl,-rpath,{PRODUCER_HOST}/usr/lib',
           'LD_LIBRARY_PATH': f'{PRODUCER_HOST}/usr/lib:{PRODUCER_HOST}/usr/lib/x86_64-linux-gnu', 'HOME': str(Path.home())}
    return env


def tool_version(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.splitlines()[0]


def build(build=BUILD):
    """Build pinned Mesa llvmpipe and the reference renderer, skipping unchanged stamps."""
    build.mkdir(parents=True, exist_ok=True)
    env = producer_env()
    stamp = {'archive_sha256': MESA_SHA256, 'options': MESA_OPTIONS, 'lp_num_threads': LP_NUM_THREADS,
             'clang': tool_version(['clang-21', '--version']),
             'llvm': tool_version([str(PRODUCER_HOST / 'bin/llvm-config-21'), '--version'])}
    install = build / 'mesa-install'
    stamp_path = build / 'mesa-stamp.json'
    rebuilt = False
    if not stamp_path.exists() or json.loads(stamp_path.read_text()) != stamp or not (install / 'lib').exists():
        digest = hashlib.sha256(MESA_ARCHIVE.read_bytes()).hexdigest()
        if digest != MESA_SHA256:
            raise ValueError(f'Mesa archive SHA-256 {digest} differs from the pin')
        for d in ('mesa-src', 'mesa-build', 'mesa-install'):
            shutil.rmtree(build / d, ignore_errors=True)
        (build / 'mesa-src').mkdir()
        with tarfile.open(MESA_ARCHIVE) as tar:
            for m in tar.getmembers():
                parts = Path(m.name).parts[1:]     # strip the archive's top directory
                if parts:
                    m.name = str(Path(*parts))
                    tar.extract(m, build / 'mesa-src', filter='data')
        run(['nice', '-n', '10', str(PRODUCER_VENV / 'bin/meson'), 'setup', str(build / 'mesa-build'),
             str(build / 'mesa-src'), f'--prefix={install}'] + MESA_OPTIONS, build / 'mesa-configure.log', env)
        run(['nice', '-n', '10', str(PRODUCER_VENV / 'bin/ninja'), '-C', str(build / 'mesa-build'), f'-j{JOBS}',
             'install'], build / 'mesa-build.log', env)
        stamp_path.write_text(json.dumps(stamp, indent=1) + '\n')
        rebuilt = True
    source = ROOT / 'tools/gpu-conformance/reference.c'
    binary = build / 'reference'
    ref_stamp = hashlib.sha256(source.read_bytes() + stamp_path.read_bytes()).hexdigest()
    ref_stamp_path = build / 'reference-stamp'
    if rebuilt or not binary.exists() or not ref_stamp_path.exists() or ref_stamp_path.read_text() != ref_stamp:
        lib = library_dir(build)
        run(['clang-21', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', f'-I{build}/mesa-src/include', str(source),
             f'-L{lib}', '-lEGL', f'-Wl,-rpath,{lib}', '-o', str(binary)], build / 'reference-build.log', env)
        ref_stamp_path.write_text(ref_stamp)
    return {'mesa_rebuilt': rebuilt, 'reference': str(binary), 'stamp': stamp}


def library_dir(build):
    return next((build / 'mesa-install').glob('lib*/**/libEGL.so.1')).parent


def launch_file(entries):
    """entries: (frame, analysis, capture, mutations); capture is bytes, 'self' or None."""
    out = bytearray(struct.pack('<2I', 0x31464347, len(entries)))
    for frame, a, capture, mutations in entries:
        triangles, device = frame.expanded
        if frame.profile == 1:
            tw = th = 0
            nvert, nidx = len(triangles) * 3, 0
        else:
            tw, th, _, nvert, nidx = device['shape']
        data = device['data']
        out += struct.pack('<10I', frame.profile, frame.width, frame.height, frame.stride, frame.state, tw, th,
                           nvert, nidx, len(data)) + data
        out += struct.pack('<I', len(a.classes)) + a.classes + struct.pack('<I', a.linear_tol) + a.ties
        if capture is None:
            out += struct.pack('<I', 0)
        elif capture == 'self':
            out += struct.pack('<I', 2)
        else:
            if len(capture) != 4 * frame.stride * frame.height:
                raise ValueError(f'{frame.name}: capture extent differs from the frame')
            out += struct.pack('<I', 1) + capture
        out += struct.pack('<I', mutations)
    return bytes(out)


REFERENCE_PROCESSES = 0     # reference processes started by this Python process


def reference(entries, work, build=BUILD, frames=False):
    """Render and compare in one reference process, counted in
    REFERENCE_PROCESSES.

    Returns the renderer identity, the loaded libraries, the parsed comparison
    lines and, when frames is true, each entry's unmutated reference frame as
    width x height RGBA8 bytes. The launch and frame files are deleted once
    the process exits."""
    work.mkdir(parents=True, exist_ok=True)
    path = work / 'launch.bin'
    out = work / 'frames.bin'
    path.write_bytes(launch_file(entries))
    lib = library_dir(build)
    env = {'PATH': '/usr/bin:/bin', 'HOME': str(Path.home()), 'LD_LIBRARY_PATH': str(lib),
           'EGL_PLATFORM': 'surfaceless', 'LIBGL_ALWAYS_SOFTWARE': '1', 'GALLIUM_DRIVER': 'llvmpipe',
           'LP_NUM_THREADS': str(LP_NUM_THREADS), 'MESA_SHADER_CACHE_DISABLE': 'true', 'EGL_LOG_LEVEL': 'fatal'}
    cmd = [str(build / 'reference'), str(path)] + ([str(out)] if frames else [])
    global REFERENCE_PROCESSES
    REFERENCE_PROCESSES += 1
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, env=env)
        rendered = None
        if frames and not result.returncode:
            data, rendered, offset = out.read_bytes(), [], 0
            for frame, *_ in entries:
                size = 4 * frame.width * frame.height
                rendered.append(data[offset:offset + size])
                offset += size
    finally:
        path.unlink(missing_ok=True)
        out.unlink(missing_ok=True)
    if result.returncode:
        raise RuntimeError(f'reference process failed: {result.stderr.strip()}')
    lines = [json.loads(line) for line in result.stdout.splitlines()]
    info = lines[0]
    libs = [l['library'] for l in lines if 'library' in l]
    if 'llvmpipe' not in info['gl_renderer'] or 'Mesa 27.0.0-devel' not in info['gl_version'] or \
            not libs or not all(Path(l).resolve().is_relative_to((build / 'mesa-install').resolve()) for l in libs):
        raise RuntimeError(f'reference did not use the pinned Mesa build: {info} {libs}')
    return info, libs, [l for l in lines if 'frame' in l], rendered


def judge(results, entries):
    """Fold comparison lines into per-frame verdicts."""
    verdicts = []
    for r in results:
        frame = entries[r['frame']][0]
        ok = r['failed'] == 0
        verdicts.append({'frame': frame.name, 'mutation': r['mutation'], 'passed': ok, **{
            k: r[k] for k in ('pixels', 'covered', 'ties', 'exact', 'tolerant', 'tie_accepted', 'failed', 'stale',
                              'sentinel', 'compare_ms', 'failures')}})
    return verdicts


def check_sentinels(results, entries):
    for r in results:
        if r['mutation'] == 0 and r['sentinel']:
            raise Refused(f'{entries[r["frame"]][0].name}: {r["sentinel"]} reference pixels lie within their '
                          f'tolerance of PVR_RENDER_POISON')


# ---------------------------------------------------------------------------
# Command line

def find_launch(name):
    """A launch by name, including those the plan refuses, so that inputs and
    compare reach the plan's own refusal; None for the fixed clear. Raises
    ValueError for an unknown name."""
    if name == 'fixed-clear':
        return None
    for launch in launches():
        if launch.name == name:
            return launch
    for launch in refused_launches():
        if launch.name == name:
            return launch
    raise ValueError(f'unknown launch {name}')


def write_inputs(launch, out):
    """Canonical device inputs for the coordinator's capture functions, written
    only for a launch that passes every plan check, timing included."""
    plan([launch])
    out.mkdir(parents=True, exist_ok=False)
    record = {'launch': launch.name, 'image': launch.image, 'envelope': launch.envelope}
    if launch.kind == 'compute':
        record['inputs'] = []
        for k, data in enumerate(launch.kernel.inputs):
            (out / f'input-{k}.bin').write_bytes(data)
            record['inputs'].append({'file': f'input-{k}.bin', 'sha256': hashlib.sha256(data).hexdigest(),
                                     'role': 'package input' if k == 0 else 'runtime replacement'})
    else:
        record['profile'] = launch.profile
        record['session'] = launch.session
        record['frames'] = []
        for k, frame in enumerate(launch.frames):
            _, device = frame.expanded
            (out / f'frame-{k}.input').write_bytes(device['data'])
            record['frames'].append({'name': frame.name, 'file': f'frame-{k}.input', 'state': device['state'],
                                     'shape': device.get('shape'), 'input_sha256': input_sha256(frame)})
    (out / 'inputs.json').write_text(json.dumps(record, indent=1) + '\n')


def check_record(launch, frames):
    """Refuse a capture record that does not belong to the plan: wrong input
    hash or state, reply sequences that do not advance by one per frame, a
    texture frame without its generation (the triangle protocol has none),
    extra frames, or frames recorded after a failed one."""
    if len(frames) > len(launch.frames):
        raise Refused(f'{launch.name}: capture has {len(frames)} frames, plan has {len(launch.frames)}')
    first = frames[0].get('sequence') if frames else 0
    for k, (frame, item) in enumerate(zip(launch.frames, frames)):
        if item.get('input_sha256') != input_sha256(frame):
            raise Refused(f'{frame.name}: recorded input hash differs from the plan')
        if item.get('state') != frame.state:
            raise Refused(f'{frame.name}: recorded state {item.get("state")} differs from the plan {frame.state}')
        if not isinstance(first, int) or item.get('sequence') != first + k:
            raise Refused(f'{frame.name}: recorded sequence is missing or out of order')
        if launch.kind == 'texture' and item.get('generation') is None:
            raise Refused(f'{frame.name}: recorded generation is missing')
        if item.get('result') != 1 and k + 1 < len(frames):
            raise Refused(f'{frame.name}: frames are recorded after a failed frame')


def plan_digest(launch):
    """SHA-256 over a launch's planned input hashes and states, which ties a
    comparison to the plan version it judged."""
    h = hashlib.sha256()
    for frame in launch.frames:
        h.update(f'{input_sha256(frame)} {frame.state}\n'.encode())
    return h.hexdigest()


def rests_on_confirmation(frame):
    """Whether a frame's tolerance rests on what the profile 4 confirmation
    scenes show: linear filtering or blending."""
    if frame.profile == 3:
        return bool(frame.state & 8)
    return frame.profile == 4 and bool(frame.state & 32 or any((b.state >> 6) & 7 for b in frame.batches))


def needs_confirmation(launch):
    """Whether a launch other than profile 4 has a frame whose tolerance rests
    on the profile 4 confirmation scenes."""
    return launch.name != 'profile4' and any(rests_on_confirmation(f) for f in launch.frames)


def profile4_confirmed(path):
    """Whether path holds the comparison.json of a profile 4 launch that
    passed, confirmation included, against the current plan."""
    earlier = json.loads(path.read_text()) if path else {}
    confirmation = earlier.get('confirmation', {})
    return (earlier.get('launch') == 'profile4' and earlier.get('status') == 'passed' and
            earlier.get('plan_sha256') == plan_digest(find_launch('profile4')) and
            confirmation.get('failed') == {} and confirmation.get('not_captured') == [])


def compare_capture(name, capture_dir, out, build=BUILD, confirmation=None):
    """Compare a recorded board capture with the plan and the reference.

    The launch must pass every plan check, timing included, before its
    capture is read. A profile 4 launch counts only when its confirmation
    pixels match their exact values. Any other launch with a linear-filtered
    or blended frame needs, as confirmation, the comparison.json of a profile
    4 launch that passed against the current plan. out is created only to
    write comparison.json."""
    if out.exists():
        raise FileExistsError(f'{out} exists')
    launch = find_launch(name)
    if launch is not None:
        plan([launch])
    record = json.loads((capture_dir / 'capture.json').read_text())
    if launch is None:
        frames = record['frames']
        data = (capture_dir / frames[0]['file']).read_bytes()
        bad = [i for i in range(len(data) // 4) if struct.unpack_from('<I', data, 4 * i)[0] != CLEAR]
        report = {'launch': 'fixed-clear', 'pixels': len(data) // 4, 'failed': len(bad), 'first': bad[:16],
                  'passed': len(data) == 64 * 64 * 4 and not bad}
    elif launch.kind == 'compute':
        expected = compute_plan(launch.kernel)
        capture = []
        for item in record['inputs']:
            raw = (capture_dir / item['file']).read_bytes() if item.get('file') else b''
            capture.append({**item, 'words': list(struct.unpack(f'<{len(raw) // 4}I', raw))})
        report = compare_compute(launch.kernel, expected, capture)
    else:
        check_record(launch, record['frames'])
        statuses = ['not captured'] * len(launch.frames)
        entries = []
        for k, (frame, item) in enumerate(zip(launch.frames, record['frames'])):
            if item.get('result') != 1:
                statuses[k] = f'failed at result {item.get("result")}'
            else:
                statuses[k] = 'not compared'
                entries.append((frame, frame.analysis, (capture_dir / item['file']).read_bytes(), 0))
        report = {'launch': name}
        unconfirmed = None
        if launch.name == 'profile4':
            scenes = [f.name for f in launch.frames if has_confirmation(f)]
            failed = {f.name: confirm(f.confirmation, image, f.stride)
                      for f, _, image, _ in entries if f.name in scenes}
            report['plan_sha256'] = plan_digest(launch)
            report['confirmation'] = {'failed': {n: f[:16] for n, f in failed.items() if f},
                                      'not_captured': [n for n in scenes if n not in failed]}
            if any(report['confirmation'].values()):
                unconfirmed = 'confirmation pixels differ from their exact values or were not captured'
        elif needs_confirmation(launch) and not profile4_confirmed(confirmation):
            unconfirmed = 'needs the passed comparison of the profile 4 launch of the current plan'
        verdicts = {}
        if unconfirmed:
            # The linear-filter and blending tolerances rest on the
            # confirmation, so no other scene counts without it.
            report['status'] = f'unconfirmed: {unconfirmed}'
        elif entries:
            with tempfile.TemporaryDirectory(prefix='gpu-conformance-') as work:
                info, libs, results, _ = reference(entries, Path(work), build)
            check_sentinels(results, entries)
            report.update(renderer=info, libraries=libs)
            verdicts = {v['frame']: v for v in judge(results, entries)}
        report['frames'] = []
        for frame, status in zip(launch.frames, statuses):
            v = verdicts.get(frame.name, {})
            if v:
                status = 'pass' if v['passed'] else 'mismatch'
            report['frames'].append({'frame': frame.name, 'status': status, **v})
        report['passed'] = not unconfirmed and all(f['status'] == 'pass' for f in report['frames'])
        report.setdefault('status', 'passed' if report['passed'] else 'failed')
    out.mkdir(parents=True)
    (out / 'comparison.json').write_text(json.dumps(report, indent=1) + '\n')
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--build', type=Path, default=BUILD)
    sub = p.add_subparsers(dest='command', required=True)
    sub.add_parser('build')
    pl = sub.add_parser('plan')
    pl.add_argument('--output', type=Path)
    i = sub.add_parser('inputs')
    i.add_argument('--launch', required=True)
    i.add_argument('--output', type=Path, required=True)
    c = sub.add_parser('compare')
    c.add_argument('--launch', required=True)
    c.add_argument('--capture', type=Path, required=True)
    c.add_argument('--output', type=Path, required=True)
    c.add_argument('--confirmation', type=Path, help='comparison.json of a passed profile 4 launch, required '
                   'by every other launch with a linear-filtered or blended frame')
    a = p.parse_args()
    if a.command == 'build':
        print(json.dumps(build(a.build), indent=1))
    elif a.command == 'plan':
        result = plan()
        text = json.dumps(result, indent=1) + '\n'
        if a.output:
            a.output.write_text(text)
        for e in result['launches']:
            m = e['model']
            print(f"{e['launch']:24} {m['readback']:>9} B  {m['launch_s']:6.2f} s  stream {m['frame_stream']:6.2f} s  "
                  f"session {m['session_s']:6.2f} s")
        print(f"readback {result['readback_bytes']} B; worst stream {result['worst_frame_stream_s']:.2f} s; "
              f"worst session {result['worst_session_s']:.2f} s")
    else:
        try:
            launch = find_launch(a.launch)
        except ValueError as error:
            raise SystemExit(str(error))
        if a.command == 'inputs' and launch is None:
            raise SystemExit('the fixed clear has no input')
        try:
            if a.command == 'inputs':
                write_inputs(launch, a.output)
            else:
                report = compare_capture(a.launch, a.capture, a.output, a.build, a.confirmation)
                print(json.dumps({'launch': report['launch'], 'passed': report['passed'],
                                  'status': report.get('status')}))
                sys.exit(0 if report['passed'] else 1)
        except Refused as error:
            raise SystemExit(f'refused: {error}')


if __name__ == '__main__':
    main()
