#!/usr/bin/env python3
"""Check the linked mimus linear-layer program against the pinned compiler.

Compiles shaders/pvr_mimus_linear.cl with apps/gpu-program/compile.py
(profile three, session form) and requires the USC constant in
drivers/gpu/pvr_compute_mimus_usc.c to equal the compiled code, its length,
temporary-register count and recorded source, compiler and program digests.
From the compiler's final listing it counts the instruction groups of the
inner multiply-accumulate loop per multiply-accumulate (MAC), and confirms
the completion proof's order: every sum store carries an issue-data fence
and its wait, the tag store is the last store, and no store lies in the MAC
loop. It also compiles the session code for AArch64 with the image's kernel
options and -Werror, counts the compares that pvr_mimus_submit makes before
its first store, and confirms that the session code makes no address
translation. --write regenerates the constant. No USC code is executed and no
hardware is accessed."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'shaders/pvr_mimus_linear.cl'
CONSTANT = ROOT / 'drivers/gpu/pvr_compute_mimus_usc.c'
MACS_PER_ITERATION = 64  # 4 rows by 4 channels by 4 K steps per lane
GROUP = re.compile(r'^\s+\d{4}:')


def constant_text(code, manifest):
    words = [int.from_bytes(code[i:i + 4], 'little') for i in range(0, len(code), 4)]
    rows = ['    ' + ', '.join(f'0x{w:08x}u' for w in words[i:i + 6]) + ',' for i in range(0, len(words), 6)]
    return '\n'.join([
        '/* USC code of shaders/pvr_mimus_linear.cl from the pinned Mesa compiler:',
        ' * apps/gpu-program/compile.py compile --profile 3 --session. Generated and',
        ' * checked by checks/gpu-mimus-program.py; regenerate with --write.',
        f' * Mesa {manifest["mesa_revision"]}',
        f' * source sha256 {manifest["source_sha256"]}',
        f' * compiler sha256 {manifest["compiler_sha256"]}',
        f' * program sha256 {manifest["program_sha256"]} */',
        '#include "pvr_compute_mimus.h"',
        '',
        f'const uint32_t pvr_compute_mimus_usc[{len(words)}] = {{',
        *rows,
        '};',
        'const uint32_t pvr_compute_mimus_usc_bytes = sizeof(pvr_compute_mimus_usc);',
        f'const uint32_t pvr_compute_mimus_usc_temps = {manifest["metadata"][0]};',
        ''])


def listing(program, env):
    """The compiler's final IR, printed by the same instrumented executable."""
    run_env = {**env, 'PCO_DEBUG': 'no_pred_cf', 'PCO_DEBUG_PRINT': 'cs,internal,stats', 'PCO_COLOR': '0'}
    out = program / 'listing'
    out.mkdir(exist_ok=True)
    p = subprocess.run([str(program / 'pco_clc_metadata'), str(program / 'kernel.spv'),
                        str(out / 'kernel.h'), str(out / 'kernel.c')],
                       env=run_env, capture_output=True, text=True)
    text = p.stdout + p.stderr
    (out / 'listing.txt').write_text(text)
    assert p.returncode == 0, 'listing run failed'
    start = text.index('shader ir after passes:')
    after = text[start:text.index('CS shader:', start)]
    stats = re.search(r'CS shader: (\d+) code size, (\d+) scratch size, (\d+) spill count, (\d+) temp count', text)
    return after.splitlines(), [int(x) for x in stats.groups()]


def loops(lines):
    """Per loop: the instruction groups executed per iteration (body,
    interlogue and epilogue; the prologue runs once) and their operations."""
    found, stack = {}, []
    for line in lines:
        start = re.search(r'loop (L\d+) \{', line)
        if start:
            stack.append(start.group(1)); found[start.group(1)] = {'groups': [], 'counting': False}
            continue
        for name in stack:
            if f'loop {name} prologue end' in line: found[name]['counting'] = True
        if GROUP.match(line):
            for name in stack:
                if found[name]['counting']: found[name]['groups'].append(line)
        for name in list(stack):
            if f'loop {name} epilogue end' in line:
                found[name]['counting'] = False; stack.remove(name)
    return {k: v['groups'] for k, v in found.items()}


def operation(line):
    m = re.search(r'(?:p0: |backend: |ctrl: )([a-z0-9_.]+)', line)
    return m.group(1) if m else 'other'


KERNEL_FLAGS = ['--target=aarch64-none-elf', '-march=armv8-a', '-mstrict-align', '-ffreestanding', '-fno-builtin',
                '-fno-stack-protector', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-ffunction-sections',
                '-fdata-sections', '-O2', '-g', '-Wall', '-Wextra', '-Werror']


