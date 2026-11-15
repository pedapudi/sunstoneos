#!/usr/bin/env python3
"""Compare the G2D conversion builder with the vendor's register sequence, word for word.

drivers/g2d/a733_g2d_convert.c builds the mixer's RCQ command page for one YUV-to-RGB conversion with scaling.
vendor_blocks() below transcribes the vendor's g2d_bsp_bitblt with flag G2D_BLT_NONE (g2d_mixer.c 304-371 in
linux-orangepi 2ac08e8c, bsp/drivers/g2d/g2d_rcq) for a 4:2:2 semi-planar source and a 32-bit RGB output, with the
functions it calls: g2d_vlayer_set (g2d_ovl_v.c 244-335), g2d_ovl_v_calc_coarse (76-126), g2d_vsu_para_set and
g2d_vsu_calc_fir_coef (g2d_scal.c 248-475), bld_porter_duff, bld_set_rop_ctrl, bld_in_set, bld_cs_set,
bld_csc_reg_set and bld_out_setting (g2d_bld.c 84-354), and g2d_wb_set (g2d_wb.c 100-170), with g2d_byte_cal and
cal_align (g2d.c 351-450). The source and output images are buffers (bbuff 1) with pixel alpha mode 0, plane alpha
0xff, no premultiplication and 32-bit addresses. Each line cites the vendor line it follows. The vendor computes the
pitches from the image width and its alignment; the builder takes the pitches, so each case passes the vendor's.

The vendor selects a YUV-to-RGB block by the source and destination ranges it is told. The builder selects by the
block's contents, for a full-range RGB output (include/a733_g2d_convert.h): BT.601 or BT.709 limited is the block at
offset 0, which the vendor programs for a "full" source and a "limited" destination, and full is the block at 0x18,
which it programs for full and full. The check compares the builder's csc2 words with those blocks.

The page's other differences from the vendor are stated and checked: the builder zeroes the page for each job (the
vendor keeps its register images between jobs and rewrites only what it sets), and it writes the three UI layers'
first words as zero blocks, as a733_g2d_commands_at does.

The check builds the builder on the host and runs it on the stage's three geometries, enlargements, the coarse
decimation in both directions, the 2048-column scaler limit, odd widths, offsets, both 4:2:2 codes, the eight RGB
orders and the four tables. Each accepted job must give the vendor's words in every block. Each job outside the
builder's contract must be refused. It also checks that the builder's Lanczos bank is drivers/g2d/a733_g2d.c's, and
that every phase of every bank sums to 64, so a flat field passes the scaler unchanged.

The builder is built twice: as shipped, and with SUNSTONE_G2D_CONVERT_VARIANTS, the diagnostic changes the second
conversion stage measures (docs/g2d-os.md#second-conversion-stage). The variant build must give the shipped words for
variant 0, each variant's words (vendor_blocks' variant argument says where each comes from), and must refuse a variant
bit above the four.

Usage: g2d-convert-words.py --output DIR"""
import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

GATE_RUNS = [['--output', '{output}']]
ROOT = Path(__file__).resolve().parents[1]
M32 = 0xffffffff
G2D_V0, G2D_UI0, G2D_UI1, G2D_UI2, G2D_VSU, G2D_BLD, G2D_WB = 0x800, 0x1000, 0x1800, 0x2000, 0x8000, 0x400, 0x3000
G2D_FORMAT_BGRA1010102, G2D_FORMAT_YUV411_PLANAR = 0x17, 0x2e
YUV422 = (0x24, 0x25, 0x26)

# g2d_bld.c 37-50 and 66-79.
YCBCR2RGB_601 = [
    0x04a8, 0x0, 0x0662, 0xFFFC8480, 0x04a8, 0xFFFFFE6F, 0xFFFFFCC0, 0x21E00, 0x04a8, 0x0812, 0x0, 0xFFFBAC80,
    0x0400, 0x0, 0x057c, 0xFFFD0200, 0x0400, 0xFFFFFEA7, 0xFFFFFD35, 0x1D200, 0x0400, 0x06EE, 0x0, 0xFFFC4900,
    0x0400, 0x0, 0x059c, 0xFFFD31FF, 0x0400, 0xFFFFFE9F, 0xFFFFFD24, 0x21D80, 0x0400, 0x0717, 0x0, 0xFFFC747F,
    0x036F, 0x0, 0x04D1, 0xFFFDA090, 0x36F, 0xFFFFFED1, 0xFFFFFD8C, 0x1DA90, 0x036F, 0x0616, 0x0, 0xFFFCFE10]
