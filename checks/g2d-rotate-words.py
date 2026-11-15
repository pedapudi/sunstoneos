#!/usr/bin/env python3
"""Compare the G2D rotate builder with the vendor's register sequence, word for word.

drivers/g2d/a733_g2d_rotate.c gives the rotate module's register words for one job. vendor_words() below transcribes
the vendor's g2d_rotate_set_para (g2d_rotate.c 106-346 in linux-orangepi 2ac08e8c, bsp/drivers/g2d/g2d_rcq, with
g2d_byte_cal and cal_align from g2d.c 351-450) for a single-plane 32-bit format on the physical-address path, with the
second and third plane addresses 0 and every rectangle inside its surface, so that g2d_image_check changes nothing.
Each line cites the vendor line it follows.

The check builds the builder on the host and runs it on every combination of the four degrees, the four flip
settings, three formats and several geometries, including a 40-bit address. Each accepted job must give the vendor's
writes in the vendor's order. Each job the vendor refuses must be refused, and so must the jobs that the vendor would
program wrongly because a field overflows (a width above 8192, for example).

Usage: g2d-rotate-words.py --output DIR"""
import argparse
import itertools
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROT = 0x28000
G2D_ROT_90, G2D_ROT_180, G2D_ROT_270, G2D_ROT_0 = 0x100, 0x200, 0x300, 0x400
G2D_ROT_H, G2D_ROT_V = 0x1000, 0x2000
FLAG = {0: G2D_ROT_0, 90: G2D_ROT_90, 180: G2D_ROT_180, 270: G2D_ROT_270}


def cal_align(width, align):
    """g2d.c 421-450: width rounded up to align, a power of two from 2 to 1024, or unchanged for 0."""
    return width if align == 0 else (width + align - 1) // align * align


def vendor_words(src, dst, flag):
    """The vendor's writes for one job, as (offset, value) pairs in order, and whether it starts the module.

    src and dst are dicts with format, width, align (align[0]), laddr (laddr[0], 40 bits) and the clip rectangle x, y,
    w, h. The vendor writes every register before its alignment checks, so a refused job returns writes but no start.
    """
    writes = []
    put = lambda offset, value: writes.append((ROT + offset, value & 0xffffffff))
    tmp = 1                                                       # 152
    if flag & G2D_ROT_H:
        tmp |= 1 << 7                                             # 153-154
    if flag & G2D_ROT_V:
        tmp |= 1 << 6                                             # 155-156
    tmp |= {G2D_ROT_90: 1, G2D_ROT_180: 2, G2D_ROT_270: 3, G2D_ROT_0: 0}[flag & 0xf00] << 4  # 157-164
    put(0x00, tmp)                                                # 166 ROT_CTL
    put(0x20, src['format'] & 0x3f)                               # 167 ROT_IFMT
    put(0x24, ((src['h'] - 1) & 0x1fff) << 16 | ((src['w'] - 1) & 0x1fff))  # 168-169 ROT_ISIZE
    ycnt, ucnt, vcnt, cw, cx, cy = 4, 0, 0, 0, 0, 0               # 195-200, g2d_byte_cal 356-357
    pitch0 = cal_align(ycnt * src['width'], src['align'])         # 203
    pitch1 = cal_align(ucnt * cw, 0)                              # 205
    pitch2 = cal_align(vcnt * cw, 0)                              # 207
    put(0x30, pitch0); put(0x34, pitch1); put(0x38, pitch2)       # 204, 206, 208
    addr0 = src['laddr'] + pitch0 * src['y'] + ycnt * src['x']    # 210-211
    addr1 = 0 + pitch1 * cy + ucnt * cx                           # 214-215, laddr[1] 0
    addr2 = 0 + pitch2 * cy + vcnt * cx                           # 218-219, laddr[2] 0
    for at, addr in ((0x40, addr0), (0x48, addr1), (0x50, addr2)):
        put(at, addr & 0xffffffff); put(at + 4, (addr >> 32) & 0xff)  # 212-221
    if addr0 % 4 or addr1 % 4 or addr2 % 4:                       # 223-226
        return writes, False
    if (flag & 0xf00) in (G2D_ROT_90, G2D_ROT_270):              # 235-238
        dw, dh = src['h'], src['w']
    else:                                                         # 240-243
        dw, dh = src['w'], src['h']
    put(0x84, ((dh - 1) & 0x1fff) << 16 | ((dw - 1) & 0x1fff))    # 244-245 ROT_OSIZE
    # 246-254: the output format is the input's for an RGB source; 256-285 give one plane again.
    opitch0 = cal_align(ycnt * dst['width'], dst['align'])       # 289
    opitch1, opitch2 = cal_align(0, 0), cal_align(0, 0)           # 291, 293
    put(0x90, opitch0); put(0x94, opitch1); put(0x98, opitch2)    # 290, 292, 294
    if opitch0 % 8 or opitch1 % 8 or opitch2 % 8:                 # 296-299
        return writes, False
    oaddr0 = dst['laddr'] + opitch0 * dst['y'] + ycnt * dst['x']  # 301-302
    for at, addr in ((0xa0, oaddr0), (0xa8, 0), (0xb0, 0)):
        put(at, addr & 0xffffffff); put(at + 4, (addr >> 32) & 0xff)  # 303-312
    if oaddr0 % 4:                                                # 314-317
        return writes, False
    put(0x04, 0x10000)                                            # 327 ROT_INT
    put(0x00, tmp | 1 << 31)                                      # 328-330 ROT_CTL start
    return writes, True


