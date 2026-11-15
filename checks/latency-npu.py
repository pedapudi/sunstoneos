#!/usr/bin/env python3
"""The stage 3b NPU load of the latency diagnostic (apps/latency/npu.h): its image, its model, its decoder and its judge.

- The manifest. apps/latency/manifest-npu.json pins the input regions of stage 1b's relu: the graph-relu entry of the npu-graphs
  fixture, packed by apps/accel-overlap/pack.py and tools/npu-app-image.py --no-build. The check repacks them and requires the
  same manifest, so the image's admission digests match the regions a run uploads.
- The placement. The board image (ret-latency-npu) routes INTID 97 to CPU0 and the bar image without the load
  (ret-latency-bar) keeps it on CPU1: OS_NPU_CORE reaches the generated route table only under the option.
- The model. latency-npu-model runs the load against the stand-in device, whose jobs complete on CPU0's virtual timer
  interrupt, on QEMU's multi-threaded TCG, as checks/latency-cause-model.py runs the cause model and for the same reason: the
  cause ping is a handshake between cores that run in parallel. Its kill bound is that check's measured bound, for the same
  target and run. The model must print its NPU and latency PASS lines; it then prints its whole result, which the board
  decoder (apps/latency/host.py) must decode with every section, the NPU section and the per-sub-phase wakes included. QEMU
  implements none of the cause profile's cache refill events, so the cause section's event decode is replaced, after the
  check has seen it refuse exactly that; and the header's model word is cleared, since the decoder admits board results only.
  The result is functional evidence only: QEMU's times are not the board's.
- Decoder refusals. A job counted in an off sub-phase, a busy run that drained after the next off sub-phase began, a stream
  sweep in the NPU-alone window, and a last output that differed from the reference must each be refused.
- The judge. apps/latency/npu-judge.py must give a verdict for cores 1-3 from the decoded model result once its model flag is
  cleared, and must refuse it while the flag is set.
Usage: latency-npu.py --model DIR --image ELF --default-image ELF --graphs DIR --output DIR
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

from elftools.elf.elffile import ELFFile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--model', '{build:latency-npu-model}', '--image', '{elf:ret-latency-npu}', '--default-image', '{elf:ret-latency-bar}',
              '--graphs', '{fixture:npu-graphs}', '--output', '{output}']]
CHECK_INPUTS = ('apps/latency/host.py', 'apps/latency/npu-judge.py', 'apps/latency/manifest-npu.json', 'apps/latency/npu.h',
                'apps/accel-overlap/pack.py', 'tools/npu-app-image.py', 'tools/npu-external-pack.py', 'tools/npu_graph_layout.py')
TARGET = 'latency-partitioned-pmu-model'
NPU_INTID = 97
WINDOW, RUN, HEADER = 176, 64, 96  # struct latency_npu_window, latency_npu_run and the report header (apps/latency/npu.h)


def load_host():
    spec = importlib.util.spec_from_file_location('latency_host', ROOT / 'apps/latency/host.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    return host


def check_manifest(graphs, out):
    fixture = json.loads((graphs / 'artifacts.json').read_text())
    work = out / 'graphs'
    work.mkdir()
    (work / 'artifacts.json').write_text(json.dumps({'graph-relu': fixture['graph-relu']}))
    (work / 'list.json').write_text(json.dumps({'npu_models': ['graph-relu']}))
    vectors = out / 'vectors.region'
    subprocess.run([sys.executable, '-B', str(ROOT / 'apps/accel-overlap/pack.py'), '--graphs', str(work), '--output', str(vectors)],
                   check=True, capture_output=True)
    subprocess.run([sys.executable, '-B', str(ROOT / 'tools/npu-app-image.py'), '--app', str(ROOT / 'apps/latency'),
                    '--manifest', 'manifest-npu.json', '--graphs', str(work), '--data', str(vectors), '--out', str(out / 'pin'),
                    '--queue-depth', '8', '--manifest-variable', 'SUNSTONE_LATENCY_NPU_MANIFEST', '--no-build'],
                   check=True, capture_output=True)
    rebuilt = json.loads((out / 'pin/manifest.json').read_text())
    pinned = json.loads((ROOT / 'apps/latency/manifest-npu.json').read_text())
    assert rebuilt == pinned, ('apps/latency/manifest-npu.json differs from the regions the npu-graphs fixture packs to', out / 'pin')
    print('PASS manifest: the pinned NPU declaration and region digests are those of the repacked graph-relu regions')


def routes(elf_path):
    with open(elf_path, 'rb') as f:
        elf = ELFFile(f)
        table = elf.get_section_by_name('.symtab')
        symbols = {s.name: s for s in table.iter_symbols() if s.name in ('a733_irq_routes', 'os_npu_irq')}
        route = symbols['a733_irq_routes']
        address, size = route['st_value'], route['st_size']
        for segment in elf.iter_segments():
            if segment['p_type'] == 'PT_LOAD' and segment['p_vaddr'] <= address < segment['p_vaddr'] + segment['p_filesz']:
                data = segment.data()[address - segment['p_vaddr']:address - segment['p_vaddr'] + size]
                break
        else:
            raise AssertionError('the route table is not in a loaded segment of %s' % elf_path)
        handler = symbols['os_npu_irq']['st_value']
    rows = [struct.unpack_from('<HBB4xQ', data, at) for at in range(0, size, 16)]
    return {intid: (core, entry == handler) for intid, core, _, entry in rows}


def check_routes(image, default):
    npu = routes(image).get(NPU_INTID)
    assert npu == (0, True), ('the NPU image routes INTID 97 elsewhere than os_npu_irq on CPU0', npu)
    bar = routes(default).get(NPU_INTID)
    assert bar == (1, True), ('the bar image without the load moved INTID 97 off CPU1', bar)
    print('PASS placement: INTID 97 reaches os_npu_irq on CPU0 in the NPU image and on CPU1 in the bar image')


def run_model(qemu, image, out):
    command = [str(qemu), '-machine', 'virt,gic-version=3,secure=off,virtualization=off', '-accel', 'tcg,thread=multi', '-cpu', 'cortex-a55',
               '-smp', '8', '-m', '128M', '-nographic', '-monitor', 'none', '-semihosting-config', 'enable=on,target=native',
               '-device', f'loader,file={image},cpu-num=0']
    done = harness.run_qemu(command, harness.qemu_kill_seconds('latency-cause-model'))
    log = done.stdout + done.stderr
    (out / 'qemu.log').write_text(log)
    assert done.returncode == 0 and 'FAIL' not in log, ('the NPU model failed', out / 'qemu.log')
    for line in ('PASS latency npu model', 'PASS latency model'):
        assert line in log, ('the NPU model did not print ' + line, out / 'qemu.log')
    size = int(re.search(r'^latency result bytes=0x([0-9a-f]+)$', log, re.M).group(1), 16)
    data = bytes.fromhex(''.join(line[2:] for line in log.splitlines() if line.startswith('R ')))
    assert len(data) == size and 'latency result end' in log, ('the model result is incomplete', len(data), size)
    payload = bytearray(data)
    struct.pack_into('<I', payload, 28, 0)  # the header's model word (struct latency_result)
    print('PASS model: the stand-in NPU ran alone and in the on sub-phases, and the model printed its %d-byte result' % size)
    return bytes(payload)


def decode_model(host, payload):
    try:
        host.decode(payload)
    except ValueError as error:
        assert 'does not implement' in str(error) and 'cause core' in str(error), ('the model result was refused for a reason other '
                                                                                   'than QEMU\'s missing cache refill events', error)
    else:
        raise AssertionError('QEMU implements the cause events; decode the model result without replacing their decode')
    real_cause, real_ping = host.decode_latency_cause, host.decode_latency_cause_ping
    host.decode_latency_cause = lambda *args, **kwargs: {'tick': {'effective_mhz': 1}}
    host.decode_latency_cause_ping = lambda *args, **kwargs: {}
    try:
        return host.decode(payload)
    finally:
        host.decode_latency_cause, host.decode_latency_cause_ping = real_cause, real_ping


def check_decoded(decoded):
    npu = decoded['npu']
    assert npu['model'] and npu['core'] == 0 and npu['on'] == [False, True, True, False], npu['on']
    assert npu['alone']['jobs'] and all(npu['subphases'][k]['jobs'] for k in (1, 2)), 'the decoded NPU windows ran no jobs'
    assert not any(npu['subphases'][k]['jobs'] for k in (0, 3)), 'the decoded off sub-phases ran jobs'
    assert len(npu['wakes']) == 8 and all(len(rows) == 4 and not any(r['early'] for r in rows) for rows in npu['wakes'])
    assert 'scheduler_trace' in decoded and 'wake_segments' in decoded
    print('PASS decode: the board decoder took the model result with its scheduler trace, wake segments and NPU section; '
          'loss %.2f in the model (QEMU times)' % npu['loss'])


def check_refusals(host, payload, out):
    base = struct.unpack_from('<Q', payload, len(payload) - 64 + 24)[0]  # the NPU footer's section offset

    def window(k):
        return base + HEADER + WINDOW * (1 + k)

    def run(r):
        return base + HEADER + WINDOW * 5 + RUN * r
    alone = base + HEADER
    sub3 = window(3)
    cases = (
        ('job in an off sub-phase', lambda p: struct.pack_into('<Q', p, sub3 + 24, struct.unpack_from('<Q', p, sub3 + 24)[0] + 1),
         'NPU off sub-phase 3 ran jobs'),
        ('busy run drained in the next off sub-phase', lambda p: struct.pack_into('<Q', p, run(1) + 24, struct.unpack_from('<Q', p, sub3)[0] + 1),
         'the busy run is not enclosed'),
        ('stream sweep in the NPU-alone window', lambda p: struct.pack_into('<Q', p, alone + 96 + 4 * 8, struct.unpack_from('<Q', p, alone + 32 + 4 * 8)[0] + 1),
         'the NPU-alone window ran with the streams'),
        ('last output differs', lambda p: struct.pack_into('<I', p, run(0) + 60, 1), 'or it differed'),
    )
    results = []
    for name, mutate, expected in cases:
        mutated = bytearray(payload)
        mutate(mutated)
        try:
            decode_model(host, bytes(mutated))
        except (ValueError, AssertionError) as error:
            message = str(error)
        else:
            message = ''
        results.append({'case': name, 'refusal': message})
        assert expected in message, ('the decoder did not refuse: ' + name, message)
    (out / 'refusals.json').write_text(json.dumps(results, indent=1))
    print('PASS refusals: %s' % '; '.join(r['case'] for r in results))


def check_judge(decoded, out):
    judge = ROOT / 'apps/latency/npu-judge.py'
    model = out / 'model-run'
    model.mkdir()
    (model / 'result.json').write_text(json.dumps({'decoded': decoded}))
    done = subprocess.run([sys.executable, '-B', str(judge), str(model / 'result.json')], capture_output=True, text=True)
    assert done.returncode and 'QEMU model result' in done.stderr + done.stdout, ('the judge took a model result', done.stdout[-400:])
    board = out / 'judge-run'
    board.mkdir()
    flagged = json.loads(json.dumps(decoded))
    flagged['npu']['model'] = False
    (board / 'result.json').write_text(json.dumps({'decoded': flagged}))
    done = subprocess.run([sys.executable, '-B', str(judge), str(board / 'result.json')], capture_output=True, text=True)
    (out / 'judge.txt').write_text(done.stdout + done.stderr)
    assert done.returncode == 0, ('the judge failed on the decoded model result', out / 'judge.txt')
    for core in (1, 2, 3):
        assert re.search(r'^CPU%d +[-+]\d+\.\d+ \([^)]*\) +(under 0\.5|between|at least 2) ' % core, done.stdout, re.M), (
            'the judge gave no verdict for CPU%d' % core, out / 'judge.txt')
    assert 'CPU0 busy-wake maximum' in done.stdout and 'NPU loss under the streams' in done.stdout
    print('PASS judge: a verdict for cores 1-3, CPU0\'s placement cost and the NPU loss; a model result is refused')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--default-image', type=Path, required=True)
    parser.add_argument('--graphs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--qemu', type=Path, default=harness.CACHE / 'host/usr/bin/qemu-system-aarch64')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    check_manifest(args.graphs.resolve(), out)
    check_routes(args.image, args.default_image)
    host = load_host()
    payload = run_model(args.qemu, args.model.resolve() / (TARGET + '.elf'), out)
    decoded = decode_model(host, payload)
    check_decoded(decoded)
    check_refusals(host, payload, out)
    check_judge(decoded, out)
    harness.save(out / 'result.json', {'passed': True})


if __name__ == '__main__':
    main()
