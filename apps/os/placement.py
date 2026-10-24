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
the CPU and GPU clocks and their MAC rates. job_rows is the GPU worker's own
bound for the tensor (kernel_job_rows) rounded down to a multiple of 32, so
every job the hook issues passes the worker's rows <= job_rows check. Every
other tensor gets n_gpu = 0 and stays on the CPU.

  placement.py --models BASE OVERLAY --cores a55 --output placement.h
  placement.py --models BASE OVERLAY --cores a55 --calls 222   # call list
  placement.py --models BASE OVERLAY --cores a55 --summary     # time model
  placement.py ... --summary --load-cycles 200                  # latency-bound GPU

The header also lists, for every row with n_gpu > 0, the weights' model file
(region 0 for the base file, 1 for the overlay), the byte offset of weight row
0 in that file, the row stride and K, which OPEN admits once per tensor. The
GPU maps each file at the start of its own 8 MiB region.

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

# Kernel limits from the GPU kernel design: one output region of 2 MiB, a
# 64 KiB tag area (one word per lane) followed by the sums.
MAX_JOB_MACS = 1 << 26
SUM_BYTES = (2 << 20) - (64 << 10)
MAX_WORKGROUPS = (64 << 10) // 4 // 32
TILE_ROWS, TILE_CHANNELS, LANES = 32, 16, 32
JOB_WORD_LINES = 2        # 128-byte argument record and CDM block
SUBSAMPLING_BLOCK = 224   # rows per subsampling producer block
OUT_CHUNK = 32            # mimus kOutChunk: subsampling output rows per call
SUBSAMPLING = 'encoder.pre_encode.conv.2.weight'

# Time model seeds; board measurements replace them. The GPU and read-out
# rates are the measured counts of docs/designs/gpu-mimus-compute-measurements.md:
# 217 USC instruction groups per 64 MACs, and 37.6 M read-out instructions for
# the bench recording's 12,923,134 GPU outputs.
USC_INSTRUCTIONS_PER_MAC = 217 / 64   # inner loop of the GPU kernel, 3.39
A55_MAC_PER_CYCLE = 4167910560 / 14.894 / 494e6   # NEON profile, one A55
A76_MAC_FACTOR = 2.0              # A76 NEON MACs per clock against an A55
READOUT_INSTRUCTIONS = 37.6e6 / 12923134   # per GPU output, 2.9: conversion and scaling
LINE_CYCLES = 20.0                # one DC CVAC or DC CIVAC line
SUBMIT_US, FIRMWARE_US, WAKE_US, DISPATCH_US = 25.0, 25.0, 25.0, 10.0
# Optional latency-bound GPU term (checks/gpu-mimus-time-model.py): a pass of
# 4 K steps issues 64 x USC_INSTRUCTIONS_PER_MAC groups with 12 waited loads,
# a lane's last pass adds 32 waited sum stores, and a workgroup's chain hides
# behind at most RESIDENT_TASKS others, the tasks the program's 106 temps
# leave resident. 0 load cycles keeps the issue-bound default.
GPU_HZ = 400e6
LOADS_PER_PASS, FENCED_STORES, RESIDENT_TASKS = 12, 32, 8
JOB_POLL_S = 4e-3                 # PVR_GPU_SERVICE_JOB_POLL_US: worker wait bound per job
REGION_BYTES = 8 << 20            # one model file per GPU region
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
                        'act': e[2], 'n': e[5], 'k': e[6], 'k_padded': e[9], 'row_stride': e[10],
                        'data_offset': e[11], 'data_size': e[12], 'file_bytes': len(data)})
    return flags, tensors


def merged_tensors(base, overlay):
    """Tensors in Model::Load order; the index is the ordinal."""
    models = json.loads(MANIFEST.read_text())['models']
    for path, model in zip((base, overlay), models):
        assert hashlib.sha256(path.read_bytes()).hexdigest() == model['sha256'], \
            '%s differs from the model the image embeds' % path
    base_flags, merged = read_model(base)
    overlay_flags, additions = read_model(overlay)
    for region, group in enumerate((merged, additions)):
        for t in group:
            t['region'] = region
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


