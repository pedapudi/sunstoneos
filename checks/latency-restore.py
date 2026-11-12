#!/usr/bin/env python3
"""Check deferred mask closure and every assembly return boundary."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from harness import CLANG, SANITIZE, environment

GATE_RUNS = [['--output', '{output}']]
CHECK_INPUTS = ['runtime/latency_restore.c', 'patches/freertos-mask-returns.patch',
                'checks/latency-restore.c', 'checks/latency-restore-host/FreeRTOS.h',
                'checks/latency-restore-host/task.h', 'checks/latency-restore-host/sunstone_latency.h']


def linked_audit(image, port_object, output, objdump):
    """Check actual post-restore callees and each selected port return."""
    disassembly = subprocess.check_output([objdump, '-d', '--no-show-raw-insn', image], text=True)
    (output / 'linked-disassembly.txt').write_text(disassembly)
    functions, current = {}, None
    for line in disassembly.splitlines():
        header = re.match(r'^[0-9a-f]+ <([^>]+)>:$', line)
        if header:
            current = header.group(1)
            functions[current] = []
        elif current:
            instruction = re.match(r'^\s*[0-9a-f]+:\s+([a-z0-9.]+)\s*(.*)$', line)
            if instruction:
                functions[current].append(instruction.groups())
    pending, seen = ['ss_latency_exception_return'], set()
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        assert name in functions, ('missing reachable function', name)
        seen.add(name)
        for opcode, operands in functions[name]:
            assert not re.search(r'\b(?:[vqdsbh][0-9]+|fpcr|fpsr)\b', operands, re.I), (name, opcode, operands)
            assert opcode not in ('br', 'blr', 'braa', 'brab', 'blraa', 'blrab'), (name, opcode, operands)
            if opcode in ('bl', 'b'):
                target = re.search(r'<([^>]+)>', operands)
                assert target, ('unresolved direct call', name, opcode, operands)
                callee = target.group(1).split('+')[0]
                if callee != name:
                    pending.append(callee)
    port = subprocess.check_output([objdump, '-dr', '--no-show-raw-insn', port_object], text=True)
    (output / 'port-disassembly.txt').write_text(port)
    symbols = subprocess.check_output([objdump, '-t', image], text=True)
    startup = 'ss_sched_trace_startup_markers' in symbols
    if startup:
        guard = next((line.split() for line in symbols.splitlines()
                      if line.endswith(' ss_latency_first_return_seen')), None)
        assert guard and int(guard[-2], 16) == 8 * 8, 'startup return guards must occupy eight words'
    instructions = []
    for line in port.splitlines():
        instruction = re.match(r'^\s*[0-9a-f]+:\s+([a-z0-9.]+)\s*(.*)$', line)
        if instruction:
            instructions.append([*instruction.groups(), ''])
        elif instructions and 'R_AARCH64_' in line:
            instructions[-1][2] = line.split()[-1]
    returns = 0
    for index, (opcode, operands, relocation) in enumerate(instructions):
        if opcode == 'eret':
            returns += 1
            recent = list(reversed(instructions[:index]))
            call = next((row for row in recent if row[0] in ('bl', 'eret')), None)
            assert call and call[0] == 'bl' and call[2] == 'ss_latency_exception_return', \
                ('return lacks the final recorder hook', index)
            if startup:
                tail = recent[:recent.index(call)]
                assert all(not re.search(r'\b(?:[vqdsbh][0-9]+|fpcr|fpsr)\b', row[1], re.I)
                           for row in tail), ('SIMD after the return observer', index)
                assert any(row[2] == 'ss_latency_first_return_seen' for row in tail) and \
                       any(row[2] == 'ss_sched_trace_startup_markers' for row in tail), \
                    ('startup marker must follow the return observer', index)
    assert returns, 'selected port has no exception return'
    report = {'passed': True, 'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
              'port_object_sha256': hashlib.sha256(port_object.read_bytes()).hexdigest(),
              'reachable_functions': sorted(seen), 'selected_return_hooks': returns,
              'scope': 'no SIMD or indirect callees after task FPU restore; mask timestamps exclude entry saves and post-timestamp observer/temporary restore/ERET work',
              'startup_scope': 'with scheduler trace, the first return marker includes observer bookkeeping and excludes its timestamp store, final temporary GP restore and ERET'}
    (output / 'linked-result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--image', type=Path)
    parser.add_argument('--port-object', type=Path)
    parser.add_argument('--objdump', type=Path, default=Path('/usr/bin/llvm-objdump-21'))
    parser.add_argument('--linked-only', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if bool(args.image) != bool(args.port_object) or args.linked_only and not args.image:
        parser.error('--image and --port-object are required together')
    if args.linked_only:
        linked_audit(args.image, args.port_object, output, str(args.objdump))
        return
    common = [str(CLANG), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
              '-DSS_RESTORE_SPAN_TEST=1', '-I'+str(root/'checks/latency-restore-host')]
    source = root/'runtime/latency_restore.c'
    binary = output/'restore-spans'
    subprocess.run([*common, *SANITIZE, source, root/'checks/latency-restore.c', '-o', binary],
                   check=True, env=environment())
    subprocess.run([binary], check=True, env=environment())
    text = source.read_text()
    for name, old, new in [('el0', 'if (!ss_restore_test_el1()) return;', ''),
                           ('masked-return', '!(spsr & UINT64_C(0x80)) && ', '')]:
        assert text.count(old) == 1
        broken = output/(name+'.c')
        broken.write_text(text.replace(old, new))
        bad = output/(name+'-negative')
        subprocess.run([*common, '-Wno-unused-parameter', broken, root/'checks/latency-restore.c', '-o', bad],
                       check=True, env=environment())
        done = subprocess.run([bad], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=environment())
        (output/(name+'-negative.log')).write_bytes(done.stdout)
        assert done.returncode != 0
    print('PASS negative controls: unsafe EL0 read and premature masked return both fail')
    if args.image:
        linked_audit(args.image, args.port_object, output, str(args.objdump))


if __name__ == '__main__':
    main()
