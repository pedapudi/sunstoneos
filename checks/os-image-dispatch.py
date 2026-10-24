#!/usr/bin/env python3
"""Check the OS image's placement of mimus linear layers on the CPU or GPU against its design budgets.

The check covers docs/recognition-dispatch.md on the host; no device is used:

- apps/os/placement.py emits the placement table for the A55 pair at 494 MHz
  and the A76 pair at 2,002 MHz from the bench models; the table and the
  call and job counts equal the values docs/designs/gpu-recognition.md
  records, and the modeled linear-stage time is within its regression bound;
- the header's OPEN admission list has one entry per placed row, in ordinal
  order, whose weight extent lies inside its model file and its 8 MiB GPU
  region; a C program compiled against apps/os/dispatch.h reads the entries
  back with the values the time model computed;
- every row's job_rows is at most the GPU worker's own bound, computed here
  with the GPU kernel design's formula;
- a placement bound that rounds to zero rows gives n_gpu = 0;
- checks/os-image-dispatch.c, built with AddressSanitizer and
  UndefinedBehaviorSanitizer, runs apps/os/dispatch.c over the bench call
  list with exact integer sums compared against a CPU-only reference, and
  over the longest accepted input (the 1 MiB WAV slot), where both tables
  split a call into several jobs, as many as the time model splits it into. It covers no OPEN before the model
  hashes, a refused OPEN, a refused job, a job with a missing tag, RESET and
  LOST, a reply that never comes, two jobs that end at the worker's deadline,
  and the read-only mapping rule with one writable, unreadable or Device page
  among 1,024;
- the recognizer patch applies with and without the capture patch and opens
  the session only after every model hash has matched;
- the patch's arena assertion compiles for a 12 MiB mimus arena at
  0x46000000 and refuses a larger arena or another base; the result records
  whether apps/mimus/recorded.h already fits;
- os_dispatch_place compiles for AArch64 to two loads and one compare;
- with --configure-from, CMake refuses the option in the supervised
  lifetime and with a user-supplied A733_GPU_OUTPUT_EL0_READ_ONLY, which it
  sets itself; it configures the resident build with and without the
  microphone.
"""
import argparse
import hashlib
import importlib.util
import json
import math
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

# Emitted tables by shape (K x N: n_gpu, job_rows) for the bench recording,
# as docs/designs/gpu-recognition.md records them, from the measured GPU and
# read-out rates.
EXPECTED = {
    'a55': {'1584x176': (160, 256), '3520x176': (160, 96), '176x704': (624, 416), '704x176': (160, 576),
            '176x176': (160, 1632), '176x352': (304, 832), '176x1025': (897, 256)},
    'a76': {'1584x176': (80, 512), '3520x176': (80, 224), '176x704': (320, 800), '704x176': (80, 1184),
            '176x176': (80, 3264), '176x352': (160, 1632), '176x1025': (449, 544)},
}
# The design's regression bound on the modeled linear stage, in seconds.
DESIGN_BOUND = {'a55': 1.12, 'a76': 0.54}
BENCH = {'placed_calls': 204, 'jobs': 204, 'block_jobs': 20, 'block_padded_rows': 8,
         'block_codes': 7032960, 'block_clean_lines': 219780, 'outputs': 12923134}