def kernel_job_rows(k, n):
    """The worker's bound at OPEN: the 64 Mi MAC cap, the sum area and the
    512 workgroups whose tags fill the tag area."""
    return min(MAX_JOB_MACS // (k * n), SUM_BYTES // (n * width(k)),
               TILE_ROWS * (MAX_WORKGROUPS // math.ceil(n / TILE_CHANNELS)))


def job_rows(k, n):
    return kernel_job_rows(k, n) // TILE_ROWS * TILE_ROWS


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


def split(rows, k, n):
    """Jobs of one call as the GPU hook issues them: one job, or consecutive
    jobs of job_rows rows (a multiple of 32) and a last job with the rest."""
    limit = job_rows(k, n)
    return [limit] * (rows // limit) + ([rows % limit] if rows % limit else [])


class Clocks:
    def __init__(self, cores='a55', cpu_mhz=None, gpu_mhz=400.0, cpu_rate=None, gpu_rate=None,
                 load_cycles=0.0):
        c = CORES[cores]
        self.cores, self.hz, self.ipc = cores, (cpu_mhz or c['mhz']) * 1e6, c['ipc']
        self.cpu_rate = cpu_rate or A55_MAC_PER_CYCLE * c['factor'] * self.hz
        self.gpu_hz = gpu_mhz * 1e6
        self.gpu_rate = gpu_rate or self.gpu_hz * LANES / USC_INSTRUCTIONS_PER_MAC
        self.load_cycles = load_cycles


def gpu_seconds(rows, k, n, clocks):
    """One job on the padded grid of 32-row by 16-channel workgroups:
    issue-bound, or with load_cycles the larger of issue and each
    workgroup's latency chain shared among the resident tasks."""
    padded = math.ceil(rows / TILE_ROWS) * TILE_ROWS * math.ceil(n / TILE_CHANNELS) * TILE_CHANNELS
    issue = padded * k / clocks.gpu_rate
    if not clocks.load_cycles:
        return FIRMWARE_US * 1e-6 + issue
    groups = padded // (TILE_ROWS * TILE_CHANNELS)
    passes = k / 4
    chain = (passes * (64 * USC_INSTRUCTIONS_PER_MAC + LOADS_PER_PASS * clocks.load_cycles) +
             FENCED_STORES * clocks.load_cycles) / clocks.gpu_hz
    return FIRMWARE_US * 1e-6 + max(issue, groups * chain / min(RESIDENT_TASKS, groups))


def call_seconds(rows, k, n_all, n, clocks, totals=None):
    """One call on the recognizer core (A), the second inference worker (B)
    and the GPU, with one job in flight (GPU kernel design). Per job, A
    cleans the job's codes and job words and submits it. While the first job
    runs, A and B each compute half of the CPU channels and join. After each
    completion A invalidates the job's tags and sums and reads them out: alone
    when the call splits channels, with B when the GPU has every channel. The
    next job is submitted only after that read-out, because it reuses the
    output region."""
    hz, line = clocks.hz, LINE_CYCLES / clocks.hz
    cpu_half = (n_all - n) * rows * k / 2 / clocks.cpu_rate
    if not n:
        return DISPATCH_US * 1e-6 + cpu_half
    a = 0.0
    for i, r in enumerate(split(rows, k, n)):
        clean = math.ceil(r * 2 * k / 64)
        a += (clean + JOB_WORD_LINES) * line + SUBMIT_US * 1e-6
        finish = a + gpu_seconds(r, k, n, clocks)
        if i == 0 and n < n_all:
            a += DISPATCH_US * 1e-6 + cpu_half
        tags = math.ceil(r / TILE_ROWS) * math.ceil(n / TILE_CHANNELS) * LANES
        invalidate = math.ceil((tags * 4 + r * n * width(k)) / 64)
        readout = r * n * READOUT_INSTRUCTIONS / (clocks.ipc * hz)
        a = max(a, finish + WAKE_US * 1e-6) + invalidate * line + \
            (readout if n < n_all else DISPATCH_US * 1e-6 + readout / 2)
        if totals is not None:
            totals['jobs'] = totals.get('jobs', 0) + 1
            totals.setdefault('job_seconds', []).append(gpu_seconds(r, k, n, clocks))
            totals['clean_lines'] = totals.get('clean_lines', 0) + clean
            totals['invalidate_lines'] = totals.get('invalidate_lines', 0) + invalidate
            totals['gpu_seconds'] = totals.get('gpu_seconds', 0.0) + gpu_seconds(r, k, n, clocks)
            totals['gpu_macs'] = totals.get('gpu_macs', 0) + r * k * n
            totals['padded_rows'] = totals.get('padded_rows', 0) + (-r) % TILE_ROWS
    return a


def choose(rows, k, n_all, clocks):
    """n_gpu with the shortest modeled call, among N and N minus multiples of
    16 (NEON computes 16 channels at a time); ties go to more CPU channels."""
    if job_rows(k, n_all) == 0:
        return 0
    candidates = [n for n in range(n_all, 0, -TILE_CHANNELS) if job_rows(k, n) > 0]
    return min(candidates, key=lambda n: (round(call_seconds(rows, k, n_all, n, clocks), 12), n))


def table(tensors, clocks, frames):
    rows = []
    for t in tensors:
        reached = calls(t['name'], frames)
        if not reached or not eligible(t):
            rows.append((0, 0))
            continue
        n = choose(max(reached), t['k'], t['n'], clocks)
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
    for ordinal, rows, k, n_all, _, _ in call_list(tensors, frames):
        n = placement[ordinal][0]
        seconds += call_seconds(rows, k, n_all, n, clocks, totals)
        macs += rows * k * n_all
    shapes = {}
    for t, (n, j) in zip(tensors, placement):
        if n:
            shapes['%dx%d' % (t['k'], t['n'])] = {'n_gpu': n, 'job_rows': j}
    durations = totals.pop('job_seconds', [])
    ordered = sorted(durations)
    middle = len(ordered) // 2
    median = (ordered[middle] if len(ordered) % 2 else (ordered[middle - 1] + ordered[middle]) / 2) \
        if ordered else 0.0
    # The worker wakes for the hook's message and the completion interrupt,
    # and once per JOB_POLL_S timed wait while the job runs.
    wakes = sum(2 + math.ceil(d / JOB_POLL_S) for d in durations)
    return {'cores': clocks.cores, 'cpu_mhz': clocks.hz / 1e6, 'gpu_mhz': clocks.gpu_hz / 1e6,
            'load_cycles': clocks.load_cycles, 'frames': frames,
            'linear_seconds': seconds, 'gpu_share': totals.get('gpu_macs', 0) / macs, 'macs': macs,
            'placed_calls': sum(1 for c in call_list(tensors, frames) if placement[c[0]][0]),
            **totals, 'job_median_seconds': median, 'job_max_seconds': max(durations, default=0.0),
            'job_total_seconds': sum(durations), 'worker_wakes': wakes,
            'job_durations_us': [round(d * 1e6, 3) for d in durations], 'shapes': shapes}


def gpu_tensors(tensors, placement):
    """(offset, ordinal, stride, k, n, region) for every row with n_gpu > 0:
    the admission record the runtime passes to pvr_mimus_tensor_open, with N
    for the hook's check of each call. offset is the entry's data_offset, the
    byte offset of weight row 0 in its own file."""
    out = []
    for ordinal, (t, (n, _)) in enumerate(zip(tensors, placement)):
        if not n:
            continue
        offset, stride, k = t['data_offset'], t['row_stride'], t['k']
        extent = (n - 1) * stride + k
        assert k % 4 == 0 and stride % 4 == 0 and offset % 4 == 0 and k <= stride < 65536, t['name']
        assert ordinal < 65536 and t['n'] < 65536 and t['region'] in (0, 1), t['name']
        assert extent <= t['data_size'] and offset + extent <= t['file_bytes'] and \
            offset + extent <= REGION_BYTES, '%s leaves its model region' % t['name']
        out.append((offset, ordinal, stride, k, t['n'], t['region']))
    return out


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
    admitted = gpu_tensors(tensors, placement)
    lines += ['/* Tensor admission, one entry per row with n_gpu > 0 in ordinal order:',
              ' * {offset, ordinal, stride, k, n, region}. offset is the tensor entry\'s',
              ' * data_offset, weight row 0 within its model file; region 0 is the base',
              ' * file and 1 the overlay, each mapped at the start of its own 8 MiB. */',
              '#define OS_PLACEMENT_GPU_TENSORS %du' % len(admitted),
              'const struct os_placement_tensor os_placement_tensors[OS_PLACEMENT_GPU_TENSORS]={']
    for i in range(0, len(admitted), 4):
        lines.append('    ' + ''.join('{%d,%d,%d,%d,%d,%d},' % row for row in admitted[i:i + 4]))
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
    parser.add_argument('--load-cycles', type=float, default=0.0,
                        help='GPU load and store latency in GPU clocks (default 0: issue-bound)')
    parser.add_argument('--frames', type=int, default=222, help='encoder frames (default: bench recording)')
    parser.add_argument('--output', type=Path, help='write the C table')
    parser.add_argument('--calls', type=int, metavar='FRAMES', help='print the call list for FRAMES frames')
    parser.add_argument('--summary', action='store_true', help='print the time model summary as JSON')
    args = parser.parse_args()
    clocks = Clocks(args.cores, args.cpu_mhz, args.gpu_mhz, args.cpu_rate, args.gpu_rate,
                    args.load_cycles)
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