YCBCR2RGB_709 = [
    0x04a8, 0x0, 0x072c, 0xFFFC1F80, 0x04a8, 0xFFFFFF26, 0xFFFFFDDE, 0x13380, 0x04a8, 0x0873, 0, 0xFFFB7C00,
    0x0400, 0x0, 0x0629, 0xFFFCAB80, 0x0400, 0xFFFFFF44, 0xFFFFE2B, 0x10880, 0x0400, 0x0742, 0x0, 0xFFFC1F00,
    0x0400, 0x0, 0x64D, 0xFFFC9980, 0x0400, 0xFFFFFF40, 0xFFFFFE21, 0x10F80, 0x0400, 0x076C, 0x0, 0xFFFC0A00,
    0x36f, 0x0, 0x0569, 0xFFFD5490, 0x36f, 0xFFFFFF5B, 0xFFFFFE64, 0x12990, 0x36f, 0x0660, 0x0, 0xFFFCD910]
COLOR_RANGE_0_255, COLOR_RANGE_16_235 = 0, 1
FILL_Y0, NO_FILL, CSC0, ROUND = 1, 2, 4, 8                               # A733_G2D_CONVERT_*
G2D_BT601, G2D_BT709 = 0, 1
# The builder's table (A733_G2D_CSC_*) and the vendor request that programs the same block: gamut, src_cr, dst_cr.
CSC = {1: (G2D_BT601, COLOR_RANGE_0_255, COLOR_RANGE_16_235), 2: (G2D_BT601, COLOR_RANGE_0_255, COLOR_RANGE_0_255),
       3: (G2D_BT709, COLOR_RANGE_0_255, COLOR_RANGE_16_235), 4: (G2D_BT709, COLOR_RANGE_0_255, COLOR_RANGE_0_255)}