def job_line(j):
    return ' '.join(str(j[k]) for k in ('source', 'output', 'source_stride', 'output_stride', 'sx', 'sy', 'dx', 'dy',
                                        'w', 'h', 'format', 'degrees', 'flip'))


def vendor_inputs(j):
    """The vendor's src, dst and flag for one builder job. Each stride is given as the narrowest pixel width that
    holds the rectangle and that cal_align rounds up to the stride at alignment 64, so that the vendor's pitch
    arithmetic runs, or as the stride's own width at alignment 0 when no such width exists. A width that holds the
    rectangle keeps g2d_image_check from clipping it."""
    def width_align(stride, least):
        for width in range(max(least, 1), stride // 4 + 1):
            if cal_align(4 * width, 64) == stride:
                return width, 64
        return stride // 4, 0

    def image(stride, base, x, y, w, h, fmt):
        width, align = width_align(stride, x + w)
        return {'format': fmt, 'width': width, 'align': align, 'laddr': base, 'x': x, 'y': y, 'w': w, 'h': h}
    flag = FLAG[j['degrees']] | (G2D_ROT_H if j['flip'] & 1 else 0) | (G2D_ROT_V if j['flip'] & 2 else 0)
    src = image(j['source_stride'], j['source'], j['sx'], j['sy'], j['w'], j['h'], j['format'])
    ow, oh = (j['h'], j['w']) if j['degrees'] in (90, 270) else (j['w'], j['h'])
    dst = image(j['output_stride'], j['output'], j['dx'], j['dy'], ow, oh, j['format'])
    return src, dst, flag


GEOMETRIES = [
    # The first stage's shape: a 96 by 40 rectangle of a 400 by 260 surface, stride 1664.
    dict(source=0x10200000, output=0x10000000, source_stride=1664, output_stride=1664, sx=24, sy=16, dx=150, dy=52,
         w=96, h=40),
    dict(source=0x10200000, output=0x10000000, source_stride=1664, output_stride=1664, sx=0, sy=0, dx=0, dy=0,
         w=1, h=1),
    dict(source=0x10200000, output=0x10000000, source_stride=1600, output_stride=1040, sx=7, sy=3, dx=9, dy=5,
         w=17, h=9),
    # The largest extent in one dimension, and a 40-bit address that needs the high words.
    dict(source=0x10000000, output=0x40000000, source_stride=32768, output_stride=32768, sx=0, sy=0, dx=0, dy=0,
         w=8192, h=1),
    dict(source=0x8_1000_0000, output=0x7f_ffff_0000, source_stride=1664, output_stride=1664, sx=24, sy=16, dx=4,
         dy=2, w=96, h=40),
]
# Jobs the vendor refuses after writing registers, and jobs whose fields overflow. Each must be refused before any
# write.
REFUSED = [
    ('input origin not 4-byte aligned', dict(GEOMETRIES[0], source=0x10200002), True),
    ('output stride not 8-byte aligned', dict(GEOMETRIES[0], output_stride=1660), True),
    ('output origin not 4-byte aligned', dict(GEOMETRIES[0], output=0x10000002), True),
    ('width 0', dict(GEOMETRIES[0], w=0), False),
    ('width 8193', dict(GEOMETRIES[3], w=8193, source_stride=32776, output_stride=32776), False),
    ('height 8193', dict(GEOMETRIES[1], h=8193), False),
    ('degrees 45', dict(GEOMETRIES[0], degrees=45), False),
    ('flip 4', dict(GEOMETRIES[0], flip=4), False),
    ('format 8', dict(GEOMETRIES[0], format=8), False),
    ('rectangle wider than the source stride', dict(GEOMETRIES[0], w=417), False),
    ('rotated rectangle wider than the output stride', dict(GEOMETRIES[0], w=40, h=417, degrees=90), False),
    ('origin at 2^40', dict(GEOMETRIES[0], source=1 << 40), False),
]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    cc = shutil.which('clang-21') or shutil.which('clang') or shutil.which('gcc')
    binary = a.output / 'g2d-rotate-words'
    command = [cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-Iinclude', 'drivers/g2d/a733_g2d_rotate.c',
               'checks/g2d-rotate-words/main.c', '-o', str(binary)]
    done = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if done.returncode:
        sys.exit('FAIL build: ' + done.stderr[-2000:])
    jobs = []
    for g, degrees, flip, fmt in itertools.product(GEOMETRIES, (0, 90, 180, 270), range(4), (0, 3, 7)):
        j = dict(g, degrees=degrees, flip=flip, format=fmt)
        if degrees in (90, 270) and j['h'] * 4 > j['output_stride']:
            continue
        jobs.append(('accepted', j, True))
    jobs += [(name, dict({'degrees': 0, 'flip': 0, 'format': 0}, **j), vendor) for name, j, vendor in REFUSED]
    run = subprocess.run([str(binary)], input=''.join(job_line(j) + '\n' for _, j, _ in jobs), capture_output=True,
                         text=True)
    if run.returncode:
        sys.exit('FAIL run: ' + run.stderr[-2000:])
    lines = run.stdout.splitlines()
    problems, rows = [], []
    for (name, j, vendor), line in zip(jobs, lines, strict=True):
        fields = line.split()
        got = [tuple(int(x, 16) for x in f.split('=')) for f in fields[1:]]
        row = {'case': name, 'job': j, 'count': int(fields[0])}
        if name == 'accepted':
            want, started = vendor_words(*vendor_inputs(j))
            row['vendor'] = [(f'0x{o:05x}', f'0x{v:08x}') for o, v in want]
            if not started or got != want:
                problems.append(f'{j}: builder {got[:4]}..., vendor {want[:4]}... (started {started})')
        else:
            if vendor:
                _, started = vendor_words(*vendor_inputs(j))
                row['vendor_starts'] = started
                if started:
                    problems.append(f'{name}: the vendor transcription starts the module')
            if got:
                problems.append(f'{name}: the builder gave {len(got)} words')
        rows.append(row)
    accepted = sum(1 for n, _, _ in jobs if n == 'accepted')
    (a.output / 'result.json').write_text(json.dumps({'passed': not problems, 'problems': problems, 'rows': rows},
                                                     indent=1) + '\n')
    if problems:
        sys.exit(f'FAIL {len(problems)} of {len(jobs)} jobs: ' + '; '.join(problems[:5]))
    print(f'PASS G2D rotate words: {accepted} jobs over four degrees, four flip settings, three formats and '
          f'{len(GEOMETRIES)} geometries equal the vendor sequence word for word; {len(REFUSED)} jobs refused '
          f'before any write')


if __name__ == '__main__':
    main()
