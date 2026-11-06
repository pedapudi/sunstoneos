#!/usr/bin/env python3
"""Run OS NPU failure predicates and actual SDK/built-in reports on the host.

Use the selected OS image's flags.make for its FreeRTOS/provider types. Device
operations and task waits are modeled. This check opens no device and links
no board image. Red cases remove only the complete-copy guards in a scratch
copy; the actual report fixture must reject that missing validation.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import subprocess
import sys
from harness import SANITIZE, environment

CHECK_INPUTS = ('apps/os/npu.c', 'apps/os/npu-declaration.c', 'apps/os/npu.h', 'apps/os/npu-client.c',
                'apps/os/npu-report.py', 'include/sunstone_crumb.h',
                'tools/a733-crumbs.py', 'checks/os-npu-first-failure.c',
                'checks/os-npu-sdk-report.c', 'drivers/storage/storage_digest.c')
ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--native-flags', '{build:sup}/CMakeFiles/sunstone-a733-os.dir/flags.make', '--output', '{output}']]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--native-flags', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--cc', default='/usr/bin/clang-21')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
flags = a.native_flags.read_text()
def value(name):
    return shlex.split(next(line.split('=', 1)[1] for line in flags.splitlines() if line.startswith(name + ' =')))
args = value('C_DEFINES') + value('C_INCLUDES')
old_root = next(arg[2:-len('/apps/os')] for arg in args if arg.startswith('-I') and arg.endswith('/apps/os'))
args = [arg.replace(old_root, str(ROOT)) for arg in args]
# Host mocks retain note packing; ARM SVC assembly is checked by the native ELF.
args += ['-USUNSTONE_CRUMBS', '-std=c11', '-O2', '-g', '-ffunction-sections', '-fdata-sections', *SANITIZE]
commands, results = [], []

def run(command, red=False):
    result = subprocess.run(command, capture_output=True, text=True,
                            env={**environment(), 'TMPDIR': str(a.output.resolve())})
    row = dict(command=command, returncode=result.returncode, stdout=result.stdout, stderr=result.stderr)
    commands.append(row)
    if (result.returncode == 0) == red:
        raise AssertionError(row)
    return result


def build(source, output, definitions=()):
    run([a.cc, *args, *definitions, str(source), str(ROOT / 'drivers/storage/storage_digest.c'),
         '-Wl,--gc-sections', '-o', str(output)])


def module(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


report = module('npu_report', 'apps/os/npu-report.py')
crumbs = module('npu_crumbs', 'tools/a733-crumbs.py')
defines, _ = crumbs.header()
exe = a.output / 'first-failure'
build(ROOT / 'checks/os-npu-first-failure.c', exe)
operations = ('STATUS', 'OPEN', 'SUBMIT', 'CANCEL', 'REAP', 'CLOSE', 'WAIT', 'LOAD')
for case in range(20):
    result = run([str(exe), str(case)])
    site, observed, ticket, operation = map(int, result.stdout.split())
    decoded = None
    if site:
        decoded = crumbs.board_failure(dict(fault=0xb00, esr=site, elr=0, far=observed), defines, None)
        if site & defines['NPU_REQUEST']:
            assert decoded['request_operation'] == operations[operation]
            assert decoded['expected_ticket_low8'] == ticket & 255
            assert decoded['observed_word'] == hex(observed)
    results.append(dict(case='first-failure-' + str(case), decoded=decoded))

for builtin in (False, True):
    exe = a.output / ('builtin-report' if builtin else 'sdk-report')
    build(ROOT / 'checks/os-npu-sdk-report.c', exe, ['-DREPORT_BUILTIN'] if builtin else [])
    cases = [(0, 'refused')] if builtin else [(0, 'completed'), (1, 'retained'), (2, 'retained'), (3, 'refused'), (4, 'retained'), (5, 'retained')]
    for case, wanted in cases:
        raw = a.output / (exe.name + '-' + str(case) + '.bin')
        actual = run([str(exe), str(case), str(raw)])
        site, observed = map(int, actual.stdout.split())
        failure = None
        if not builtin and case in (1, 2, 4, 5):
            lines = (ROOT / 'apps/os/npu-client.c').read_text().splitlines()
            guard = '    if(offset!=' + ('input' if case == 4 else 'output') + '_bytes) {'
            note_line = lines.index(guard) + 2
            assert site & 0xfff == note_line and observed == (4 if case == 5 else 0)
            failure = crumbs.board_failure(dict(fault=0xb00, esr=site, elr=0, far=observed), defines, None)
            assert failure['group'] == 'npu' and failure['aux'] == 0
        decoded = report.decode(raw.read_bytes(), 10, 20)
        assert report.classify(decoded) == wanted
        results.append(dict(case=exe.name + '-' + str(case), classification=wanted, decoded=decoded, first_failure=failure))

for label, offset in [('mismatches', 44), ('pattern-bytes', 64), ('digest', 72), ('reserved-pattern', 68)]:
    raw = bytearray((a.output / 'sdk-report-0.bin').read_bytes())
    raw[offset:offset + 4] = (1).to_bytes(4, 'little')
    try:
        report.decode(raw, 10, 20)
    except ValueError as error:
        results.append(dict(case='malformed-sdk-' + label, refusal=str(error)))
    else:
        raise AssertionError('Malformed SDK report accepted: ' + label)

client = (ROOT / 'apps/os/npu-client.c').read_text()
for kind in ('input', 'output'):
    guard = '    if(offset!=' + kind + '_bytes) {\n'
    assert client.count(guard) == 1, 'Update the complete-copy red fixture for the current implementation'
    start = client.index(guard)
    end = client.index('    }\n', start) + len('    }\n')
    client = client[:start] + client[end:]
red_client = a.output / 'incomplete-client.c'
red_client.write_text(client)
red_fixture = a.output / 'incomplete-report.c'
red_fixture.write_text((ROOT / 'checks/os-npu-sdk-report.c').read_text().replace('../apps/os/npu-client.c', str(red_client.resolve())))
exe = a.output / 'incomplete-report'
build(red_fixture, exe)
run([str(exe), '0', str(a.output / 'red-control.bin')])
for case in (1, 2, 4, 5):
    result = run([str(exe), str(case), str(a.output / 'unused.bin')], red=True)
    condition = '!memcmp(tensor_input,input,sizeof(input))' if case == 4 else '!memcmp(output,tensor_output,sizeof(output))'
    assert result.returncode == -6 and condition in result.stdout
    results.append(dict(case='missing-copy-check-' + str(case), rejected=True))

noteless = (ROOT / 'apps/os/npu-client.c').read_text()
note = '        board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(NPU)|((model&0x7ffu)<<16),\n            (uint32_t)offset,SUNSTONE_CRUMB_HERE());\n'
assert noteless.count(note) == 2
noteless = noteless.replace(note, '')
red_client = a.output / 'noteless-client.c'
red_client.write_text(noteless)
red_fixture = a.output / 'noteless-report.c'
red_fixture.write_text((ROOT / 'checks/os-npu-sdk-report.c').read_text().replace('../apps/os/npu-client.c', str(red_client.resolve())))
exe = a.output / 'noteless-report'
build(red_fixture, exe)
run([str(exe), '0', str(a.output / 'noteless-control.bin')])
for case in (4, 5):
    result = run([str(exe), str(case), str(a.output / 'unused.bin')], red=True)
    assert result.returncode == -6 and '(first_site>>12&15)==SUNSTONE_CRUMB_FAIL_NPU' in result.stdout
    results.append(dict(case='missing-guard-note-' + str(case), rejected=True))

inputs = {str(ROOT / path): hashlib.sha256((ROOT / path).read_bytes()).hexdigest()
          for path in (*CHECK_INPUTS, 'checks/os-npu-sdk-contract.py')}
inputs[str(a.native_flags)] = hashlib.sha256(a.native_flags.read_bytes()).hexdigest()
(a.output / 'result.json').write_text(json.dumps(dict(result='pass', source_inputs=inputs,
    cases=results, commands=commands, physical_access=False), indent=1) + '\n')
print('PASS OS NPU contract: 20 failure predicates, seven actual producer/parser reports, four malformed reports, four missing-copy red cases, two missing-note red cases')