BENCH_FRAMES = 222
# The recorded WAV slot holds at most 1 MiB (apps/mimus-board/configure.py):
# a 44-byte header and 524,266 samples at 16 kHz, 640 samples per encoder frame.
LONGEST_FRAMES = -(-((1048576 - 44) // 2) // 640)
COMPILE = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
           '-fno-sanitize-recover=all', '-Iinclude', '-Iplatform/a733-smp-cached', '-Iapps/os',
           '-DSUNSTONE_BOARD_SUPERVISED=1', '-DSUNSTONE_BOARD_USB2_DUAL=1', '-DSUNSTONE_BOARD_RESIDENT=1',
           '-DSUNSTONE_A733_OS_GPU_LINEAR=1']


def replay_sizes():
    """Allocation sizes for the C model's pool replay, from headers that the
    host compiler cannot include (they include FreeRTOS)."""
    def find(path, pattern):
        return int(re.search(pattern, (ROOT / path).read_text()).group(1), 0)
    return ['-DREPLAY_FIRMWARE_WB_PAGES=%d' % find('include/a733_gpu_reserved.h', r'A733_GPU_RESERVED_WB_BYTES \((\d+)u \*'),
            '-DREPLAY_FIRMWARE_NC_PAGES=%d' % find('include/a733_gpu_reserved.h', r'A733_GPU_RESERVED_NC_BYTES \((\d+)u \*'),
            '-DREPLAY_SESSION_WB_PAGES=%d' % find('include/a733_gpu_mimus.h', r'A733_GPU_MIMUS_PAYLOAD_PAGES (\d+)u'),
            '-DREPLAY_SESSION_NC_PAGES=%d' % find('include/pvr_mimus_service.h', r'PVR_MIMUS_SERVICE_OBJECTS (\d+)u'),
            '-DREPLAY_XHCI_BYTES=%d' % find('include/a733_xhci.h', r'A733_XHCI_MEMORY_BYTES (\d+)u')]


CACHE_ENTRIES = ('FREERTOS_DIR', 'TINYUSB_DIR', 'SUNSTONE_LLD', 'SUNSTONE_NETWORK_C_HEADERS', 'SUNSTONE_RSN_DIR',
                 'SUNSTONE_TCP_DIR', 'SUNSTONE_BOARD_SUPERVISED', 'SUNSTONE_GPU_FIRMWARE', 'MIMUS_DIR',
                 'NEWLIB_ROOT', 'NEWLIB_SMP_ROOT', 'SUNSTONE_BT_SECURITY_DIR', 'SUNSTONE_MIMUS_BOARD_APPLICATION')


def patch(source, patch_file, output):
    subprocess.run(['patch', '-s', '--fuzz=0', '--reject-file=-', '--output', str(output), str(source),
                    str(patch_file)], check=True)
    return output.read_text()


def recognizer_order(out):
    """The patch applies to both recognizer sources, opens the session (through
    apps/os/gpu_hook.cc) after the hashes and records the recognizer task."""
    base = ROOT / 'apps/mimus-board/recognition.cc'
    capture = out / 'recognition-capture.cc'
    patch(base, ROOT / 'apps/os/recognition.patch', capture)
    for name, source in (('recorded', base), ('capture', capture)):
        text = patch(source, ROOT / 'apps/os/recognition-dispatch.patch', out / ('recognition-gpu-%s.cc' % name))
        hashed = text.index('if(std::memcmp(hash,mimus_asset_sha256[i],32)){failed();}')
        opened = text.index('os_gpu_open();')
        assert text.count('os_gpu_open();') == 1 and hashed < opened < text.index('mimus_transcribe_recorded'), name
        assert 'os_recognizer=xTaskCreateStaticAffinitySet(recognize' in text and \
            'void os_dispatch_stop(void){failed();}' in text
    return ['recorded', 'capture']


def worker_job_rows(k, n_gpu):
    """The GPU worker's bound at OPEN, as the GPU kernel design states it."""
    width = 4 if k <= 512 else 8
    return min((1 << 26) // (k * n_gpu), ((2 << 20) - (64 << 10)) // (n_gpu * width),
               32 * (512 // -(-n_gpu // 16)))


def arena_bound(out):
    """The recognizer patch's assertion that mimus's arena ends where the GPU
    output region begins, compiled with three arenas."""
    text = (out / 'recognition-gpu-recorded.cc').read_text()
    start = text.index('static_assert(kMimusArenaBase')
    assertion = text[start:text.index(';', start) + 1]
    source = out / 'arena.cc'
    results = {}
    for name, base, size in (('12 MiB', 0x46000000, 12 << 20), ('16 MiB', 0x46000000, 16 << 20),
                             ('moved base', 0x46001000, 12 << 20)):
        source.write_text('#include <cstddef>\n#include <cstdint>\nextern "C" {\n#include "dispatch.h"\n}\n'
                          'inline constexpr uintptr_t kMimusArenaBase = %#x;\n'
                          'inline constexpr size_t kMimusArenaBytes = %d;\n%s\n' % (base, size, assertion))
        result = subprocess.run(['clang++', '-std=c++20', '-fsyntax-only', '-Iinclude', '-Iapps/os', str(source)], cwd=ROOT,
                                capture_output=True, text=True)
        results[name] = result.returncode == 0
    assert results == {'12 MiB': True, '16 MiB': False, 'moved base': False}, results
    header = (ROOT / 'apps/mimus/recorded.h').read_text()
    size = re.search(r'kMimusArenaBytes = ([0-9 *]+);', header).group(1)
    arena = math.prod(int(factor) for factor in size.split('*'))
    return {'compiles': results, 'recorded_arena_bytes': arena, 'recorded_arena_fits': arena <= 12 << 20}


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
    assert summary['linear_seconds'] <= DESIGN_BOUND[cores], (cores, summary['linear_seconds'])
    for t, (n_gpu, rows) in zip(tensors, table):
        assert not n_gpu or 32 <= rows <= worker_job_rows(t['k'], n_gpu), (t['name'], n_gpu, rows)
    core = placement.CORES[cores]['pair'][0]
    admitted = admission_list(directory, tensors, table, models, core)
    binary = directory / 'dispatch'
    subprocess.run(['clang'] + COMPILE + replay_sizes() + ['-I' + str(directory), '-DOS_RECOGNITION_CORE=%d' % core,
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
    # The longest input's calls split into several jobs (the output head's
    # 820 rows into four on the A55 pair, the feed-forward first linear's into
    # two on the A76 pair), exactly as the time model splits them.
    expected_jobs = max(len(placement.split(r, k, table[o][0])) for o, r, k, _, _, _ in
                        placement.call_list(tensors, LONGEST_FRAMES) if table[o][0])
    assert expected_jobs >= 2 and results[LONGEST_FRAMES]['max_jobs_per_call'] == expected_jobs, \
        (results[LONGEST_FRAMES], expected_jobs)
    return {'summary': summary, 'bench': bench, 'longest': results[LONGEST_FRAMES],
            'admission': admitted, 'design_bound_s': DESIGN_BOUND[cores],
            'placement_h_sha256': hashlib.sha256((directory / 'placement.h').read_bytes()).hexdigest()}


def admission_list(directory, tensors, table, models, core):
    """The OPEN admission entries: one per placed row in ordinal order, each
    weight extent inside its model file and its 8 MiB GPU region, read back
    through apps/os/dispatch.h's struct."""
    entries = placement.gpu_tensors(tensors, table)
    placed = [i for i, (n, _) in enumerate(table) if n]
    assert [e[1] for e in entries] == placed, 'admission list differs from the placed rows'
    sizes = [m.stat().st_size for m in models]
    for offset, ordinal, stride, k, n, region in entries:
        t, n_gpu = tensors[ordinal], table[ordinal][0]
        end = offset + (n_gpu - 1) * stride + k
        assert region in (0, 1) and k == t['k'] and n == t['n'] and n_gpu <= n and stride == t['row_stride'] and \
            k % 4 == 0 and \
            stride % 4 == 0 and k <= stride < 65536 and end <= sizes[region] and end <= 8 << 20, t['name']
    source = directory / 'admission.c'
    source.write_text('#include <stdio.h>\n#include "dispatch.h"\n#include "placement.h"\nint main(void){\n'
                      '    for(unsigned i=0;i<OS_PLACEMENT_GPU_TENSORS;++i){const struct os_placement_tensor *t='
                      '&os_placement_tensors[i];\n'
                      '        printf("%u %u %u %u %u %u\\n",(unsigned)t->offset,(unsigned)t->ordinal,(unsigned)t->stride,'
                      '(unsigned)t->k,(unsigned)t->n,(unsigned)t->region);}\n    return 0;\n}\n')
    binary = directory / 'admission'
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Iinclude', '-Iapps/os', '-I' + str(directory),
                    '-DOS_RECOGNITION_CORE=%d' % core, str(source), '-o', str(binary)], cwd=ROOT, check=True)
    lines = subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout.split('\n')
    read = [tuple(int(v) for v in line.split()) for line in lines if line]
    assert read == entries, 'the C struct reads other values than the time model wrote'
    return {'entries': len(entries), 'regions': [sum(1 for e in entries if e[5] == r) for r in (0, 1)],
            'largest_end': max(e[0] + (table[e[1]][0] - 1) * e[2] + e[3] for e in entries)}


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
    model_dir = '-DSUNSTONE_A733_OS_MODEL_DIR=' + str(models[0].parent)
    # CMake sets the read-only view's constant itself.
    cases = [
        ('supervised', ['-DSUNSTONE_A733_OS_RESIDENT=OFF', model_dir], 'resident lifetime'),
        ('user-read-only', ['-DSUNSTONE_A733_OS_RESIDENT=ON', '-DA733_GPU_OUTPUT_EL0_READ_ONLY=ON',
                            model_dir], 'A733_GPU_OUTPUT_EL0_READ_ONLY'),
        ('resident', ['-DSUNSTONE_A733_OS_RESIDENT=ON', model_dir], None),
        ('resident-audio', ['-DSUNSTONE_A733_OS_RESIDENT=ON', '-DSUNSTONE_A733_OS_AUDIO=ON', model_dir], None),
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
    result['arena'] = arena_bound(out)
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
        print('PASS %s pair: %d calls placed, %d jobs, linear stage %.3f s modeled (bound %.2f s), '
              '%.1f%% of MACs on the GPU; longest input %d jobs at most per call' %
              (cores.upper(), b['placed_calls'], b['jobs'], s['linear_seconds'], DESIGN_BOUND[cores],
               100 * s['gpu_share'], result[cores]['longest']['max_jobs_per_call']))
    print('PASS dispatcher cases, exact sums, supervision, lookup %d loads and %d compare%s' %
          (result['lookup']['loads'], result['lookup']['compares'],
           ', configure gate' if args.configure_from else ''))
    print('PASS arena assertion; the mimus arena in apps/mimus/recorded.h is %d bytes, %s' %
          (result['arena']['recorded_arena_bytes'], 'within the 12 MiB span' if result['arena']['recorded_arena_fits']
           else 'above the 12 MiB span, so the GPU build stops at the assertion until the mimus port shrinks it'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
