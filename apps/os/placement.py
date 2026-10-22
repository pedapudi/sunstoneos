#!/usr/bin/env python3
"""Generate the integrated OS image's placement table for mimus linear layers.

The tool reads the base and overlay model files, checks their SHA-256 against
apps/bluetooth/recognition-models.json (the hashes the image embeds in
mimus_asset_sha256), and lists the merged model's tensors in the order that
mimus's Model::Load builds them: base tensors in file order with overlay
replacements in place, then the overlay's new tensors in file order. A
tensor's index in that list is its ordinal.

For every tensor that a bench-recording call reaches and that the GPU kernel
can run (int8 weights, int16 activations, K a multiple of 4), the time model
below chooses n_gpu, the output channels the GPU computes, from the core set,
the CPU and GPU clocks and their MAC rates. job_rows is the largest multiple
of 32 rows that keeps a job within the kernel's 64 Mi MAC cap and one 2 MiB
sum slot. Every other tensor gets n_gpu = 0 and stays on the CPU.

  placement.py --models BASE OVERLAY --cores a55 --output placement.h
  placement.py --models BASE OVERLAY --cores a55 --calls 222   # call list
  placement.py --models BASE OVERLAY --cores a55 --summary     # time model

docs/recognition-dispatch.md describes the time model and its seeds. No
hardware is accessed.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / 'apps/bluetooth/recognition-models.json'

# Model file format (mimus train/model_format.py): 256-byte header, then a
# table of 64-byte tensor entries.
HEADER = struct.Struct('<4sHHIIIIIIIIQ32s32s')
ENTRY = struct.Struct('<IBBHI4IIIIIIIIII')
DTYPE_I8, ACT_INT16 = 2, 3

# Kernel limits from the GPU kernel design.
MAX_JOB_MACS = 1 << 26
SLOT_BYTES = 2 << 20
TILE_ROWS, TILE_CHANNELS, LANES = 32, 16, 32
SUBSAMPLING_BLOCK = 224   # rows per subsampling producer block
OUT_CHUNK = 32            # mimus kOutChunk: subsampling output rows per call
SUBSAMPLING = 'encoder.pre_encode.conv.2.weight'

# Time model seeds; board measurements replace them.
USC_INSTRUCTIONS_PER_MAC = 1.75   # inner loop of the GPU kernel
A55_MAC_PER_CYCLE = 4167910560 / 14.894 / 494e6   # NEON profile, one A55
A76_MAC_FACTOR = 2.0              # A76 NEON MACs per clock against an A55
READOUT_INSTRUCTIONS = 12.0       # per GPU output, conversion and scaling
LINE_CYCLES = 20.0                # one DC CVAC or DC CIVAC line
SUBMIT_US, FIRMWARE_US, WAKE_US, DISPATCH_US = 25.0, 25.0, 25.0, 10.0
CORES = {'a55': {'mhz': 494.0, 'pair': (2, 3), 'factor': 1.0, 'ipc': 1.0},
         'a76': {'mhz': 2002.0, 'pair': (6, 7), 'factor': A76_MAC_FACTOR, 'ipc': 2.0}}


def read_model(path):
    data = path.read_bytes()
    magic, major, _minor, flags, count, table, strings, *_rest = HEADER.unpack_from(data, 0)
    assert magic == b'MIMS' and major == 1, '%s is not a mimus model file' % path
    tensors = []
    for i in range(count):
        e = ENTRY.unpack_from(data, table + ENTRY.size * i)
        start = strings + e[0]
        tensors.append({'name': data[start:data.index(b'\0', start)].decode(), 'dtype': e[1],
                        'act': e[2], 'n': e[5], 'k': e[6], 'k_padded': e[9], 'row_stride': e[10]})
    return flags, tensors


def merged_tensors(base, overlay):
    """Tensors in Model::Load order; the index is the ordinal."""
    models = json.loads(MANIFEST.read_text())['models']
    for path, model in zip((base, overlay), models):
        assert hashlib.sha256(path.read_bytes()).hexdigest() == model['sha256'], \
            '%s differs from the model the image embeds' % path
    base_flags, merged = read_model(base)
    overlay_flags, additions = read_model(overlay)
    assert base_flags == 1 and overlay_flags == 2, 'base and overlay files required'
    index = {t['name']: i for i, t in enumerate(merged)}
    for t in additions:
        if t['name'] in index:
            merged[index[t['name']]] = t
        else:
            index[t['name']] = len(merged)
            merged.append(t)
    return merged


def width(k):
    """Sum width in bytes: int32 while 512 products cannot overflow, else int64."""
    return 4 if k <= 512 else 8


def job_rows(k, n):
    rows = min(MAX_JOB_MACS // (k * n), SLOT_BYTES // (n * width(k)))
    return rows // TILE_ROWS * TILE_ROWS


def eligible(t):
    return t['dtype'] == DTYPE_I8 and t['act'] == ACT_INT16 and t['k'] % 4 == 0 and \
        t['k_padded'] == t['k'] and t['row_stride'] % 4 == 0


def calls(name, frames):
    """Row counts of the calls that reach a tensor for an utterance of
    `frames` encoder frames, or None for a tensor no bench call reaches."""
    if name == SUBSAMPLING:
        rows = 20 * frames   # 3 x 3 windows over 20 frequency positions per frame
        return [SUBSAMPLING_BLOCK] * (rows // SUBSAMPLING_BLOCK) + \
            ([rows % SUBSAMPLING_BLOCK] if rows % SUBSAMPLING_BLOCK else [])
    if name == 'encoder.pre_encode.out.weight':
        return [OUT_CHUNK] * (frames // OUT_CHUNK) + ([frames % OUT_CHUNK] if frames % OUT_CHUNK else [])
    if name.endswith('self_attn.linear_pos.weight'):
        return [2 * frames - 1]
    if name.startswith('encoder.layers.') or name == 'decoder.decoder_layers.0.weight':
        return [frames]
    return None


def residual(name):
    """Calls that add alpha times their output into the residual stream."""
    return name.endswith(('linear2.weight', 'linear_out.weight', 'pointwise_conv2.weight'))


def split(rows, k, n, block=False):
    """Jobs of one call as the GPU hook issues them: at most job_rows rows,
    boundaries on multiples of 32, and two jobs for a call of more than 32
    rows so that the first job's read-out overlaps the second job's GPU work.
    A subsampling producer block (block=True) is one job."""
    limit = job_rows(k, n)
    parts = math.ceil(rows / limit)
    if parts == 1 and rows > TILE_ROWS and not block:
        parts = 2
    jobs, left = [], rows
    for i in range(parts, 0, -1):
        take = left if i == 1 else min(limit, max(TILE_ROWS, round(left / i / TILE_ROWS) * TILE_ROWS))
        jobs.append(take)
        left -= take
    return jobs


class Clocks:
    def __init__(self, cores='a55', cpu_mhz=None, gpu_mhz=400.0, cpu_rate=None, gpu_rate=None):
        c = CORES[cores]
        self.cores, self.hz, self.ipc = cores, (cpu_mhz or c['mhz']) * 1e6, c['ipc']
        self.cpu_rate = cpu_rate or A55_MAC_PER_CYCLE * c['factor'] * self.hz
        self.gpu_rate = gpu_rate or gpu_mhz * 1e6 * LANES / USC_INSTRUCTIONS_PER_MAC


def gpu_seconds(rows, k, n, clocks):
    """One job on the padded grid of 32-row by 16-channel workgroups."""
    padded = math.ceil(rows / TILE_ROWS) * TILE_ROWS * math.ceil(n / TILE_CHANNELS) * TILE_CHANNELS
    return FIRMWARE_US * 1e-6 + padded * k / clocks.gpu_rate


def call_seconds(rows, k, n_all, n, clocks, block=False, totals=None):
    """One call on the recognizer core (A), the second inference worker (B)
    and the GPU. A cleans each job's codes and submits it, then both cores
    compute half of the CPU channels; A then invalidates and reads out each
    job's sums in order as it completes."""
    hz = clocks.hz
    cpu_half = (n_all - n) * rows * k / 2 / clocks.cpu_rate
    a = gpu = 0.0
    done = []
    for r in (split(rows, k, n, block) if n else []):
        clean = math.ceil(r * 2 * k / 64)
        a += clean * LINE_CYCLES / hz + SUBMIT_US * 1e-6
        gpu = max(gpu, a) + gpu_seconds(r, k, n, clocks)
        done.append((gpu, r))
        if totals is not None:
            totals['jobs'] = totals.get('jobs', 0) + 1
            totals['clean_lines'] = totals.get('clean_lines', 0) + clean
            totals['gpu_macs'] = totals.get('gpu_macs', 0) + r * k * n
            totals['padded_rows'] = totals.get('padded_rows', 0) + (-r) % TILE_ROWS
    b = a + DISPATCH_US * 1e-6 + cpu_half
    a += cpu_half
    for finish, r in done:
        invalidate = math.ceil(r * n * width(k) / 64)
        a = max(a, finish + WAKE_US * 1e-6) + invalidate * LINE_CYCLES / hz + \
            r * n * READOUT_INSTRUCTIONS / (clocks.ipc * hz)
    return max(a, b)


