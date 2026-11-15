#!/usr/bin/env python3
"""Check the G2D YUV-to-RGB tables that tools/g2d-csc-tables.py derives from the BT.601 and BT.709 matrices.

- include/a733_g2d_csc_tables.h, which the conversion builder includes, is the generator's output, and the decoder's
  copy (apps/g2d-convert/host.py TABLES) holds the same words.
- Every value fits its register field: a coefficient 13 signed bits and a constant 20 (g2d_mixer_type.h).
- Each table, run through the converter's integer arithmetic ((cy Y + cu U + cv V + k) >> 10, clamped to 0 to 255), is
  within 1 of the real matrix rounded to nearest and clamped, in every channel, over all 2^24 (Y, U, V) inputs.
- Two mutant converters, one without the lower clamp and one without the upper, must each exceed that bound, and the
  luma-sweep frame of the third conversion stage (apps/g2d-convert) must drive every table into both clamps, so the
  board's CRC-32 of that frame tests both bounds.
- Each derived table is compared with the vendor block that the builder programmed before (g2d_bld.c Ycbcr2rgb_601 and
  Ycbcr2rgb_709): the result names every word that differs and each channel's output at neutral chroma (U = V = 128)
  for Y 0, 16, 128, 235 and 255 under both. The vendor's BT.709 full block (Ycbcr2rgb_709 words 24 to 35) subtracts 16
  from luma at neutral chroma; the check requires the derived one not to, and requires every derived table to map
  neutral chroma to grey with the standard's luma scale.

Usage: g2d-csc-tables.py --output DIR"""
import argparse
import importlib.util
import json
import sys
from pathlib import Path

import numpy as np

GATE_RUNS = [['--output', '{output}']]
ROOT = Path(__file__).resolve().parents[1]
NAMES = ('bt601-limited', 'bt601-full', 'bt709-limited', 'bt709-full')
# The vendor blocks the builder programmed before the tables were derived: (array, first word) of g2d_bld.c 37-50
# and 66-79.
VENDOR_BLOCKS = (('601', 0), ('601', 24), ('709', 0), ('709', 24))
GREY = (0, 16, 128, 235, 255)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def signed(word, bits):
    word &= (1 << bits) - 1
    return word - (1 << bits) if word >> (bits - 1) else word


def convert(values, y, u, v, low=True, high=True):
    """The converter's three channels for int64 arrays y, u, v; low and high select the clamps."""
    out = []
    for c in range(3):
        cy, cu, cv, k = (signed(values[4 * c + i], 20 if i == 3 else 13) for i in range(4))
        x = (cy * y + cu * u + cv * v + k) >> 10
        if low:
            x = np.maximum(x, 0)
        if high:
            x = np.minimum(x, 255)
        out.append(x)
    return out


def reference(rows, y0, y, u, v):
    out = []
    for a, b, c in rows:
        x = a * (y - y0) + b * (u - 128.0) + c * (v - 128.0)
        out.append(np.clip(np.floor(x + 0.5), 0, 255))
    return out


def errors(values, rows, y0, low=True, high=True):
    """The largest absolute error per channel over all 2^24 inputs, and the number of inputs off by exactly 1."""
    worst, ones = [0, 0, 0], [0, 0, 0]
    u, v = (g.astype(np.int64) for g in np.meshgrid(np.arange(256), np.arange(256), indexing='ij'))
    for yv in range(256):
        y = np.full(u.shape, yv, dtype=np.int64)
        got, want = convert(values, y, u, v, low, high), reference(rows, y0, y, u, v)
        for c in range(3):
            d = np.abs(got[c] - want[c])
            worst[c] = max(worst[c], int(d.max()))
            ones[c] += int((d == 1).sum())
    return worst, ones


