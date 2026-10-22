#!/usr/bin/env python3
"""Compute the NPU runtime's start, reply and load bounds per image and refuse a short collect timeout.

The runtime (drivers/npu/a733_npu_runtime.c, design docs/designs/npu-breadth.md)
gives each image a fixed slot count and fixed regions, so its worst cases follow
from the catalogue models alone. For every image that uses the runtime this
check computes, in microseconds at Boot0's 494 MHz core 0 clock:

  wait  <= (S - 1) * (G + max(D, W)) + P + G + L              before a job starts
  reply <= wait + max(D, H + L) + P + G + H + 2L               for a held SUBMIT reply
  load  <= n * (G + max(D, W))                                  for a LOAD of n steps

S is the slot count and D the image's job deadline cap (the SUBMIT timeout).
G = T + A is the pre-trigger block, P the job and pool preparation, H the
post-job check and W = P + H + L the worker's bulk work per job. G, P and H are
the largest over the catalogue, because any slot can load any model. L is the
longest non-yielding step other than G: C = 100 us by the step sizes, or the
LOAD step that opens and plans the staged copy, whichever is longer. Worker
terms take a factor of 1.25 where the USB0 loop shares core 0. An image fails
when its client collect timeout is shorter than its reply bound.

Derivation of each term, from the bytes the runtime touches and the design's
estimated worst-case rates (cycles at 494 MHz, which board timing replaces):

  T  every NPU table word: 4096 + 16384 per 16 MiB of device addresses,
     streamed at 64 cycles per 64-byte line; run before every trigger and after
     every completion, so twice per job.
  A  CRC32C of the NN descriptors and the command stream, and a word compare of
     the END pairs from the stream end to the 524,280-byte fetch end, both
     streamed at 64 cycles per line.
  P  clean the input lines and, in the interim, clean and invalidate the output
     lines (96 cycles per line each); zero the working pool with DC ZVA (16
     cycles per line) and clean and invalidate it (96 cycles per line).
  H  CRC32C of the slot's read-only bytes (descriptors, coefficients and their
     padding, and the stream) at 64 cycles per line. Coefficient bytes are
     bounded by the model file size here; the emulator check counts the actual
     bytes.
  L  C = 100 us, or the LOAD step that decodes and plans the staged copy:
     58,000 instructions for ShuffleNetV2 in an instrumented QEMU count, taken
     at 2 cycles per instruction, 0.235 ms.
  n  LOAD steps: one per 1 KiB copied and hashed (portable SHA-256), one to open
     and plan, one per 8 operations assembled, one per 16 KiB of END pairs up to
     the END mark (at most the whole fetch span), one for padding, one per 16 KiB
     of coefficients, one per 32 KiB of reference CRC and one per 16 KiB cleaned.

Counters that measure these terms in the emulator, printed by
checks/npu-session-runtime.c through checks/npu-session.py --mode runtime on its
COUNT lines and compared here with --evidence: requests, replies, translations
and SHA-256 bytes per inference; table_bytes (T), address_bytes and
end_tail_bytes (A), readonly_bytes (H), zeroed_bytes (pool) and cache_lines
(inputs, outputs and pool); load, prepare, pool, start and check step counts.

Images: the emulator check image, whose constants this check reads from its
source, and the planned first consumer, apps/npu-runtime-large with one
ShuffleNetV2 slot and USB0 on core 0, whose deadline cap and host request
deadline it reads from apps/npu-runtime/main.c and apps/npu-runtime-large/host.py.
"""
import argparse
import json
import math
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402  (assertion policy)

ROOT = Path(__file__).resolve().parents[1]
MHZ = 494
LINE = 64
FETCH = 524280
COMMAND = 524288
STEP_US = 100.0
OPEN_US = 2 * 58000 / MHZ
STREAM, MAINTAIN, ZERO, COPY, SHA_BYTE = 64, 96, 16, 128, 25
DEVICE_BASE, PAGE, GROUP = 0x10000000, 4096, 2 << 20


def up(value, unit):
    return (value + unit - 1) // unit * unit


def us(cycles):
    return cycles / MHZ


def lines(byte_count):
    return up(byte_count, LINE) // LINE