def lan2_bank(path):
    """The 512 words of lan2coefftab32_full in a C source."""
    text = Path(path).read_text()
    body = text[text.index('lan2coefftab32_full[512] = {'):]
    body = body[:body.index('};')]
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    words = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', body.split('{', 1)[1])]
    assert len(words) == 512, len(words)
    return words


def cal_align(width, align):
    """g2d.c 421-450."""
    return width if align == 0 else (width + align - 1) // align * align


def fir_coef(step):
    """g2d_scal.c 248-268, with VSU_ZOOM0_SIZE 1, VSU_ZOOM1_SIZE 8, VSU_ZOOM2_SIZE 4, VSU_ZOOM3_SIZE 1, VSU_ZOOM4_SIZE 1
    and VSU_PHASE_NUM 32 (g2d_scal.h)."""
    ratio = step >> (19 - 3)
    whole, part = ratio >> 3, ratio & 7
    offset = 1 if whole == 0 else 1 + part if whole == 1 else 9 + (part >> 1) if whole == 2 else \
        13 if whole == 3 else 14 if whole == 4 else 15
    return offset * 32


def vendor_blocks(src, dst, gamut, src_cr, dst_cr, lan2, variant=0):
    """The vendor's register images for one blit, as {block: {offset: word}} for the words it sets.

    src: format, width, align (3), laddr (3), clip x, y, w, h. dst: format, width, align (3), laddr (3), clip x, y, w, h.
    variant: the A733_G2D_CONVERT_* diagnostic bits (include/a733_g2d_convert.h), applied after the vendor's words.
    """
    v, vs, b, wb = {}, {}, {}, {}
    # g2d_vlayer_set (g2d_ovl_v.c 244-335)
    attr = (src['format'] & 0x3f) << 8                                  # 257 lay_fbfmt
    attr |= 0 << 1                                                      # 258 alpha_mode = mode 0
    attr |= 0xff << 24                                                  # 261 lay_glbalpha
    attr |= 1                                                           # 262 lay_en
    v[0x00] = attr
    size = ((src['w'] - 1) & 0x1fff) | ((src['h'] - 1) & 0x1fff) << 16  # 264-267 ovl_mem
    v[0x04] = size
    v[0x2c] = size                                                      # 269-272 ovl_winsize
    v[0x08] = 0                                                         # 275 ovl_mem_coor
    assert src['format'] in YUV422                                      # 276-282: 4:2:2
    cw, cx, cy = src['width'] >> 1, src['clip_x'] >> 1, src['clip_y']
    ycnt, ucnt, vcnt = (1, 2, 0) if src['format'] != 0x26 else (1, 1, 1)  # 306, g2d_byte_cal (g2d.c 372-381)
    pitch0 = cal_align(ycnt * src['width'], src['align'][0])            # 307
    pitch1 = cal_align(ucnt * cw, src['align'][1])                      # 309
    pitch2 = cal_align(vcnt * cw, src['align'][2])                      # 311
    v[0x0c], v[0x10], v[0x14] = pitch0, pitch1, pitch2                  # 308, 310, 312
    addr0 = src['laddr'][0] + pitch0 * src['clip_y'] + ycnt * src['clip_x']  # 313-315
    addr1 = src['laddr'][1] + pitch1 * cy + ucnt * cx                   # 317-319
    addr2 = src['laddr'][2] + pitch2 * cy + vcnt * cx                   # 321-323
    v[0x18], v[0x1c], v[0x20] = addr0 & M32, addr1 & M32, addr2 & M32   # 316, 320, 324
    v[0x28] = (addr0 >> 32) & 0xff | ((addr1 >> 32) & 0xff) << 8 | ((addr2 >> 32) & 0xff) << 16  # 325-327
    # bbuff 1: no fill colour (328-329).
    # g2d_bsp_bitblt (g2d_mixer.c 315-371), G2D_BLT_NONE: g2d_vlayer_set above (316), mode 0 so no UI layer (318-320),
    # and for a YUV source (321-331) g2d_ovl_v_calc_coarse, the 4:2:2 semi-planar case (g2d_ovl_v.c 97-126).
    inw, inh, outw, outh = src['w'], src['h'], dst['w'], dst['h']
    if inw >= (outw << 3):
        midw = min(outw << 3, 2048)
        v[0x30] = (midw << 16 | inw) & M32
        v[0x34] = (midw << 15 | ((inw + 1) >> 1)) & M32
    else:
        midw = inw
        if midw > 2048:
            midw = 2048
            v[0x30] = (midw << 16 | inw) & M32
            v[0x34] = (midw << 15 | ((inw + 1) >> 1)) & M32
    if inh >= (outh << 2):
        midh = outh << 2
        v[0x38] = v[0x3c] = (midh << 16 | inh) & M32
    else:
        midh = inh
    # g2d_vsu_para_set(fmt, midw, midh, dst w, dst h, 0xff) (g2d_mixer.c 328-330; g2d_scal.c 270-475)
    ctrl = 1 << 8 | 1                                                   # 285-286 coef_access_sel, en
    ctrl |= (1 if src['format'] > 0x23 else 0) << 16                    # 287-290 filter_type
    vs[0x00] = ctrl
    vs[0x40] = ((outw - 1) & 0x1fff) | ((outh - 1) & 0x1fff) << 16      # 292-293 out_size
    vs[0x44] = 0xff                                                     # 294 glb_alpha
    vs[0x80] = ((midw - 1) & 0x1fff) | ((midh - 1) & 0x1fff) << 16      # 296-297 y_ch_size
    yhstep = ((midw << 19) & M32) // outw                               # 299-305
    vs[0x88] = (yhstep << 1) & M32                                      # 306
    yvstep = ((midh << 19) & M32) // outh                               # 307-312
    vs[0x8c] = (yvstep << 1) & M32                                      # 313
    for i in range(32):                                                 # 315-317 y horizontal
        vs[0x200 + 4 * i] = lan2[fir_coef(yhstep) + i]
    yvcoef = fir_coef(yvstep)                                           # 319
    incw, inch = (midw + 1) >> 1, midh                                  # 361-366: 4:2:2 semi-planar
    vs[0xc0] = ((incw - 1) & 0x1fff) | ((inch - 1) & 0x1fff) << 16      # 367-368 c_ch_size
    vs[0xc8] = yhstep & M32                                             # 371 c_hor_step
    vs[0xcc] = (yvstep << 1) & M32                                      # 372 c_ver_step
    for i in range(32):
        vs[0x400 + 4 * i] = lan2[fir_coef(yhstep >> 1) + i]             # 373-377 c horizontal
        vs[0x300 + 4 * i] = lan2[yvcoef + i]                            # 379-381 y vertical
    vs[0x90] = vs[0x98] = vs[0xd0] = vs[0xd8] = 0                       # 464-469 phases
    # bld_porter_duff(G2D_BLD_SRCOVER) (g2d_mixer.c 332; g2d_bld.c 375-376)
    b[0x4c] = 0x03010301
    # bld_set_rop_ctrl(0xf0) (g2d_mixer.c 334; g2d_bld.c 205-206)
    b[0x80], b[0x84] = 0xf0, 0x41000
    # bld_in_set(0, {0, 0, dst w, h}, dst bpremul 0) (g2d_mixer.c 335-339; g2d_bld.c 84-117)
    b[0x00] = 1 << 8 | 1                                                # 95, 97 p0_en, p0_fcen
    b[0x20] = ((outw - 1) & 0x1fff) | ((outh - 1) & 0x1fff) << 16       # 107-108 mem_size[0]
    b[0x30] = 0                                                         # 110-111 mem_coor[0]
    # bld_cs_set(src format) (g2d_mixer.c 340; g2d_bld.c 227-230)
    out_color = 0 if src['format'] <= G2D_FORMAT_BGRA1010102 else 1 << 1
    # A YUV source and an RGB destination (g2d_mixer.c 359-368): bld_csc_reg_set(2, YUV2RGB_601 or _709, src_cr,
    # dst_cr) (g2d_bld.c 246-354)
    b[0x10] = b[0x14] = 0x00108080                                      # 257-258 bld_fill_color
    b[0x100] = 1 << 2                                                   # 269-271 cs2_en
    table = YCBCR2RGB_601 if gamut == G2D_BT601 else YCBCR2RGB_709
    base = (0x18 if dst_cr == COLOR_RANGE_0_255 else 0) if src_cr == COLOR_RANGE_0_255 else \
        (0x24 if dst_cr == COLOR_RANGE_0_255 else 0xc)                  # 294-310, 328-343
    for i in range(12):
        b[0x170 + 4 * i] = table[base + i] & M32
    # bld_out_setting(dst) (g2d_mixer.c 370; g2d_bld.c 182-189): premul_en 0, out_size
    b[0x60] = out_color
    b[0x48] = ((dst['w'] - 1) if dst['w'] else 0) | ((dst['h'] - 1) if dst['h'] else 0) << 16
    # g2d_wb_set(dst) (g2d_mixer.c 371; g2d_wb.c 100-170), an RGB destination: one 4-byte plane
    wb[0x00] = dst['format'] & 0x3f                                     # 113
    wb[0x04] = ((dst['h'] - 1) & 0x1fff) << 16 | ((dst['w'] - 1) & 0x1fff)  # 114-117
    pitch0 = cal_align(4 * dst['width'], dst['align'][0])               # 145
    wb[0x08], wb[0x0c], wb[0x10] = pitch0, 0, 0                         # 146-150
    addr0 = dst['laddr'][0] + pitch0 * dst['clip_y'] + 4 * dst['clip_x']  # 152-154
    wb[0x14], wb[0x18] = addr0 & M32, (addr0 >> 32) & 0xff              # 155-156
    wb[0x1c], wb[0x20], wb[0x24], wb[0x28] = dst['laddr'][1], 0, dst['laddr'][2], 0  # 157-164: cw 0
    if variant & CSC0:
        # g2d_bsp_bld's order (g2d_mixer.c 216-220): bld_csc_reg_set(0, ...) sets cs0_en and copies the same 12 words
        # to csc0 (g2d_bld.c 261-264, at blender 0x110, g2d_mixer_type.h 452-455), and bld_cs_set(BGRA1010102)
        # (g2d_mixer.c 287) keeps the blender in RGB, out_color 0. csc2 is not written.
        b[0x100] = 1
        for i in range(12):
            b[0x110 + 4 * i], b[0x170 + 4 * i] = b[0x170 + 4 * i], 0
        b[0x60] = 0
    if variant & FILL_Y0:
        b[0x10] = b[0x14] = 0x00008080                                  # g2d_bld.c 257-258 with luma 0
    if variant & NO_FILL:
        b[0x00] = 1 << 8                                                # g2d_bld.c 95 without 97 (p0_fcen)
    if variant & ROUND:
        wb[0x00] |= 1 << 8                                              # wb_attr round_en, g2d_mixer_type.h 508
    return {G2D_V0: v, G2D_VSU: vs, G2D_BLD: b, G2D_WB: wb}


BLOCKS = {G2D_V0: (128, 64), G2D_UI0: (192, 4), G2D_UI1: (224, 4), G2D_UI2: (256, 4), G2D_VSU: (1024, 1152),
          G2D_BLD: (384, 416), G2D_WB: (832, 44)}
ORDER = [G2D_V0, G2D_UI0, G2D_UI1, G2D_UI2, G2D_VSU, G2D_BLD, G2D_WB]
HARNESS = r'''
#include "a733_g2d_convert.h"
#include <stdio.h>
#include <stdlib.h>
static _Alignas(32) unsigned char page[4096];
int main(void)
{
    unsigned v[19];
    while (1) {
        for (int i = 0; i < 19; ++i) if (scanf("%u", &v[i]) != 1) return 0;
        const struct a733_g2d_convert c = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11],
                                           v[12], v[13], v[14], v[15], v[16],
#ifdef SUNSTONE_G2D_CONVERT_VARIANTS
                                           v[18]
#endif
        };
#ifndef SUNSTONE_G2D_CONVERT_VARIANTS
        if (v[18]) return 2;
#endif
        if (!a733_g2d_convert_commands(page, sizeof(page), &c, v[17])) { printf("refused\n"); continue; }
        for (unsigned i = 0; i < sizeof(page); ++i) printf("%02x", page[i]);
        printf("\n");
    }
}
'''


def build(out, variants):
    cc = shutil.which('clang-21') or shutil.which('clang') or shutil.which('gcc')
    (out / 'harness.c').write_text(HARNESS)
    binary = out / ('harness-variants' if variants else 'harness')
    done = subprocess.run([cc, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                           '-fno-sanitize-recover=all', '-Iinclude', '-Idrivers/g2d', str(out / 'harness.c'),
                           'drivers/g2d/a733_g2d_convert.c', '-o', str(binary)]
                          + (['-DSUNSTONE_G2D_CONVERT_VARIANTS=1'] if variants else []),
                          cwd=ROOT, capture_output=True, text=True)
    if done.returncode:
        sys.exit(done.stderr)
    return binary


def job(src, dst, csc, rcq=0x10800000, variant=0):
    """The builder's job for the vendor's images: the vendor's pitches and first-pixel bases."""
    pitch0 = cal_align(src['width'], src['align'][0])
    pitch1 = cal_align(2 * (src['width'] >> 1), src['align'][1])
    return [src['laddr'][0], src['laddr'][1], pitch0, pitch1, src['clip_x'], src['clip_y'], src['w'], src['h'],
            src['format'], dst['laddr'][0], cal_align(4 * dst['width'], dst['align'][0]), dst['clip_x'], dst['clip_y'],
            dst['w'], dst['h'], dst['format'], csc, rcq, variant]


def image(fmt, width, x, y, w, h, laddr, align=(0, 0, 0)):
    return {'format': fmt, 'width': width, 'align': list(align), 'laddr': list(laddr), 'clip_x': x, 'clip_y': y,
            'w': w, 'h': h}


def cases():
    """(name, src, dst, csc) accepted cases."""
    y, c, out = 0x10000000, 0x10050000, 0x10400000
    out_ = []
    for csc in (1, 2, 3, 4):
        out_.append((f'stage-unscaled-{csc}', image(0x24, 640, 0, 0, 640, 480, (y, c, 0)),
                     image(0, 656, 0, 0, 640, 480, (out, 0, 0)), csc))
    for fmt in range(8):
        out_.append((f'out-format-{fmt}', image(0x24, 640, 0, 0, 640, 480, (y, c, 0)),
                     image(fmt, 640, 0, 0, 320, 240, (out, 0, 0)), 1))
    shapes = [((640, 480), (320, 240)), ((640, 480), (224, 224)), ((320, 240), (640, 480)), ((640, 480), (1280, 720)),
              ((640, 480), (160, 120)), ((1792, 64), (224, 64)), ((1920, 1080), (224, 224)), ((1920, 1080), (240, 135)), ((2592, 1944), (320, 240)),
              ((4000, 64), (4000, 64)), ((4096, 16), (3000, 8)), ((641, 3), (17, 1)), ((2, 2), (1, 1)),
              ((1, 1), (8192, 1)), ((8, 8191), (8, 2047)), ((16, 8191), (16, 2048))]
    for (w, h), (ow, oh) in shapes:
        for fmt in (0x24, 0x25):
            out_.append((f'scale-{w}x{h}-{ow}x{oh}-{fmt:#x}', image(fmt, w + (w & 1), 0, 0, w, h, (y, c, 0), (4, 4, 0)),
                         image(0, ow, 0, 0, ow, oh, (out, 0, 0)), 2))
    out_.append(('offsets', image(0x24, 1280, 64, 37, 640, 480, (y, 0x10400000, 0)),
                 image(3, 900, 12, 9, 224, 224, (0x20000000, 0, 0)), 3))
    out_.append(('aligned-pitches', image(0x24, 650, 2, 1, 600, 400, (y, c, 0), (64, 64, 0)),
                 image(5, 330, 1, 1, 320, 240, (out, 0, 0), (64, 0, 0)), 4))
    return out_


def refusals():
    """Jobs outside the builder's contract, as builder word lists."""
    base = job(image(0x24, 640, 0, 0, 640, 480, (0x10000000, 0x10050000, 0)),
               image(0, 640, 0, 0, 320, 240, (0x10400000, 0, 0)), 1)
    def with_(**kw):
        names = ['luma', 'chroma', 'luma_stride', 'chroma_stride', 'sx', 'sy', 'w', 'h', 'format', 'output',
                 'output_stride', 'dx', 'dy', 'ow', 'oh', 'out_format', 'csc', 'rcq', 'variant']
        j = list(base)
        for k, value in kw.items():
            j[names.index(k)] = value
        return j
    return [('odd-sx', with_(sx=1, w=638)), ('w-0', with_(w=0)), ('w-8193', with_(w=8193, luma_stride=8196,
            chroma_stride=8196)), ('h-0', with_(h=0)), ('ow-0', with_(ow=0)), ('oh-8193', with_(oh=8193)),
            ('out-format-8', with_(out_format=8)), ('csc-0', with_(csc=0)), ('csc-5', with_(csc=5)),
            ('nv12', with_(format=0x28)), ('planar', with_(format=0x26)), ('rgb', with_(format=0)),
            ('luma-stride', with_(luma_stride=636)), ('chroma-stride', with_(chroma_stride=636)),
            ('output-stride', with_(output_stride=1276)), ('luma-align', with_(luma=0x10000002)),
            ('chroma-align', with_(chroma=0x10050002)), ('output-align', with_(output=0x10400002)),
            ('stride-align', with_(luma_stride=642, chroma_stride=642)), ('rcq-align', with_(rcq=0x10800040)),
            ('output-past-4g', with_(output=0xffff0000, dy=200)), ('luma-past-4g', with_(luma=0xffff0000, sy=479))]


def variant_cases():
    """(name, src, dst, csc, variant) cases for the variant build: every combination on the stage's unscaled job, and
    each single variant on a scaled job with the other tables."""
    y, c, out = 0x10000000, 0x10050000, 0x10400000
    full = [(f'variant-{v}', image(0x24, 640, 0, 0, 640, 480, (y, c, 0)), image(0, 656, 0, 0, 640, 480, (out, 0, 0)),
             4, v) for v in range(16)]
    return full + [(f'variant-{v}-scale-csc{csc}', image(0x24, 640, 0, 0, 640, 480, (y, c, 0)),
                    image(0, 320, 0, 0, 320, 240, (out, 0, 0)), csc, v) for v in (1, 2, 4, 8) for csc in (1, 2, 3)]


def variant_refusals():
    base = job(image(0x24, 640, 0, 0, 640, 480, (0x10000000, 0x10050000, 0)),
               image(0, 640, 0, 0, 320, 240, (0x10400000, 0, 0)), 1)
    return [(f'variant-{v:#x}', base[:-1] + [v]) for v in (16, 17, 0x80000000, 0xffffffff)]


def run(binary, lines):
    done = subprocess.run([str(binary)], input='\n'.join(lines) + '\n', capture_output=True, text=True)
    if done.returncode:
        sys.exit(f'{binary.name} exited {done.returncode}: {done.stderr}')
    return done.stdout.split('\n')


def parse(page):
    """{block offset: (bytes at, length)} from the page's headers, and the terminator's words."""
    heads = [tuple(int.from_bytes(page[16 * i + 4 * k:16 * i + 4 * k + 4], 'little') for k in range(4)) for i in range(8)]
    return heads


def words(page, at, n):
    return {4 * i: int.from_bytes(page[at + 4 * i:at + 4 * i + 4], 'little') for i in range(n // 4)}


def check_page(name, page, want, rcq):
    problems = []
    heads = parse(page)
    for i, blk in enumerate(ORDER):
        at, n = BLOCKS[blk]
        if heads[i] != (rcq + at, n, 1, blk):
            problems.append(f'{name}: header {i} {heads[i]}, want {(rcq + at, n, 1, blk)}')
    if heads[7] != (0, 0, 0, 0):
        problems.append(f'{name}: terminator {heads[7]}')
    for blk in ORDER:
        at, n = BLOCKS[blk]
        got = words(page, at, n)
        exp = {off: 0 for off in got}
        exp.update(want.get(blk, {}))
        bad = [(hex(off), hex(got[off]), hex(exp[off])) for off in got if got[off] != exp[off]]
        if bad:
            problems.append(f'{name}: block {blk:#x} words {bad[:6]}')
    used = set()
    for blk in ORDER:
        at, n = BLOCKS[blk]
        used |= set(range(at, at + n))
    used |= set(range(128))
    stray = [i for i in range(4096) if i not in used and page[i]]
    if stray:
        problems.append(f'{name}: bytes outside every block are set: {stray[:8]}')
    return problems


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    problems = []
    lan2 = lan2_bank(ROOT / 'drivers/g2d/a733_g2d_convert.c')
    if lan2 != lan2_bank(ROOT / 'drivers/g2d/a733_g2d.c'):
        problems.append('the conversion builder\'s Lanczos bank differs from drivers/g2d/a733_g2d.c\'s')
    sums = {}
    for bank in range(16):
        for phase in range(32):
            word = lan2[bank * 32 + phase]
            taps = [((word >> (8 * k)) & 0xff) - (256 if (word >> (8 * k)) & 0x80 else 0) for k in range(4)]
            if sum(taps) != 64:
                sums[(bank, phase)] = sum(taps)
    if sums:
        problems.append(f'phases whose taps do not sum to 64: {sorted(sums.items())[:8]}')
    accepted = [(name, src, dst, csc, 0) for name, src, dst, csc in cases()]
    rejected = refusals()
    variants = variant_cases()
    vrejected = variant_refusals()
    def lines(acc, rej):
        return [' '.join(str(x) for x in job(src, dst, csc, variant=v)) for _, src, dst, csc, v in acc] + \
            [' '.join(str(x) for x in j) for _, j in rej]
    shipped = run(build(a.output, False), lines(accepted, rejected))
    built = run(build(a.output, True), lines(accepted + variants, rejected + vrejected))
    for label, out, acc, rej in (('', shipped, accepted, rejected),
                                 ('variant build ', built, accepted + variants, rejected + vrejected)):
        for k, (name, src, dst, csc, v) in enumerate(acc):
            if out[k] == 'refused':
                problems.append(f'{label}{name}: refused')
                continue
            page = bytes.fromhex(out[k])
            problems += check_page(label + name, page, vendor_blocks(src, dst, *CSC[csc], lan2, v), 0x10800000)
        for k, (name, _) in enumerate(rej):
            if out[len(acc) + k] != 'refused':
                problems.append(f'{label}{name}: accepted')
    (a.output / 'result.json').write_text(json.dumps({'passed': not problems, 'problems': problems,
                                                      'accepted': [c[0] for c in accepted + variants],
                                                      'refused': [r[0] for r in rejected + vrejected]}, indent=2) + '\n')
    if problems:
        sys.exit('FAIL ' + '; '.join(problems[:12]))
    print(f'PASS the conversion builder gives the vendor\'s words for {len(accepted)} jobs and refuses {len(rejected)}; '
          f'built with the variants, it gives the same words, the variants\' words for {len(variants)} jobs and refuses '
          f'{len(vrejected)} variant values; its Lanczos bank is a733_g2d.c\'s and every phase sums to 64')


if __name__ == '__main__':
    main()