def grey(values):
    y = np.array(GREY, dtype=np.int64)
    n = np.full(y.shape, 128, dtype=np.int64)
    return [[int(x) for x in channel] for channel in convert(values, y, n, n)]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    gen = load('g2d_csc_tables', 'tools/g2d-csc-tables.py')
    host = load('g2d_convert_host', 'apps/g2d-convert/host.py')
    problems, report = [], {}
    if not gen.HEADER.exists() or gen.HEADER.read_text() != gen.header():
        problems.append('include/a733_g2d_csc_tables.h is not the generator\'s output')
    derived = gen.tables()
    if [list(values) for _, values in derived] != [list(host.TABLES[k + 1]) for k in range(4)]:
        problems.append('the decoder\'s tables differ from the generator\'s')
    frame, _ = host.frames()
    for k, ((name, values), (_, kr, kb, limited), (array, first)) in enumerate(zip(derived, gen.TABLES, VENDOR_BLOCKS)):
        key = NAMES[k]
        for i, value in enumerate(values):
            bits = 20 if i % 4 == 3 else 13
            if not gen.fits(value, bits):
                problems.append(f'{key} value {i} ({value}) does not fit {bits} signed bits')
        rows, y0 = gen.real(kr, kb, limited)
        worst, ones = errors(values, rows, y0)
        if max(worst) > 1:
            problems.append(f'{key} is {worst} from the real matrix')
        no_low, _ = errors(values, rows, y0, low=False)
        no_high, _ = errors(values, rows, y0, high=False)
        if max(no_low) <= 1 or max(no_high) <= 1:
            problems.append(f'{key}: a converter without a clamp stays within 1 ({no_low}, {no_high})')
        # The luma-sweep frame's conversion must reach both clamps in some channel.
        whole = host.conversion_frame(frame, k + 1)
        channels = np.stack([(whole >> s) & 255 for s in (16, 8, 0)])
        y, u, v = (np.asarray(x, dtype=np.int64) for x in host.conversion_inputs(frame))
        raw = [(signed(values[4 * c], 13) * y + signed(values[4 * c + 1], 13) * u + signed(values[4 * c + 2], 13) * v +
                signed(values[4 * c + 3], 20)) >> 10 for c in range(3)]
        below, above = sum(int((r < 0).sum()) for r in raw), sum(int((r > 255).sum()) for r in raw)
        if not below or not above:
            problems.append(f'{key}: the luma-sweep frame does not reach both clamps ({below} below, {above} above)')
        if int(channels.min()) != 0 or int(channels.max()) != 255:
            problems.append(f'{key}: the luma-sweep frame\'s output does not span 0 to 255')
        vendor = [w & 0xffffffff for w in host.VENDOR[array][first:first + 12]]
        mine = [w & 0xffffffff for w in values]
        differs = [{'word': i, 'channel': 'RGB'[i // 4], 'field': ('Y', 'U', 'V', 'constant')[i % 4],
                    'vendor': signed(vendor[i], 20 if i % 4 == 3 else 13),
                    'derived': signed(mine[i], 20 if i % 4 == 3 else 13)} for i in range(12) if vendor[i] != mine[i]]
        vendor_grey, derived_grey = grey(host.VENDOR[array][first:first + 12]), grey(values)
        scale = 255 / 219 if limited else 1.0
        want_grey = [int(np.clip(np.floor(scale * (g - y0) + 0.5), 0, 255)) for g in GREY]
        if any(channel != want_grey for channel in derived_grey):
            problems.append(f'{key}: neutral chroma gives {derived_grey}, not {want_grey}')
        report[key] = {'max_error': worst, 'inputs_off_by_one': ones, 'no_low_clamp_max_error': no_low,
                       'no_high_clamp_max_error': no_high, 'frame_clamped_below': below, 'frame_clamped_above': above,
                       'vendor_block': f'Ycbcr2rgb_{array} words {first} to {first + 11}', 'words_differ': differs,
                       'grey_inputs': list(GREY), 'vendor_grey': vendor_grey, 'derived_grey': derived_grey}
    v709 = report.get('bt709-full', {}).get('vendor_grey')
    if not v709 or v709[0][2:] != [112, 219, 239]:
        problems.append(f'the vendor BT.709 full block\'s -16 luma offset was not found: {v709}')
    (a.output / 'result.json').write_text(json.dumps({'passed': not problems, 'problems': problems, 'tables': report},
                                                     indent=2) + '\n')
    if problems:
        sys.exit('FAIL ' + '; '.join(problems[:12]))
    worst = max(max(r['max_error']) for r in report.values())
    print(f'PASS the four derived tables fit their fields and are within {worst} of the BT.601 and BT.709 matrices over '
          f'all 2^24 inputs; each clamp mutant exceeds 1 and the luma-sweep frame reaches both clamps; the vendor\'s '
          f'BT.709 full block maps grey 128, 235 and 255 to {v709[0][2:]}, the derived one to '
          f'{report["bt709-full"]["derived_grey"][0][2:]}; words differing from the vendor: '
          f'{ {k: len(r["words_differ"]) for k, r in report.items()} }')


if __name__ == '__main__':
    main()
