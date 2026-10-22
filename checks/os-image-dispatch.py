#!/usr/bin/env python3
"""Check the OS image's placement of mimus linear layers on the CPU or GPU against its design budgets.

The check covers docs/recognition-dispatch.md on the host; no device is used:

- apps/os/placement.py emits the placement table for the A55 pair at 494 MHz
  and the A76 pair at 2,002 MHz from the bench models, and the table, the
  call and job counts and the modeled linear-stage time equal the recorded
  values below;
- a placement bound that rounds to zero rows gives n_gpu = 0;
- checks/os-image-dispatch.c, built with AddressSanitizer and
  UndefinedBehaviorSanitizer, runs apps/os/dispatch.c over the bench call
  list with exact integer sums compared against a CPU-only reference, and
  over the longest accepted input (the 1 MiB WAV slot), where the A55
  table splits a call into three jobs. It covers no OPEN before the model
  hashes, a refused OPEN, a refused job, RESET and LOST, a reply that never
  comes, two jobs that end at the worker's deadline, and the slot mapping
  rule with one writable, unreadable or Device page among 1,024;
- the recognizer patch applies with and without the capture patch and opens
  the session only after every model hash has matched;
- os_dispatch_place compiles for AArch64 to two loads and one compare;
- with --configure-from, CMake refuses the option without the resident
  lifetime or either build constant, and configures with both.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('placement', ROOT / 'apps/os/placement.py')
placement = importlib.util.module_from_spec(spec)
spec.loader.exec_module(placement)

# Emitted tables by shape (K x N: n_gpu, job_rows) for the bench recording.
EXPECTED = {
    'a55': {'1584x176': (176, 224), '3520x176': (176, 96), '176x704': (688, 544), '704x176': (176, 512),
            '176x176': (176, 2144), '176x352': (352, 1056), '176x1025': (993, 352)},
    'a76': {'1584x176': (112, 352), '3520x176': (112, 160), '176x704': (448, 832), '704x176': (112, 832),
            '176x176': (112, 3392), '176x352': (224, 1696), '176x1025': (641, 576)},
}
# Modeled linear stage at the seeds, and the design's regression bounds.
MODELED_BOUND = {'a55': 0.815, 'a76': 0.408}
DESIGN_BOUND = {'a55': 0.79, 'a76': 0.39}
BENCH = {'placed_calls': 204, 'jobs': 381, 'block_jobs': 20, 'block_padded_rows': 8,
         'block_codes': 7032960, 'block_clean_lines': 219780, 'outputs': 12923134}
BENCH_FRAMES = 222
# The recorded WAV slot holds at most 1 MiB (apps/mimus-board/configure.py):
# a 44-byte header and 524,266 samples at 16 kHz, 640 samples per encoder frame.
LONGEST_FRAMES = -(-((1048576 - 44) // 2) // 640)
COMPILE = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
           '-fno-sanitize-recover=all', '-Iinclude', '-Iplatform/a733-smp-cached', '-Iapps/os',
           '-DSUNSTONE_BOARD_SUPERVISED=1', '-DSUNSTONE_BOARD_USB2_DUAL=1', '-DSUNSTONE_BOARD_RESIDENT=1',
           '-DSUNSTONE_A733_OS_GPU_LINEAR=1']
CACHE_ENTRIES = ('FREERTOS_DIR', 'TINYUSB_DIR', 'SUNSTONE_LLD', 'SUNSTONE_NETWORK_C_HEADERS', 'SUNSTONE_RSN_DIR',
                 'SUNSTONE_TCP_DIR', 'SUNSTONE_BOARD_SUPERVISED', 'SUNSTONE_GPU_FIRMWARE', 'MIMUS_DIR',
                 'NEWLIB_ROOT', 'NEWLIB_SMP_ROOT', 'SUNSTONE_BT_SECURITY_DIR', 'SUNSTONE_MIMUS_BOARD_APPLICATION')


def patch(source, patch_file, output):
    subprocess.run(['patch', '-s', '--fuzz=0', '--reject-file=-', '--output', str(output), str(source),
                    str(patch_file)], check=True)
    return output.read_text()


def recognizer_order(out):
    """The patch applies to both recognizer sources and opens after the hashes."""
    base = ROOT / 'apps/mimus-board/recognition.cc'
    capture = out / 'recognition-capture.cc'
    patch(base, ROOT / 'apps/os/recognition.patch', capture)
    for name, source in (('recorded', base), ('capture', capture)):
        text = patch(source, ROOT / 'apps/os/recognition-dispatch.patch', out / ('recognition-gpu-%s.cc' % name))
        hashed = text.index('if(std::memcmp(hash,mimus_asset_sha256[i],32)){failed();}')
        opened = text.index('os_dispatch_open();')
        assert text.count('os_dispatch_open();') == 1 and hashed < opened < text.index('mimus_transcribe_recorded'), name
        assert 'xTaskCreateStaticAffinitySet(recognize' in text and 'void os_dispatch_stop(void){failed();}' in text
    return ['recorded', 'capture']


def run_model(out, cores, models, numeric_frames):
    clocks = placement.Clocks(cores)
    tensors = placement.merged_tensors(*models)
    table = placement.table(tensors, clocks, BENCH_FRAMES)
    directory = out / cores
    directory.mkdir()
    (directory / 'placement.h').write_text(placement.header(tensors, table, clocks, BENCH_FRAMES))
    summary = placement.summary(tensors, table, clocks, BENCH_FRAMES)
    shapes = {k: (v['n_gpu'], v['job_rows']) for k, v in summary['shapes'].items()}
    assert shapes == EXPECTED[cores], (cores, shapes)
    assert summary['linear_seconds'] <= MODELED_BOUND[cores], (cores, summary['linear_seconds'])
    core = placement.CORES[cores]['pair'][0]
    binary = directory / 'dispatch'
    subprocess.run(['clang'] + COMPILE + ['-I' + str(directory), '-DOS_RECOGNITION_CORE=%d' % core,
                    'checks/os-image-dispatch.c', '-o', str(binary)], cwd=ROOT, check=True)
    results = {}
    for frames in (BENCH_FRAMES, LONGEST_FRAMES):
        calls = directory / ('calls-%d.txt' % frames)
        calls.write_text(''.join(' '.join(map(str, c)) + '\n' for c in placement.call_list(tensors, frames)))
        command = [str(binary), str(calls)] + (['--numeric'] if frames == numeric_frames else [])
        line = subprocess.run(command, check=True, capture_output=True, text=True).stdout
        results[frames] = json.loads(line)
        assert results[frames]['zero_row_jobs'] == 0 and results[frames]['mismatched'] == 0
        assert results[frames]['lookups_per_call'] == 1.0
    bench = results[BENCH_FRAMES]
    # The C hook and the Python time model split calls the same way.
    assert bench['jobs'] == summary['jobs'] and bench['placed_calls'] == summary['placed_calls']
    if cores == 'a55':
        for key, value in BENCH.items():
            assert bench[key] == value, (key, bench[key], value)
    # On the A55 pair the output head's 352-row bound splits the longest
    # input into three jobs; on the A76 pair every bound exceeds half of it.
    assert results[LONGEST_FRAMES]['max_jobs_per_call'] >= (3 if cores == 'a55' else 2), results[LONGEST_FRAMES]
    return {'summary': summary, 'bench': bench, 'longest': results[LONGEST_FRAMES],
            'design_bound_s': DESIGN_BOUND[cores],
            'placement_h_sha256': hashlib.sha256((directory / 'placement.h').read_bytes()).hexdigest()}


def zero_bound():
    """A tensor whose job bound rounds below 32 rows stays on the CPU."""
    clocks = placement.Clocks('a55')
    assert placement.job_rows(262144, 16) == 0
    tensor = {'name': 'encoder.layers.0.feed_forward1.linear1.weight', 'dtype': placement.DTYPE_I8,
              'act': placement.ACT_INT16, 'n': 16, 'k': 262144, 'k_padded': 262144, 'row_stride': 262144}
    assert placement.table([tensor], clocks, BENCH_FRAMES) == [(0, 0)]
    return {'k': 262144, 'n': 16, 'n_gpu': 0}


def lookup_instructions(out):
    """Instructions of os_dispatch_place on AArch64."""
    obj = out / 'dispatch.o'
    subprocess.run(['clang', '--target=aarch64-none-elf', '-O2', '-ffreestanding', '-c', '-Iinclude', '-Iapps/os',
                    '-I' + str(out / 'a55'), '-DOS_RECOGNITION_CORE=2', 'apps/os/dispatch.c', '-o', str(obj)],
                   cwd=ROOT, check=True)
    listing = subprocess.run(['llvm-objdump-21', '-d', '--no-show-raw-insn', str(obj)], check=True,
                             capture_output=True, text=True).stdout
    body = listing.split('<os_dispatch_place>:')[1].split('\n\n')[0]
    ops = [line.split('\t')[1].split()[0] if '\t' in line else line.split()[1]
           for line in body.strip().splitlines()]
    loads = [o for o in ops if o.startswith('ldr')]
    compares = [o for o in ops if o in ('cmp', 'cmn', 'ccmp', 'tst', 'cbz', 'cbnz')]
    assert len(loads) == 2 and len(compares) == 1, ops
    return {'instructions': ops, 'loads': len(loads), 'compares': len(compares)}


def configure_cases(out, build, models):
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        m = re.match(r'([A-Za-z0-9_]+):[A-Z]+=(.*)$', line)
        if m and m.group(1) in CACHE_ENTRIES:
            cache[m.group(1)] = m.group(2)
    base = ['cmake', '-S', str(ROOT), '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'cmake/aarch64-clang.cmake'),
            '-DSUNSTONE_A733_OS=ON', '-DSUNSTONE_A733_OS_GPU_LINEAR=ON'] + \
        ['-D%s=%s' % item for item in sorted(cache.items())]
    env = dict(os.environ, PATH=str(Path(cache['SUNSTONE_LLD']).parent) + os.pathsep + os.environ['PATH'])
    cases = [
        ('supervised', ['-DSUNSTONE_A733_OS_RESIDENT=OFF', '-DA733_GPU_SLOTS_EL0_READ_ONLY=ON',
                        '-DA733_GPU_SLOTS_CLEAN_INVALIDATE=ON'], 'resident lifetime'),
        ('no-constants', ['-DSUNSTONE_A733_OS_RESIDENT=ON'], 'read-only at EL0'),
        ('no-clean-invalidate', ['-DSUNSTONE_A733_OS_RESIDENT=ON', '-DA733_GPU_SLOTS_EL0_READ_ONLY=ON'],
         'clean-and-invalidate'),
        ('no-read-only', ['-DSUNSTONE_A733_OS_RESIDENT=ON', '-DA733_GPU_SLOTS_CLEAN_INVALIDATE=ON'],
         'read-only at EL0'),
        ('both', ['-DSUNSTONE_A733_OS_RESIDENT=ON', '-DA733_GPU_SLOTS_EL0_READ_ONLY=ON',
                  '-DA733_GPU_SLOTS_CLEAN_INVALIDATE=ON', '-DSUNSTONE_A733_OS_MODEL_DIR=' + str(models[0].parent)], None),
        ('both-audio', ['-DSUNSTONE_A733_OS_RESIDENT=ON', '-DSUNSTONE_A733_OS_AUDIO=ON',
                        '-DA733_GPU_SLOTS_EL0_READ_ONLY=ON', '-DA733_GPU_SLOTS_CLEAN_INVALIDATE=ON',
                        '-DSUNSTONE_A733_OS_MODEL_DIR=' + str(models[0].parent)], None),
    ]
    results = {}
    for name, options, refusal in cases:
        directory = out / ('configure-' + name)
        result = subprocess.run(base + ['-B', str(directory)] + options, env=env, capture_output=True, text=True)
        if refusal:
            assert result.returncode != 0 and refusal in result.stderr, (name, result.stderr[-2000:])
        else:
            assert result.returncode == 0, (name, result.stderr[-2000:])
            generated = directory / 'os-image'
            assert (generated / 'placement.h').is_file() and (generated / 'recognition-gpu.cc').is_file(), name
        results[name] = 'refused' if refusal else 'configured'
        shutil.rmtree(directory)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--models', type=Path, default=Path.home() / '.cache/sunstoneos/inputs/mimus-bench',
                        help='directory with base.mimus and overlay.mimus')
    parser.add_argument('--configure-from', type=Path, help='a configured OS build whose cache gives toolchain paths')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    models = [args.models / 'base.mimus', args.models / 'overlay.mimus']
    result = {'passed': True, 'physical_access': False, 'recognizer_patch': recognizer_order(out),
              'zero_bound': zero_bound()}
    for cores in ('a55', 'a76'):
        result[cores] = run_model(out, cores, models, BENCH_FRAMES)
    result['lookup'] = lookup_instructions(out)
    if args.configure_from:
        result['configure'] = configure_cases(out, args.configure_from, models)
    sources = ['apps/os/dispatch.c', 'apps/os/dispatch.h', 'apps/os/placement.py', 'apps/os/recognition-dispatch.patch',
               'checks/os-image-dispatch.c', 'checks/os-image-dispatch.py']
    result['sources'] = {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    for cores in ('a55', 'a76'):
        s, b = result[cores]['summary'], result[cores]['bench']
        print('PASS %s pair: %d calls placed, %d jobs, linear stage %.3f s modeled (design bound %.2f s), '
              '%.1f%% of MACs on the GPU; longest input %d jobs at most per call' %
              (cores.upper(), b['placed_calls'], b['jobs'], s['linear_seconds'], DESIGN_BOUND[cores],
               100 * s['gpu_share'], result[cores]['longest']['max_jobs_per_call']))
    print('PASS dispatcher cases, exact sums, supervision, lookup %d loads and %d compare%s' %
          (result['lookup']['loads'], result['lookup']['compares'],
           ', configure gate' if args.configure_from else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