def choose(rows, k, n_all, clocks, block=False):
    """n_gpu with the shortest modeled call, among N and N minus multiples of
    16 (NEON computes 16 channels at a time); ties go to more CPU channels."""
    if job_rows(k, n_all) == 0:
        return 0
    candidates = [n for n in range(n_all, 0, -TILE_CHANNELS) if job_rows(k, n) > 0]
    return min(candidates, key=lambda n: (round(call_seconds(rows, k, n_all, n, clocks, block), 12), n))


def table(tensors, clocks, frames):
    rows = []
    for t in tensors:
        reached = calls(t['name'], frames)
        if not reached or not eligible(t):
            rows.append((0, 0))
            continue
        n = choose(max(reached), t['k'], t['n'], clocks, t['name'] == SUBSAMPLING)
        rows.append((n, job_rows(t['k'], n) if n else 0))
    return rows


def call_list(tensors, frames):
    """(ordinal, rows, K, N, residual, block) for every linear call, in model
    order; block marks a subsampling producer block."""
    out = []
    for i, t in enumerate(tensors):
        reached = calls(t['name'], frames)
        if reached and eligible(t):
            out += [(i, r, t['k'], t['n'], int(residual(t['name'])), int(t['name'] == SUBSAMPLING))
                    for r in reached]
    return out


