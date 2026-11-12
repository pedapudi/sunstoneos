#!/usr/bin/env python3
"""The compiler's estimate for each operation of a compiled graph, in the graph's operation order.

  op-estimates.py --native LIB --model network_binary.nb --log export.log

The export log holds one estimate block (CycleCount, MAC, ReadBW, WriteBW and the image and kernel sizes) for most operations, in
execution order, and none for shader operations and some transposes. The graph's operation kinds come from the OS decoder
(checks/nbg-model.py; LIB is its shared library, as tools/npu-nbg-census.py builds it). Blocks are matched to operations in order:
a block with no stride (stridex 0) is a neural transpose (kind 8); a block with multiply-accumulates or a kernel wider than 1 is a
neural-network operation (kind 2); any other block is a stream-processor, activation or neural-network operation.
Every block must match; an operation that matches none has no estimate. Prints JSON: one entry per operation after INIT.
"""
import argparse, importlib.util, json, re
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
KIND = {1: 'SH', 2: 'NN', 3: 'TP', 4: 'SW', 6: 'SP', 7: 'ACT', 8: 'NT', 65535: 'INIT'}
FIELDS = ('CycleCount', 'MAC', 'ReadBW', 'WriteBW', 'stridex', 'KernelX', 'KernelY', 'KernelZ', 'OrigInImageX', 'OrigInImageY',
          'OrigInImageZ', 'OutImageX', 'OutImageY', 'OutImageZ', 'PoolingSize', 'kernelDDRReadBW', 'InImageDDrReadBW')


def blocks(log):
    out, cur = [], {}
    for line in Path(log).read_text(errors='replace').splitlines():
        m = re.match(r'\s*([A-Za-z_0-9]+)\s*:\s*(-?\d+)(?:\s|$)', line)
        if m and m.group(1) in FIELDS:
            cur.setdefault(m.group(1), int(m.group(2)))
            if m.group(1) == 'CycleCount':
                out.append(cur); cur = {}
    return out


def fits(kind, b):
    if b.get('stridex', 1) == 0:
        return kind == 'NT'
    if b.get('MAC', 0) or b.get('KernelX', 1) > 1:
        return kind == 'NN'
    return kind in ('NN', 'SP', 'ACT')


def align(kinds, bs):
    """Match every block to an operation in order (the earliest operation that fits and leaves room for the rest)."""
    n, m = len(kinds), len(bs)
    # can[i][j]: blocks j.. can be matched within operations i..
    can = [[False] * (m + 1) for _ in range(n + 1)]
    for i in range(n + 1):
        can[i][m] = True
    for i in range(n - 1, -1, -1):
        for j in range(m - 1, -1, -1):
            can[i][j] = can[i + 1][j] or (fits(kinds[i], bs[j]) and can[i + 1][j + 1])
    if not can[0][0]:
        raise SystemExit('the estimate blocks do not match the operations in order')
    match, j = [None] * n, 0
    for i in range(n):
        if j < m and fits(kinds[i], bs[j]) and can[i + 1][j + 1]:
            match[i] = j; j += 1
    return match


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--native', type=Path, required=True)
    p.add_argument('--model', type=Path, required=True)
    p.add_argument('--log', type=Path, required=True)
    a = p.parse_args()
    spec = importlib.util.spec_from_file_location('nm', ROOT / 'checks/nbg-model.py')
    nm = importlib.util.module_from_spec(spec); spec.loader.exec_module(nm)
    ops = nm.Native(a.native, a.model.read_bytes()).operations()
    kinds = [KIND.get(o[0], str(o[0])) for o in ops if o[0] != 65535]
    bs = blocks(a.log)
    match = align(kinds, bs)
    print(json.dumps([{'operation': i + 1, 'kind': k, 'estimate': bs[match[i]] if match[i] is not None else None}
                      for i, k in enumerate(kinds)], indent=1))


if __name__ == '__main__':
    main()