def session_code(out):
    """Admission compares before the first job-word store, and the calls."""
    objects = []
    for name in ('pvr_compute_mimus', 'pvr_compute_mimus_usc'):
        obj = out / f'{name}.o'
        subprocess.run(['clang-21', '-std=c11', '-c', str(ROOT / f'drivers/gpu/{name}.c'), '-o', str(obj),
                        '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime/include')] + KERNEL_FLAGS, check=True)
        objects.append(obj)
    dump = subprocess.run(['llvm-objdump-21', '-dr', '--no-show-raw-insn', str(objects[0])],
                          capture_output=True, text=True, check=True).stdout
    (out / 'session.lst').write_text(dump)
    submit = dump[dump.index('<pvr_mimus_submit>:'):]
    submit = submit[:submit.index('\n\n')].splitlines()[1:]
    compares = 0
    for line in submit:
        op = line.split('\t')[1].split()[0] if '\t' in line else ''
        if op.startswith(('str', 'stp')) and '[sp' not in line: break  # the first job-word store
        compares += op in ('cmp', 'cmn', 'ccmp', 'tst', 'cbz', 'cbnz', 'tbz', 'tbnz')
    calls = sorted({l.split('R_AARCH64_CALL26')[1].split()[0] for l in submit if 'R_AARCH64_CALL26' in l})
    assert compares <= 12, compares
    assert calls == ['aarch64_device_cache', 'pvr_compute_job_grid_cdm'], calls
    assert not re.search(r'\tat\t|aarch64_device_translate|sha256', dump)
    return compares, calls


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for x in ('mesa-source', 'mesa-build', 'ninja', 'output'): p.add_argument('--' + x, type=Path, required=True)
    p.add_argument('--write', action='store_true', help='regenerate the linked constant')
    a = p.parse_args()
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('program_compile', ROOT / 'apps/gpu-program/compile.py')
    tool = importlib.util.module_from_spec(spec); spec.loader.exec_module(tool)
    manifest = tool.compile_program(SOURCE, a.mesa_source, a.mesa_build, out / 'program', a.ninja, 3, True)
    blob = (out / 'program' / 'program.bin').read_bytes()
    code = blob[8:]
    text = constant_text(code, manifest)
    if a.write: CONSTANT.write_text(text)
    assert CONSTANT.read_text() == text, 'linked constant differs from the compiler product; rerun with --write'
    assert manifest['source_sha256'] == hashlib.sha256(SOURCE.read_bytes()).hexdigest()
    assert manifest['session'] and manifest['metadata'][1:3] == [5, 3] and manifest['sysval_ranges'] == [4, 1, 0, 3, 0, 0]

    lines, (size, scratch, spills, temps) = listing(out / 'program', os.environ)
    assert size == len(code) and scratch == 0 and spills == 0 and temps == manifest['metadata'][0]
    found = loops(lines)
    # The innermost loop with the 64 multiply-adds of one 4 by 4 by 4 step.
    mac_loop = min((k for k in found if sum(operation(g) == 'imadd32.s' for g in found[k]) >= MACS_PER_ITERATION),
                   key=lambda k: len(found[k]))
    body = found[mac_loop]
    ops = {}
    for g in body: ops[operation(g)] = ops.get(operation(g), 0) + 1
    assert ops.get('imadd32.s', 0) >= MACS_PER_ITERATION and ops.get('ld', 0) == 12
    # Completion proof: sums carry idf and wdf; the tag store is last and
    # has neither; nothing is stored inside the MAC loop.
    groups = [g for g in lines if GROUP.match(g)]
    stores = [i for i, g in enumerate(groups) if operation(g).startswith('st')]
    assert stores and all(operation(g) != 'st.writethrough' for g in body if 'backend: st' in g)
    for i in stores[:-1]:
        assert operation(groups[i + 1]) == 'idf' and operation(groups[i + 2]) == 'wdf', groups[i]
    tag = stores[-1]
    assert operation(groups[tag + 1]) == 'wdf' and 'idf' not in groups[tag + 1]
    assert all(not operation(g).startswith('st') for g in groups[tag + 1:])
    assert not any('fence' in operation(g) for g in groups)
    compares, calls = session_code(out)
    cccb = (ROOT / 'include/pvr_cccb.h').read_text()
    header = int(re.search(r'#define PVR_CCCB_HEADER_BYTES (\d+)u', cccb).group(1))
    ring = 2 * header + 96 + 8  # the compute command and the timeline UPDATE
    result = {
        'scope': 'Pinned compiler output and listing; no USC execution',
        'submit_compares': compares, 'submit_calls': calls,
        'ring_and_job_word_bytes_per_job': ring + 32 + 60,
        'code_bytes': len(code), 'program_bytes': tool.SESSION_CODE + 64, 'temps': temps,
        'coefficients': manifest['metadata'][2], 'mac_loop_groups': len(body),
        'instructions_per_mac': len(body) / MACS_PER_ITERATION, 'mac_loop_operations': ops,
        'sum_stores_with_idf': len(stores) - 1, 'tag_store_last': True,
        'manifest': {k: manifest[k] for k in ('mesa_revision', 'source_sha256', 'compiler_sha256', 'program_sha256')},
    }
    (out / 'program-check.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: result[k] for k in ('code_bytes', 'temps', 'mac_loop_groups', 'instructions_per_mac',
                                             'submit_compares', 'ring_and_job_word_bytes_per_job')}))
    print('PASS linked mimus program equals the pinned compiler product; sum stores fenced before the tag store')


if __name__ == '__main__':
    main()
