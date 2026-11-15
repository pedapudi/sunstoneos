#!/usr/bin/env python3
"""Check that the queue's job slot and the compiled GRID and GRID_INLINE programs agree on the unified registers: the
driver's PDS and CDM encoders are emulated for all 32 lanes and compared with each program's compiler record; mutations
of the encoders must fail."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
JOB = 'drivers/gpu/pvr_compute_job.c'
SOURCES = ['checks/gpu-queue-inline.c', JOB]
PROGRAMS = {'GPU_QUEUE_ABI_GRID': 'apps/accel-overlap/add4', 'GPU_QUEUE_ABI_GRID_INLINE': 'apps/accel-overlap/add4inline'}
THIRD = '0xf01e0e01u,0xf0074001u,0xfc000002u,0xd3000000u'


def replaced(text, old, new):
    if text.count(old) != 1:
        raise SystemExit(f'mutation anchor not found once: {old!r}')
    return text.replace(old, new)


# Each mutation changes the driver's encoder; the mutated build must report a disagreement.
MUTATIONS = {
    'local-index-at-4': lambda t: replaced(t, 'put32(pds,28,0x8000c000u);', 'put32(pds,28,0x80008000u);'),
    'no-third-write': lambda t: replaced(t, THIRD, '0xf0074001u,0xfc000002u,0xd3000000u,0xd3000000u'),
    'data-units-7': lambda t: replaced(t, 'abi==GPU_QUEUE_ABI_GRID_INLINE?8u:7u', '(void)abi,7u'),
    'block-halves-swapped': lambda t: replaced(t, 'doutw_pair(pds,80,get64(b,0));doutw_pair(pds,112,get64(b,8));',
                                               'doutw_pair(pds,80,get64(b,8));doutw_pair(pds,112,get64(b,0));'),
    'local-index-not-last': lambda t: replaced(t, 'put32(pds,28,0x8000c000u);', 'put32(pds,28,0x0000c000u);'),
    'third-write-common-store': lambda t: replaced(t, 'put32(pds,120,0x8002u);', 'put32(pds,120,0x10008002u);'),
    'block-over-16-accepted': lambda t: replaced(t, '8u*(job->bindings-1u)+job->argument_bytes>GPU_QUEUE_INLINE_BYTES)return 0;',
                                                 '8u*(job->bindings-1u)+job->argument_bytes>GPU_QUEUE_INLINE_BYTES+1u)return 0;'),
}


def slot_defines():
    header = (ROOT / 'include/pvr_compute_queue.h').read_text()
    defines = []
    for name in ('FENCE', 'CDM', 'ARGUMENTS'):
        found = re.findall(r'^#define PVR_COMPUTE_QUEUE_' + name + r' (\d+)u$', header, re.M)
        if len(found) != 1:
            raise SystemExit('slot offset not found: ' + name)
        defines.append(f'-DSLOT_{name}={found[0]}u')
    return defines


def records():
    """abi local_start local_count mask vtxins for each program, from its compiler record and descriptor."""
    values = []
    for abi, folder in PROGRAMS.items():
        m = json.loads((ROOT / folder / 'sources.json').read_text())
        if f'.abi = {abi},' not in (ROOT / folder / 'program.h').read_text():
            raise SystemExit(f'{folder}/program.h is not {abi}')
        number = 1 if abi == 'GPU_QUEUE_ABI_GRID' else 3
        start, count = m['sysval_ranges'][:2]
        values += [str(number), str(start), str(count), str(m['explicit_vtxin_mask']), str(m['abi_vtxins'])]
    return values


def queue_calls():
    """Both queue paths take the slot encoders through the ABI."""
    for path, calls in (('drivers/gpu/pvr_compute_queue.c', ('pvr_compute_job_grid_pds_abi(page,p.abi',
                                                             'pvr_compute_job_grid_pds_abi(slot,p.abi',
                                                             'pvr_compute_job_grid_pds_abi(page,q->abi',
                                                             'pvr_compute_job_grid_inline(page,q->lease,&q->job)',
                                                             'pvr_compute_job_pds_data_units(q->abi)')),
                        ('drivers/gpu/pvr_compute_fast.c', ('pvr_compute_job_grid_inline(page,q->lease,job)',
                                                            'pvr_compute_job_pds_data_units(q->abi)'))):
        text = (ROOT / path).read_text()
        for call in calls:
            if text.count(call) != 1:
                raise SystemExit(f'FAIL {path} does not call {call} once')
        if 'pvr_compute_job_grid_pds(' in text or 'pvr_compute_job_grid_cdm(' in text:
            raise SystemExit(f'FAIL {path} calls an encoder that ignores the ABI')


def build(output, sources):
    binary = output / 'binary'
    command = ['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-ffunction-sections',
               '-Wl,--gc-sections', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               '-fno-omit-frame-pointer', *slot_defines(), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'drivers/gpu'),
               *map(str, sources), '-o', str(binary)]
    return command, [str(binary), *records()]


def run(output, index, command):
    result = subprocess.run(command, capture_output=True, text=True,
                            env={**harness.environment(), 'ASAN_OPTIONS': 'detect_leaks=0', 'TMPDIR': str(output)})
    (output / f'{index}.log').write_text(result.stdout + result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    queue_calls()
    steps = []
    index = 0

    def step(command, expect_pass):
        nonlocal index
        result = run(args.output, index, command)
        index += 1
        steps.append({'command': command, 'exit': result.returncode, 'expect_pass': expect_pass})
        harness.save(args.output / 'result.json', {'physical_access': False, 'steps': steps})
        return result

    compile_command, run_command = build(args.output, [ROOT / s for s in SOURCES])
    step(compile_command, True).check_returncode()
    result = step(run_command, True)
    print(result.stdout + result.stderr, end='')
    result.check_returncode()
    for name, mutate in MUTATIONS.items():
        folder = args.output / name
        folder.mkdir(exist_ok=True)
        mutated = folder / Path(JOB).name
        mutated.write_text(mutate((ROOT / JOB).read_text()))
        compile_command, run_command = build(folder, [ROOT / SOURCES[0], mutated])
        step(compile_command, True).check_returncode()
        result = step(run_command, False)
        if result.returncode == 0:
            raise SystemExit(f'FAIL mutation {name} passed: {result.stdout.strip()}')
        print(f'mutation {name} fails as required: {(result.stdout + result.stderr).strip().splitlines()[-1]}')


if __name__ == '__main__':
    main()