def model_figures(entry):
    """Byte counts of one catalogue model in the runtime's slot layout."""
    nn = entry['nn_operations'] * 192
    stream = entry['command_body_bytes'] + 56
    coefficients = entry['model']['bytes']   # Upper bound: coefficients are a section of the file.
    inputs = [t['logical_bytes'] for t in entry['inputs']]
    outputs = [t['logical_bytes'] for t in entry['outputs']]
    tensor = [up(up(b, 64) + 64, PAGE) for b in inputs + outputs]
    return dict(
        name=entry['name'], model_bytes=entry['model']['bytes'], nn=nn, stream=stream,
        constant=up(nn, PAGE) + up(coefficients + 128, PAGE),
        read=sum(tensor[:len(inputs)]), pool=up(entry['pool_bytes'], PAGE) if entry['pool_bytes'] else 0,
        write=(up(entry['pool_bytes'], PAGE) if entry['pool_bytes'] else 0) + sum(tensor[len(inputs):]),
        inputs=inputs, outputs=outputs, operations=entry['nn_operations'],
        readonly=up(nn, PAGE) + up(coefficients + 128, PAGE) + stream)


def table_bytes(slots, region):
    """NPU table arena: one leaf table per 16 MiB of device addresses."""
    device = DEVICE_BASE + COMMAND + PAGE
    device += slots * (COMMAND + PAGE + region['constant'] + PAGE)
    device += slots * (region['read'] + PAGE) + slots * (region['write'] + PAGE)
    roots = ((device - PAGE - 1) >> 24) - (DEVICE_BASE >> 24) + 1
    return PAGE + roots * 16384


def terms(m, table):
    t = us(lines(table) * STREAM)
    a = us(lines(m['nn'] + m['stream']) * STREAM + lines(FETCH - m['stream']) * STREAM)
    p = us(sum(lines(b) for b in m['inputs']) * MAINTAIN + sum(lines(b) for b in m['outputs']) * MAINTAIN +
           lines(m['pool']) * (ZERO + MAINTAIN))
    h = us(lines(m['readonly']) * STREAM)
    steps = (math.ceil(m['model_bytes'] / 1024) + 1 + math.ceil(m['operations'] / 8) + FETCH // 16384 + 1 +
             math.ceil(m['model_bytes'] / 16384) + math.ceil(m['readonly'] / 32768) +
             math.ceil((m['constant'] + COMMAND) / 16384))
    idle_load = us(m['model_bytes'] * SHA_BYTE + lines(m['model_bytes']) * COPY * 2 +
                   lines(m['readonly']) * STREAM + lines(m['constant'] + COMMAND) * MAINTAIN)
    return dict(T=t, A=a, G=t + a, P=p, H=h, load_steps=steps, idle_load_us=idle_load)


def bounds(image, catalogue):
    models = [model_figures(catalogue[name]) for name in image['models']]
    region = {k: max(m[k] for m in models) for k in ('constant', 'read', 'write')}
    table = table_bytes(image['slots'], region)
    per = {m['name']: terms(m, table) for m in models}
    factor = 1.25 if image['usb0_core0'] else 1.0
    g = factor * max(v['G'] for v in per.values())
    p = factor * max(v['P'] for v in per.values())
    h = factor * max(v['H'] for v in per.values())
    step = factor * max(STEP_US, OPEN_US)
    w = p + h + step
    d = image['deadline_us']
    s = image['slots']
    wait = (s - 1) * (g + max(d, w)) + p + g + step
    reply = wait + max(d, h + step) + p + g + h + 2 * step
    loads = {name: v['load_steps'] * (g + max(d, w)) for name, v in per.items()}
    return dict(image=image['name'], slots=s, deadline_us=d, usb0_core0=image['usb0_core0'],
                factor=factor, table_bytes=table, regions=region, G_us=g, P_us=p, H_us=h, L_us=step, W_us=w,
                start_wait_us=wait, reply_us=reply, collect_timeout_us=image['collect_us'],
                load_us=loads, per_model={k: {kk: round(vv, 3) for kk, vv in v.items()} for k, v in per.items()},
                result='pass' if image['collect_us'] >= reply else 'fail')


def constant(path, pattern):
    text = (ROOT / path).read_text()
    found = re.findall(pattern, text)
    assert found, f'{path} no longer states {pattern}'
    return found