def summary(tensors, placement, clocks, frames):
    totals, seconds, macs = {}, 0.0, 0
    for ordinal, rows, k, n_all, _, block in call_list(tensors, frames):
        n = placement[ordinal][0]
        seconds += call_seconds(rows, k, n_all, n, clocks, block, totals)
        macs += rows * k * n_all
    shapes = {}
    for t, (n, j) in zip(tensors, placement):
        if n:
            shapes['%dx%d' % (t['k'], t['n'])] = {'n_gpu': n, 'job_rows': j}
    return {'cores': clocks.cores, 'cpu_mhz': clocks.hz / 1e6, 'frames': frames,
            'linear_seconds': seconds, 'gpu_share': totals.get('gpu_macs', 0) / macs, 'macs': macs,
            'placed_calls': sum(1 for c in call_list(tensors, frames) if placement[c[0]][0]),
            **totals, 'shapes': shapes}


def header(tensors, placement, clocks, frames):
    lines = ['/* Generated by apps/os/placement.py for the %s pair at %.0f MHz, GPU at %.0f G MAC/s; */'
             % (clocks.cores.upper(), clocks.hz / 1e6, clocks.gpu_rate / 1e9),
             '/* %d encoder frames per placement decision. Do not edit. */' % frames,
             '#define OS_PLACEMENT_TENSORS %du' % len(tensors),
             '_Static_assert(OS_RECOGNITION_CORE==%du,"table generated for another core pair");'
             % CORES[clocks.cores]['pair'][0],
             'const struct os_placement os_placement_table[OS_PLACEMENT_TENSORS]={']
    for i in range(0, len(placement), 8):
        lines.append('    ' + ''.join('{%d,%d},' % row for row in placement[i:i + 8]))
    lines.append('};')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--models', type=Path, nargs=2, required=True, metavar=('BASE', 'OVERLAY'))
    parser.add_argument('--cores', choices=sorted(CORES), default='a55', help='recognition core pair')
    parser.add_argument('--cpu-mhz', type=float, help='CPU clock (default 494 for the A55, 2002 for the A76)')
    parser.add_argument('--gpu-mhz', type=float, default=400.0)
    parser.add_argument('--cpu-rate', type=float, help='MACs per second per core (default: profile seed)')
    parser.add_argument('--gpu-rate', type=float, help='GPU MACs per second (default: clock seed)')
    parser.add_argument('--frames', type=int, default=222, help='encoder frames (default: bench recording)')
    parser.add_argument('--output', type=Path, help='write the C table')
    parser.add_argument('--calls', type=int, metavar='FRAMES', help='print the call list for FRAMES frames')
    parser.add_argument('--summary', action='store_true', help='print the time model summary as JSON')
    args = parser.parse_args()
    clocks = Clocks(args.cores, args.cpu_mhz, args.gpu_mhz, args.cpu_rate, args.gpu_rate)
    tensors = merged_tensors(*args.models)
    placement = table(tensors, clocks, args.frames)
    if args.output:
        args.output.write_text(header(tensors, placement, clocks, args.frames))
    if args.calls:
        for c in call_list(tensors, args.calls):
            print(*c)
    if args.summary:
        print(json.dumps(summary(tensors, placement, clocks, args.frames)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
