#!/usr/bin/env python3
"""Per-core time model of mimus linear layers with exact sums on the GPU.

The model replays each linear call of a recording on three timelines: core 6
(recognizer, GPU worker and interrupt), core 7 (the second inference worker)
and the GPU. It prints every time and count row of the budget table in
docs/designs/gpu-mimus-compute.md for three placements: GPU only with one job
per call, GPU only with each call split into two jobs, and the best channel
split per tensor shape. Each assumption is a named constant below; board
measurements replace them, and rerunning the model then sets n_gpu.

By default the model uses the 203 calls of the bench recording (8.87 s of
audio, 222 encoder frames). --calls reads another call list, one call per
line as "rows K N accumulate", as checks/gpu-mimus-layers prints it. No
hardware is accessed."""
import argparse, json, math

# GPU: one 32-lane USC instruction per clock; instructions per MAC from the
# kernel's inner loop (checks/gpu-mimus-program counts them).
USC_HZ = 400e6
LANES = 32
INSTR_PER_MAC = 1.75
FW_US = 25.0          # kick to start, end to interrupt and GPU cache flush, per job
SUBMIT_US = 25.0      # recognizer submit to kick, on core 6
WAKE_US = 25.0        # completion interrupt to recognizer wake
DISPATCH_US = 10.0    # ParallelFor hand-off of a read-out half to core 7
# CPU: an A76 core does twice an A55's NEON MACs per clock; the A55 rate is
# the profile's 4,167,910,560 MACs in 14.894 s at 494 MHz.
A55_MAC_PER_CYCLE = 4167910560 / 14.894 / 494e6
A76_MAC_FACTOR = 2.0
LINE_CYCLES = 20.0    # one DC CVAC or DC IVAC line
READOUT_IPC = 2.0     # read-out instructions retired per A76 clock
READOUT_INSTR = {4: 3.0, 8: 4.0}  # per output, by sum width in bytes
ACCUMULATE_INSTR = 1.0            # extra per output for y += alpha * value
TAG_INSTR = 0.5                   # per tag word compared
JOB_WORD_LINES = 2                # argument record and CDM block, one per slot
TAG_AREA = 64 * 1024
REGION = 2 * 1024 * 1024
MAX_JOB_MACS = 1 << 26

BENCH = ([(240, 1584, 176, 0)] * 18 + [(120, 1584, 176, 0)] +
         [(32, 3520, 176, 0)] * 6 + [(30, 3520, 176, 0)] +
         [(222, 176, 704, 0)] * 32 + [(222, 704, 176, 1)] * 32 +
         [(222, 176, 176, 0)] * 48 + [(222, 176, 176, 1)] * 32 +
         [(443, 176, 176, 0)] * 16 + [(222, 176, 352, 0)] * 16 +
         [(222, 176, 1025, 0)])


def width(k):
    return 4 if k <= 512 else 8


def workgroups(rows, n):
    return math.ceil(rows / 32) * math.ceil(n / 16)