def images():
    check = 'checks/npu-session-runtime.c'
    slots = int(constant(check, r'#define SLOTS (\d+)u')[0])
    collect = int(constant(check, r'npu_runtime_collect\(&rt, slot, ticket, &reply, (\d+)\)')[0])
    deadline = int(constant(check, r'call\(slot, NPU_SERVICE_SUBMIT, 0, (\d+)\)')[0])
    large_cap = max(int(v) for v in constant('apps/npu-runtime/main.c', r'if\(timeout>(\d+)\)timeout='))
    large_request = int(constant('apps/npu-runtime-large/host.py', r'exchange\(BEGIN,rid,timeout_us=(\d+)')[0])
    return [
        dict(name=check, slots=slots, deadline_us=deadline, usb0_core0=False, collect_us=collect * 1000,
             models=['conv8', 'dual-u8', 'dual-i8', 'shufflenet-v2-published', 'mimus-linear-q']),
        dict(name='apps/npu-runtime-large (planned first consumer)', slots=1, deadline_us=large_cap,
             usb0_core0=True, collect_us=large_request, models=['shufflenet-v2-published']),
    ]


def compare(evidence, catalogue):
    """Per-inference counts from the emulator against the byte counts above."""
    record = json.loads(evidence.read_text())
    assert record['mode'] == 'runtime' and record['result'] == 'pass', 'evidence is not a passing runtime run'
    counts = [c for case in record['cases'] for c in case.get('counts', []) if c.get('model') == 3]
    assert counts, 'evidence has no ShuffleNetV2 COUNT line'
    c = counts[0]
    jobs = c['jobs']
    m = model_figures(catalogue['shufflenet-v2-published'])
    table = bounds(images()[0], catalogue)['table_bytes']
    expected = dict(requests=jobs, replies=jobs, translations=0, sha256_bytes=0,
                    table_bytes=jobs * 2 * table, address_bytes=jobs * (m['nn'] + m['stream']),
                    end_tail_bytes=jobs * (FETCH - m['stream']), zeroed_bytes=jobs * m['pool'],
                    cache_lines=jobs * (sum(lines(b) for b in m['inputs']) + 2 * sum(lines(b) for b in m['outputs'])
                                        + lines(m['pool'])))
    for key, value in expected.items():
        assert c[key] == value, f'{key}: counted {c[key]}, computed {value}'
    assert c['readonly_bytes'] <= jobs * m['readonly'], 'read-only bytes exceed the bound used for H'
    return dict(evidence=str(evidence), counted=c, computed=expected)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--output', type=Path, required=True, help='new evidence directory')
    ap.add_argument('--evidence', type=Path, help='result.json of checks/npu-session.py --mode runtime')
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    catalogue = {m['name']: m for m in json.loads((ROOT / 'tools/npu-session-models.json').read_text())['models']}
    results = [bounds(image, catalogue) for image in images()]
    # Negative case: an image whose collect timeout is one microsecond short.
    short = dict(images()[1]); short['name'] += ' with a short collect timeout'
    short['collect_us'] = math.ceil(bounds(short, catalogue)['reply_us']) - 1
    refused = bounds(short, catalogue)
    assert refused['result'] == 'fail', 'a collect timeout shorter than the reply bound was accepted'
    record = dict(rates=dict(mhz=MHZ, stream=STREAM, maintain=MAINTAIN, zero=ZERO, copy=COPY, sha256_byte=SHA_BYTE),
                  images=results, refused=refused['image'],
                  comparison=compare(args.evidence, catalogue) if args.evidence else None)
    harness.save(args.output / 'result.json', record)
    for r in results:
        print(f"{r['image']}: wait {r['start_wait_us'] / 1000:.2f} ms, reply {r['reply_us'] / 1000:.2f} ms, "
              f"collect {r['collect_timeout_us'] / 1000:.0f} ms, G {r['G_us'] / 1000:.2f} ms, "
              f"P {r['P_us'] / 1000:.2f} ms, H {r['H_us'] / 1000:.2f} ms, W {r['W_us'] / 1000:.2f} ms: {r['result']}")
    assert all(r['result'] == 'pass' for r in results), 'an image collects for less than its reply bound'


if __name__ == '__main__':
    main()
