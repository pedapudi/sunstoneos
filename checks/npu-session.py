#!/usr/bin/env python3
"""Build and run the shared-service and model-session FreeRTOS checks in QEMU.

The configured central build supplies the npu-service-model target's compiler
flags, FreeRTOS port objects and link line; this driver compiles the changed
check and driver sources with exactly those flags and replaces only the
check object. QEMU supplies two Cortex-A55 cores and no NPU: each check models
device registers itself. A pass requires exit status 0 and the check's PASS
line for every case. Evidence goes to a new output directory.

The session and reserved modes also need --artifacts, a JSON object mapping each catalogue
model name to {"model": path, "inputs": [paths], "references": [paths]}. The
driver verifies every file against tools/npu-session-models.json and embeds
the bytes in a generated header; no compiler or SDK code runs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402  (assertion policy and shared host paths)

ROOT = Path(__file__).resolve().parents[1]
QEMU = harness.CACHE / 'host/usr/bin/qemu-system-aarch64'
CASE_ADDRESS = 0x47fff000  # Outside the ELF load interval, inside QEMU RAM.
MODES = {
    'service-shared': dict(sources=['checks/npu-service-shared.c'], marker='PASS NPU shared service:',
                           cases=[None]),
    # Case 0 polls, case 1 completes through the interrupt path and case 2
    # changes one model's coefficients after admission.
    'session': dict(sources=['checks/npu-session.c', 'drivers/npu/npu_session.c',
                             'drivers/npu/nbg_model.c', 'drivers/npu/nbg_assembly.c',
                             'drivers/npu/nbg_checksum.c', 'drivers/npu/nbg_relocation.c',
                             'drivers/storage/storage_digest.c'],
                    marker='PASS NPU sessions', cases=[0, 1, 2],
                    port_define='npu_service_task_privileged=npu_session_task_privileged',
                    wraps=['a733_npu_native_operations', 'aarch64_device_translate',
                           'aarch64_device_cache', 'storage_digest_bytes']),
    # The same check with the published ShuffleNet graph and a mimus linear
    # layer added, its parent
    # taken from the reservation constructor: case 0 runs every model and
    # case 3 offers a pool smaller than the parent.
    'reserved': dict(sources=['checks/npu-session.c', 'drivers/npu/npu_session.c',
                              'drivers/npu/nbg_model.c', 'drivers/npu/nbg_assembly.c',
                              'drivers/npu/nbg_checksum.c', 'drivers/npu/nbg_relocation.c',
                              'drivers/storage/storage_digest.c', 'platform/a733/npu_session_reserved.c',
                              'runtime/device_memory_pool.c'],
                     marker='PASS NPU sessions', cases=[0, 3], defines=['NPU_SESSION_RESERVED_CHECK=1'],
                     port_define='npu_service_task_privileged=npu_session_task_privileged',
                     wraps=['a733_npu_native_operations', 'aarch64_device_translate',
                           'aarch64_device_cache', 'storage_digest_bytes']),
    # The runtime that loads any catalogue model into fixed slots at runtime:
    # see the case list at the top of checks/npu-session-runtime.c.
    'runtime': dict(sources=['checks/npu-session-runtime.c', 'drivers/npu/a733_npu_runtime.c',
                             'drivers/npu/a733_npu_runtime_interim.c', 'drivers/npu/nbg_model.c',
                             'drivers/npu/nbg_assembly.c', 'drivers/npu/nbg_checksum.c',
                             'drivers/npu/nbg_relocation.c', 'drivers/storage/storage_digest.c'],
                    marker='PASS NPU runtime', cases=[0, 1, 2, 3, 4],
                    port_define='npu_service_task_privileged=npu_session_task_privileged',
                    wraps=['a733_npu_native_operations', 'aarch64_device_translate', 'aarch64_device_cache',
                           'storage_digest_add', 'nbg_assembly_next', 'npu_service_release']),
}
SESSION_NAMES = {'session': ('conv8', 'dual-u8', 'dual-i8'),
                 'reserved': ('conv8', 'dual-u8', 'dual-i8', 'shufflenet-v2-published', 'mimus-linear-q'),
                 'runtime': ('conv8', 'dual-u8', 'dual-i8', 'shufflenet-v2-published', 'mimus-linear-q')}


def flags(build):
    text = (build / 'CMakeFiles/npu-service-model.dir/flags.make').read_text()
    values = {}
    for line in text.splitlines():
        if line.startswith('C_') and ' = ' in line:
            key, value = line.split(' = ', 1)
            values[key] = shlex.split(value)
    return values['C_DEFINES'] + values['C_INCLUDES'] + values['C_FLAGS']


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def array(name, blob):
    body = ','.join(str(b) for b in blob)
    return f'static const unsigned char {name}[{len(blob)}] __attribute__((aligned(64)))={{{body}}};\n'


def fixtures(artifacts, names, out):
    """Verified catalogue artifacts as a C header, in the given name order."""
    catalogue = {m['name']: m for m in json.loads((ROOT / 'tools/npu-session-models.json').read_text())['models']}
    paths = json.loads(artifacts.read_text())
    text, rows, identities = '/* Generated by checks/npu-session.py from verified catalogue artifacts. */\n', [], {}
    largest = 0
    for index, name in enumerate(names):
        entry, files = catalogue[name], paths[name]
        listed = [('model', files['model'], entry['model'])]
        listed += [('input', p, t['reference']) for p, t in zip(files['inputs'], entry['inputs'], strict=True)]
        listed += [('reference', p, t['reference'])
                   for p, t in zip(files['references'], entry['outputs'], strict=True)]
        tensors = []
        for kind, path, identity in listed:
            blob = Path(path).read_bytes()
            assert len(blob) == identity['bytes'] and hashlib.sha256(blob).hexdigest() == identity['sha256'], \
                f'{path} does not match the catalogue'
            identities[str(Path(path).resolve())] = identity
            symbol = f'fixture_{index}_{kind}_{len(tensors)}'
            text += array(symbol, blob)
            if kind != 'model':
                tensors.append(symbol)
                largest = max(largest, len(blob))
        rows.append('{' + f'fixture_{index}_model_0,sizeof(fixture_{index}_model_0),'
                    f'{len(entry["inputs"])}u,{len(entry["outputs"])}u,'
                    '{' + ','.join(tensors) + '},{' + ','.join(f'sizeof({t})' for t in tensors) + '}}')
    text += ('struct fixture{const unsigned char *model;size_t bytes;unsigned inputs,outputs;'
             'const unsigned char *tensor[16];size_t tensor_bytes[16];};\n'
             f'#define FIXTURES {len(rows)}u\n#define FIXTURE_MAX_TENSOR {largest}u\nstatic const struct fixture fixtures[FIXTURES]={{\n' +
             ',\n'.join(rows) + '};\n')
    (out / 'npu_session_fixtures.h').write_text(text)
    return identities


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('build', type=Path, help='configured central build with npu-service-model built')
    ap.add_argument('output', type=Path, help='new evidence directory')
    ap.add_argument('--mode', choices=sorted(MODES), required=True)
    ap.add_argument('--artifacts', type=Path, help='session mode: catalogue artifact paths')
    ap.add_argument('--icount', action='store_true',
                    help='run QEMU with -icount shift=0, so one counter microsecond is 1,000 instructions')
    args = ap.parse_args()
    build = args.build.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    mode = MODES[args.mode]
    identities = fixtures(args.artifacts, SESSION_NAMES[args.mode], out) if args.mode in SESSION_NAMES else {}
    link = shlex.split((build / 'CMakeFiles/npu-service-model.dir/link.txt').read_text())
    check = 'CMakeFiles/npu-service-model.dir/checks/npu-service.c.obj'
    port = 'CMakeFiles/npu-service-model.dir/npu-service-port.c.obj'
    assert link.count(check) == 1 and link.count(port) == 1, 'unexpected npu-service-model link line'
    compiler = link[0]
    target = [v for v in link if v.startswith('--target=')]
    assert len(target) == 1, 'link line names no single target'
    common = target + flags(build) + [f'-I{out}'] + ['-D' + d for d in mode.get('defines', [])]
    objects, commands = {check: [], port: [port]}, []

    def compile_one(source, extra=()):
        obj = out / (Path(source).name + '.obj')
        command = [compiler] + common + list(extra) + ['-c', str(source), '-o', str(obj)]
        commands.append(command)
        subprocess.run(command, check=True, cwd=build)
        return str(obj)

    for source in mode['sources']:
        objects[check].append(compile_one(ROOT / source))
    if 'port_define' in mode:
        objects[port] = [compile_one(build / 'npu-service-port.c', ['-D' + mode['port_define']])]
    elf = out / f'npu-{args.mode}.elf'
    command = []
    for value in link:
        if value in objects:
            command += objects[value]
        elif value.startswith('-Wl,-Map,'):
            command.append('-Wl,-Map,' + str(out / f'npu-{args.mode}.map'))
        else:
            command.append(value)
    command[command.index('-o') + 1] = str(elf)
    command[1:1] = [f'-Wl,--wrap={symbol}' for symbol in mode.get('wraps', [])]
    commands.append(command)
    subprocess.run(command, check=True, cwd=build)
    results = []
    for case in mode['cases']:
        qemu = [str(QEMU), '-machine', 'virt,gic-version=3,secure=off,virtualization=off',
                '-cpu', 'cortex-a55', '-smp', '2', '-m', '128M', '-nographic', '-monitor', 'none',
                '-semihosting-config', 'enable=on,target=native', '-kernel', str(elf)]
        if case is not None:
            qemu += ['-device', f'loader,addr={CASE_ADDRESS:#x},data={case},data-len=4']
        if args.icount:
            qemu += ['-icount', 'shift=0']
        result = subprocess.run(qemu, capture_output=True, text=True, timeout=180,
                                env=harness.environment())
        log = out / (f'qemu-case{case}.log' if case is not None else 'qemu.log')
        log.write_text(result.stdout + result.stderr)
        lines = result.stdout.splitlines()
        passed = result.returncode == 0 and any(l.startswith(mode['marker']) for l in lines)
        counts = [dict(item.split('=', 1) for item in l.split()[1:] if '=' in item)
                  for l in lines if l.startswith('COUNT ')]
        results.append(dict(case=case, result='pass' if passed else 'fail', exit=result.returncode,
                            line=next((l for l in lines if l.startswith(('PASS', 'FAIL'))), None),
                            counts=[{k: int(v) for k, v in c.items()} for c in counts], qemu=qemu))
        print(f'case {case}:', results[-1]['line'] or result.stderr.strip()[-200:])
        for line in lines:
            if line.startswith('COUNT '):
                print(' ', line)
    record = dict(mode=args.mode, icount=args.icount,
                  result='pass' if all(r['result'] == 'pass' for r in results) else 'fail',
                  cases=results, elf_sha256=sha(elf), commands=commands,
                  sources={s: sha(ROOT / s) for s in mode['sources']}, artifacts=identities,
                  modeled_device=True, physical_access=False)
    harness.save(out / 'result.json', record)
    assert record['result'] == 'pass', f'{args.mode} failed; see {out}'


if __name__ == '__main__':
    main()