def job_rows(k, n):
    """Most rows in one job: the MAC cap, the sum area and the tag area.
    The worker computes the same bound at open; only boundaries between the
    jobs of one call fall on multiples of 32 rows."""
    return min(MAX_JOB_MACS // (k * n), (REGION - TAG_AREA) // (n * width(k)),
               32 * (TAG_AREA // (LANES * 4) // math.ceil(n / 16)))


def split_rows(rows, k, n, force_two):
    limit = job_rows(k, n)
    parts = math.ceil(rows / limit)
    if force_two and parts == 1 and rows > 32:
        parts = 2
    out, left = [], rows
    for i in range(parts, 0, -1):
        take = left if i == 1 else min(limit // 32 * 32, max(32, round(left / i / 32) * 32))
        out.append(take)
        left -= take
    return out


class Totals(dict):
    def add(self, key, value):
        self[key] = self.get(key, 0) + value


def job_costs(rows, k, n, acc, hz, t):
    wg = workgroups(rows, n)
    code = rows * 2 * k
    sums = rows * n * width(k)
    tags = wg * LANES
    clean = math.ceil(code / 64) + 1 + JOB_WORD_LINES
    inval = math.ceil((sums + tags * 4) / 64)
    readout = rows * n * (READOUT_INSTR[width(k)] + ACCUMULATE_INSTR * acc)
    t.add('jobs', 1); t.add('clean_lines', clean); t.add('inval_lines', inval)
    t.add('tags', tags); t.add('readout_instr', readout)
    t.add('gpu_macs', rows * k * n)
    return {
        'gpu': FW_US * 1e-6 + wg * 32 * 16 * k * INSTR_PER_MAC / (USC_HZ * LANES),
        'clean': clean * LINE_CYCLES / hz + SUBMIT_US * 1e-6,
        'inval': inval * LINE_CYCLES / hz + tags * TAG_INSTR / (READOUT_IPC * hz),
        'readout': readout / (READOUT_IPC * hz),
    }


def gpu_only(call, hz, force_two, t):
    """Every channel on the GPU. One job is in flight, or two when each call
    is forced into two jobs; each read-out runs over rows on cores 6 and 7."""
    rows, k, n, acc = call
    jobs = [job_costs(r, k, n, acc, hz, t) for r in split_rows(rows, k, n, force_two)]
    depth = 2 if force_two else 1
    core6 = core7 = gpu = 0.0
    done = []
    for job in jobs[:depth]:
        core6 += job['clean']
        gpu = max(gpu, core6) + job['gpu']
        done.append(gpu)
    for i, job in enumerate(jobs):
        start = max(core6, done[i] + WAKE_US * 1e-6) + job['inval']
        half = job['readout'] / 2
        core7 = max(core7, start + DISPATCH_US * 1e-6) + half
        core6 = max(start + half, core7)
        if i + depth < len(jobs):
            core6 += jobs[i + depth]['clean']
            gpu = max(gpu, core6) + jobs[i + depth]['gpu']
            done.append(gpu)
    return core6


def channel_split(call, n_gpu, hz, t, two_worker_readout=False):
    rows, k, n, acc = call
    cpu_half = (n - n_gpu) * rows * k / 2 / (A55_MAC_PER_CYCLE * A76_MAC_FACTOR * hz)
    jobs = [job_costs(r, k, n_gpu, acc, hz, t) for r in split_rows(rows, k, n_gpu, False)]
    core6, gpu, inval, readout = 0.0, 0.0, 0.0, 0.0
    for job in jobs:
        core6 += job['clean']
        gpu = max(gpu, core6) + job['gpu']
        inval += job['inval']
        readout += job['readout']
    join = max(core6 + cpu_half + inval, DISPATCH_US * 1e-6 + cpu_half,
               gpu + WAKE_US * 1e-6 + jobs[-1]['inval'])
    if two_worker_readout:
        return join + DISPATCH_US * 1e-6 + readout / 2
    return join + readout


def run(calls, hz):
    rows = {}
    for name, force in (('gpu_only_one_job', False), ('gpu_only_two_jobs', True)):
        t = Totals()
        rows[name] = dict(seconds=sum(gpu_only(c, hz, force, t) for c in calls), **t)
    t, t2, total, total2, gpu_macs = Totals(), Totals(), 0.0, 0.0, 0
    split_n = {}
    for c in calls:
        _, k, n, _ = c
        options = [n] + list(range(16, n - 15, 16))
        best = min(options, key=lambda m: channel_split(c, m, hz, Totals()) if m < n
                   else gpu_only(c, hz, False, Totals()))
        split_n[f'{k}x{n}'] = best
        if best == n:
            total += gpu_only(c, hz, False, t)
            total2 += gpu_only(c, hz, False, t2)
        else:
            total += channel_split(c, best, hz, t)
            total2 += channel_split(c, best, hz, t2, True)
    all_macs = sum(r * k * n for r, k, n, _ in calls)
    rows['channel_split'] = dict(seconds=total, gpu_share=t['gpu_macs'] / all_macs,
                                 n_gpu=split_n, **t)
    rows['channel_split_two_worker_readout_saving_s'] = total - total2
    rows['cpu_only_a76_pair_s'] = all_macs / (2 * A55_MAC_PER_CYCLE * A76_MAC_FACTOR * hz)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--calls', help='call list: rows K N accumulate per line')
    parser.add_argument('--mhz', type=float, action='append',
                        help='A76 clock in MHz (default 494 and 2002)')
    args = parser.parse_args()
    calls = BENCH
    if args.calls:
        with open(args.calls) as f:
            calls = [tuple(int(v) for v in line.split()) for line in f if line.strip()]
    macs = sum(r * k * n for r, k, n, _ in calls)
    outputs = {w: sum(r * n for r, k, n, _ in calls if width(k) == w) for w in (4, 8)}
    print(json.dumps({'calls': len(calls), 'macs': macs, 'int32_outputs': outputs[4],
                      'int64_outputs': outputs[8],
                      'gpu_compute_all_channels_s': sum(
                          workgroups(r, n) * 512 * k for r, k, n, _ in calls)
                      * INSTR_PER_MAC / (USC_HZ * LANES)}))
    for mhz in args.mhz or [494.0, 2002.0]:
        print(json.dumps({'mhz': mhz, **run(calls, mhz * 1e6)}, default=float))


if __name__ == '__main__':
    main()
