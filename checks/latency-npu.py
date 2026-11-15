#!/usr/bin/env python3
"""The stage 3b NPU load of the latency diagnostic (apps/latency/npu.h): its image, its model, its decoder and its judge.

- The manifest. apps/latency/manifest-npu.json pins the input regions of stage 1b's relu: the graph-relu entry of the npu-graphs
  fixture, packed by apps/accel-overlap/pack.py and tools/npu-app-image.py --no-build. The check repacks them and requires the
  same manifest, so the image's admission digests match the regions a run uploads.
- The placement. The board image (ret-latency-npu) routes INTID 97 to CPU0 and the bar image without the load
  (ret-latency-bar) keeps it on CPU1: OS_NPU_CORE reaches the generated route table only under the option.
- The owner's core. The OS builds apps/os/npu*.c in their own object library (sunstone-a733-os-npu), so the route table
  and the owner can be compiled for different cores. From each linked image's code the check reads the affinity that
  os_npu_start passes to every task it creates and the GICD_IROUTER value that os_npu_irq_bind requires for INTID 97.
  Both must name the routed core. The first board run of this image (queue entry 1794766775350069931-da297888) had the
  route on CPU0 and the owner on CPU1, and the bind refused at startup. A mutant relinks the NPU image with the
  sunstone-a733-os-npu objects compiled without OS_NPU_CORE=0u, as that run's image was, and the check must refuse it.
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
Usage: latency-npu.py --model DIR --image ELF --image-build DIR --default-image ELF --graphs DIR --output DIR
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys

from elftools.elf.elffile import ELFFile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--model', '{build:latency-npu-model}', '--image', '{elf:ret-latency-npu}', '--image-build', '{build:ret-latency-npu}',
              '--default-image', '{elf:ret-latency-bar}', '--graphs', '{fixture:npu-graphs}', '--output', '{output}']]
CHECK_INPUTS = ('apps/latency/host.py', 'apps/latency/npu-judge.py', 'apps/latency/manifest-npu.json', 'apps/latency/npu.h',
                'apps/accel-overlap/pack.py', 'tools/npu-app-image.py', 'tools/npu-external-pack.py', 'tools/npu_graph_layout.py')
TARGET = 'latency-partitioned-pmu-model'
NPU_INTID = 97
OBJDUMP = '/usr/lib/llvm-21/bin/llvm-objdump'
IROUTER_97 = 0x03400000 + 0x6000 + 8 * NPU_INTID  # GICD (platform/a733/npu_irq.c) + GICD_IROUTER<n>
NPU_LIBRARY = 'sunstone-a733-os-npu.dir/'
NPU_DEFINE = '-DOS_NPU_CORE=0u'
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


def disassemble(elf, symbol):
    done = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', '--disassemble-symbols=' + symbol, str(elf)],
                          capture_output=True, text=True, check=True)
    rows = []
    for line in done.stdout.splitlines():
        match = re.match(r'^ *([0-9a-f]+):\s+(\S+)\s*(.*?)\s*(?://.*)?$', line)
        if match:
            rows.append((int(match.group(1), 16), match.group(2), match.group(3)))
    assert rows, ('%s has no %s' % (elf, symbol))
    return rows


def immediate(text):
    return int(text.lstrip('#'), 0)


def owner_affinities(elf):
    """The affinity argument (w7) of each xTaskCreateStaticAffinitySet call in os_npu_start."""
    rows = disassemble(elf, 'os_npu_start')
    found = []
    for i, (_, op, args) in enumerate(rows):
        if op == 'bl' and args.endswith('<xTaskCreateStaticAffinitySet>'):
            value = None
            for _, back_op, back_args in reversed(rows[:i]):
                if back_op in ('bl', 'blr', 'ret', 'b'):
                    break
                parts = [a.strip() for a in back_args.split(',')]
                if parts and parts[0] in ('w7', 'x7'):
                    assert back_op == 'mov' and parts[1].startswith('#'), ('os_npu_start sets the affinity by %s %s, '
                                                                          'which the check does not resolve' % (back_op, back_args))
                    value = immediate(parts[1])
                    break
            assert value is not None, 'a task creation in os_npu_start at %#x has no resolvable affinity' % rows[i][0]
            found.append(value)
    assert len(found) >= 2, ('os_npu_start creates %d tasks; the check expects the boot and client tasks' % len(found))
    return found


def route_compare(elf):
    """The GICD_IROUTER<97> value that os_npu_irq_bind requires: the comparison after its 64-bit load of that word."""
    rows = disassemble(elf, 'os_npu_irq_bind')
    values = {}
    for i, (_, op, args) in enumerate(rows):
        parts = [a.strip() for a in args.split(',')]
        if op == 'mov' and len(parts) == 2 and parts[1].startswith('#'):
            values[parts[0]] = immediate(parts[1])
        elif op == 'movk' and parts[0] in values and len(parts) == 3 and parts[2] == 'lsl #16':
            values[parts[0]] |= immediate(parts[1]) << 16
        elif op == 'ldr' and len(parts) == 2 and parts[0].startswith('x') and re.fullmatch(r'\[x\d+\]', parts[1]):
            base = 'w' + parts[1][2:-1]
            if values.get(base) != IROUTER_97:
                continue
            loaded = parts[0]
            _, next_op, next_args = rows[i + 1]
            follow = [a.strip() for a in next_args.split(',')]
            if next_op in ('cbz', 'cbnz') and follow[0] == loaded:
                return 0
            if next_op == 'cmp' and follow[0] == loaded and follow[1].startswith('#'):
                return immediate(follow[1]) << (12 if len(follow) == 3 and follow[2] == 'lsl #12' else 0)
            raise AssertionError('os_npu_irq_bind tests GICD_IROUTER<97> by %s %s, which the check does not resolve'
                                 % (next_op, next_args))
    raise AssertionError('os_npu_irq_bind in %s does not load GICD_IROUTER<97> (%#x)' % (elf, IROUTER_97))


def placement(elf):
    core, handled = routes(elf)[NPU_INTID]
    return {'route': core, 'handler': handled, 'affinities': owner_affinities(elf), 'bind_route': route_compare(elf)}


def placement_refusal(found):
    core = found['route']
    if not found['handler']:
        return 'INTID 97 does not reach os_npu_irq'
    if any(a != 1 << core for a in found['affinities']):
        return 'os_npu_start creates the owner tasks with affinities %s, but INTID 97 is routed to CPU%d' % (
            ', '.join('%#x' % a for a in found['affinities']), core)
    if found['bind_route'] != core << 8:
        return 'os_npu_irq_bind requires GICD_IROUTER<97> = %#x, but INTID 97 is routed to CPU%d' % (found['bind_route'], core)
    return None


def mutant_without_define(build, elf, out):
    """The NPU image relinked with every sunstone-a733-os-npu object compiled without OS_NPU_CORE=0u."""
    links = [p for p in (build / 'CMakeFiles').glob('*.dir/link.txt')
             if any(' -o ' in l and shlex.split(l)[shlex.split(l).index('-o') + 1] == elf.name for l in p.read_text().splitlines())]
    assert len(links) == 1, 'no single link command writes %s' % elf.name
    link = shlex.split(next(l for l in links[0].read_text().splitlines() if ' -o ' in l))
    work = out / 'mutant-no-npu-define'
    work.mkdir()
    replaced = 0
    for entry in json.loads((build / 'compile_commands.json').read_text()):
        command = shlex.split(entry['command'])
        obj = command[command.index('-o') + 1]
        if NPU_LIBRARY not in obj:
            continue
        assert NPU_DEFINE in command, ('%s is compiled without %s; the mutant expects the fixed build' % (entry['file'], NPU_DEFINE))
        named = [i for i, a in enumerate(link) if a.endswith(obj)]
        if not named:
            continue
        assert len(named) == 1, 'the link command names %s more than once' % obj
        command = [a for a in command if a != NPU_DEFINE]
        target = work / ('%d.o' % replaced)
        command[command.index('-o') + 1] = str(target)
        done = subprocess.run(command, cwd=entry['directory'], capture_output=True, text=True)
        assert done.returncode == 0, 'the mutant %s does not compile:\n%s' % (entry['file'], done.stderr[-2000:])
        link[named[0]] = str(target)
        replaced += 1
    assert replaced >= 2, 'the NPU image links %d sunstone-a733-os-npu objects; the mutant expects apps/os/npu*.c' % replaced
    link[link.index('-o') + 1] = str(work / elf.name)
    link = [a for a in link if not a.startswith('-Wl,-Map,')]
    done = subprocess.run(link, cwd=build, capture_output=True, text=True)
    assert done.returncode == 0, 'the mutant image does not link:\n' + done.stderr[-2000:]
    return work / elf.name


def check_owner(image, build, default, out):
    found = {'npu': placement(image), 'bar': placement(default)}
    assert found['npu']['route'] == 0 and not placement_refusal(found['npu']), ('the NPU image', placement_refusal(found['npu']), found['npu'])
    assert found['bar']['route'] == 1 and not placement_refusal(found['bar']), ('the bar image', placement_refusal(found['bar']), found['bar'])
    mutant = placement(mutant_without_define(build, image, out))
    refusal = placement_refusal(mutant)
    assert refusal, ('the mutant without the sunstone-a733-os-npu define passed', mutant)
    found['mutant'] = dict(mutant, refusal=refusal)
    (out / 'placement.json').write_text(json.dumps(found, indent=1))
    print('PASS owner: os_npu_start and os_npu_irq_bind name CPU0 in the NPU image and CPU1 in the bar image; the mutant '
          'without the sunstone-a733-os-npu define is refused: ' + refusal)


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
    parser.add_argument('--image-build', type=Path, required=True, help='the build directory of --image')
    parser.add_argument('--default-image', type=Path, required=True)
    parser.add_argument('--graphs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--qemu', type=Path, default=harness.CACHE / 'host/usr/bin/qemu-system-aarch64')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    check_manifest(args.graphs.resolve(), out)
    check_routes(args.image, args.default_image)
    check_owner(args.image.resolve(), args.image_build.resolve(), args.default_image.resolve(), out)
    host = load_host()
    payload = run_model(args.qemu, args.model.resolve() / (TARGET + '.elf'), out)
    decoded = decode_model(host, payload)
    check_decoded(decoded)
    check_refusals(host, payload, out)
    check_judge(decoded, out)
    harness.save(out / 'result.json', {'passed': True})


if __name__ == '__main__':
    main()
