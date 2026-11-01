#!/usr/bin/env python3
"""Run the GPU compute queue and native memory path in two-core FreeRTOS QEMU.

A synthetic linear-grid fixture checks buffer bindings, exact sums, client-ring
commands, interrupts, deadlines, refusal and retained lifecycle. --fixture accepts
an external workload's tensor and job JSON for replay, including the optional
mimus bench recording. Shader execution, register effects and time are modeled;
no hardware is accessed.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
from harness import environment
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CASES = {1: 'grid replay, exact sums', 2: 'grid replay, pattern sums', 3: 'grid replay, jobs complete at once',
         4: 'as 3 with one firmware event per job', 5: 'refused jobs', 6: 'coalesced wake and reserved request capacity',
         7: 'missing tag', 8: 'hang', 9: 'context reset', 10: 'OPEN only',
         11: 'unexpected timeline before a submit', 12: 'completion clean and invalidate fails',
         13: 'completion at deadline', 14: 'output acquisition reaches deadline',
         15: 'completion just before deadline', 16: 'failed final clock read',
         17: 'backward final clock sample', 18: 'pending, active and drained CLOSE',
         19: 'reserved-slot CLOSE and abort', 20: 'lost CLOSE',
         21: 'two healthy retained reopens', 22: 'reopen proof refusals', 23: 'reopen cache failure',
         24: 'creation result publication failure', 25: 'performance transition and reopen', 26: 'accepted job drains before forced idle',
         27: 'forced idle acknowledgement absent', 28: 'cancel acknowledgement absent', 29: 'clean drain refusal', 30: 'firmware reset while performance HOLD drains a job', 31: 'final PARK',
         32: 'thermal close and reopen before final PARK', 33: 'final PARK callback failure',
         34: 'unleased neighbour in GPU output rounded tail',
         35: 'unleased neighbour in model rounded tail',
         36: 'unleased neighbour in CPU-written input rounded tail',
         37: 'unleased neighbour at exact rounded boundary',
         38: 'worker immutable program source bounds and final-word creation',
         39: 'ten public close/open cycles', 40: 'two caller slots and independent sequences',
         41: 'timeout reset resumes queued peer', 42: 'decoded reset resumes queued peer',
         43: 'queued peer kick deadline survives reset', 44: 'idle refusal retains both callers',
         45: 'failed reset read retains both callers', 46: 'failed isolation retains both callers',
         47: 'failed restart retains queued peer', 48: 'failed restart health retains queued peer',
         49: 'session limit clips recovery deadlines',
         50: 'maximum job duration recovers an unconsumed kernel kick',
         51: 'firmware timeline serial exhaustion refuses without a kick',
         52: 'qualification timeout refuses recovery without a queued peer',
         53: 'direct interrupt fences, coalescing and notification races',
         54: 'EL0 caller submission and ISR collection without the service worker',
         55: 'four-job batches, output ownership and out-of-order fences',
         56: 'lost completion IRQ recovers without polling and preserves a contended fault wake',
         57: 'caller timeout retains payload and conflicting output ownership',
         58: 'permanent timeline fault wakes the caller and retains buffers',
         59: 'two clients pipeline independent sequences and direct notification waiters',
         60: 'forged fence under lock contention refuses after an IRQ-free unlock',
         61: 'uncertain batch doorbell returns every retained fence in its accepted prefix',
         62: 'uncached retained payloads preserve the timeline and application maintenance',
         63: 'admitted KCCB publication wraps without provider validation',
         64: 'admitted KCCB rejects a jumping firmware cursor and retains its origin',
         65: 'admitted KCCB preserves full-ring and reserved-slot backpressure',
         66: 'admitted KCCB view is revoked on close and restored after fresh READY',
         67: 'uncertain admitted doorbell preserves the accepted batch prefix',
         68: 'idle direct queue preserves the forced-idle hold deadline'}
COUNTED = ('aarch64_device_translate', 'aarch64_device_cache', 'vTaskEnterCritical', 'vTaskEnterCriticalFromISR',
           'xQueueGenericSend', 'xQueueReceive', 'xQueueGenericSendFromISR', 'gicv3_spi_mask', 'gicv3_spi_rearm',
           'advance', 'external', 'now', 'gpu_queue_submit', 'gpu_queue_wait', 'kick',
           'fixture_gpu_irq', 'post', 'maintain', 'fixture_mark', 'irq_read',
           'a733_gpu_memory_read', 'a733_gpu_memory_write', 'a733_gpu_memory_barrier', 'pvr_cccb_poll',
           'pvr_ccb_submit', 'pvr_ccb_receive', 'pvr_ccb_poll', 'step', 'irq_write', 'read_reg', 'write_reg',
           'ring_write', 'drive_to_kick')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def fixture(source, out):
    if source:
        data = json.loads(source.read_text())
    else:
        data = {'tensors': [dict(offset=0, ordinal=0, stride=64, k=64, n_gpu=32, job_rows=32, region=0),
                            dict(offset=4096, ordinal=1, stride=1024, k=1024, n_gpu=16, job_rows=32, region=1)],
                'jobs': [dict(ordinal=i % 2, rows=(i % 3 + 1), k=64 if i % 2 == 0 else 1024,
                              n_gpu=32 if i % 2 == 0 else 16, duration_us=80) for i in range(8)]}
    tensors, jobs = data['tensors'], data['jobs']
    assert tensors and len(jobs) >= 3, 'fixture needs tensors and at least three jobs'
    tensor_fields = ('offset', 'ordinal', 'stride', 'k', 'n_gpu', 'job_rows', 'region')
    job_fields = ('ordinal', 'rows', 'k', 'n_gpu', 'duration_us')
    assert len({t['ordinal'] for t in tensors}) == len(tensors)
    for t in tensors:
        assert all(isinstance(t[k], int) and t[k] >= 0 for k in tensor_fields)
        assert t['region'] < 2 and t['k'] and t['n_gpu'] and t['stride'] >= t['k']
        assert t['offset'] + (t['n_gpu'] - 1) * t['stride'] + t['k'] <= 8 << 20
    by_ordinal = {t['ordinal']: t for t in tensors}
    for j in jobs:
        assert isinstance(j['duration_us'],(int,float)) and j['duration_us']>=0
        j['duration_us']=round(j['duration_us'])
        assert all(isinstance(j[k], int) and j[k] >= 0 for k in job_fields)
        t = by_ordinal[j['ordinal']]
        assert 0 < j['rows'] <= t['job_rows'] and j['k'] == t['k'] and j['n_gpu'] == t['n_gpu']
    lines = ['/* Synthetic grid workload or an externally supplied workload replay. */',
             'struct fixture_tensor { uint32_t offset; uint16_t ordinal, stride, k, n_gpu, job_rows; uint8_t region; };',
             'struct fixture_job { uint16_t ordinal, rows, k, n_gpu; uint32_t duration_us; };',
             '#define FIXTURE_TENSORS %du' % (max(by_ordinal) + 1),
             '#define FIXTURE_GPU_TENSORS %du' % len(tensors), '#define FIXTURE_JOBS %du' % len(jobs),
             'static const struct fixture_tensor fixture_tensors[FIXTURE_GPU_TENSORS]={']
    lines += ['    {' + ','.join(str(t[k]) for k in tensor_fields) + '},' for t in tensors]
    lines += ['};', 'static const struct fixture_job fixture_jobs[FIXTURE_JOBS]={']
    lines += ['    {' + ','.join(str(j[k]) for k in job_fields) + '},' for j in jobs]
    lines.append('};')
    (out / 'queue-fixture.h').write_text('\n'.join(lines) + '\n')
    (out / 'workload.json').write_text(json.dumps(data, indent=2) + '\n')
    return [j['duration_us'] for j in jobs]


def symbols(elf):
    text = subprocess.check_output(['llvm-nm-21', '-n', '-S', str(elf)], text=True)
    funcs = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in 'tTwW':
            funcs.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
    return funcs


def trace_filter(elf, funcs):
    dsb = []
    stores=collections.defaultdict(list)
    ring=[(a,a+n) for a,n,name in funcs if name=='ring_write']
    for line in subprocess.check_output(['llvm-objdump-21', '-d', '--no-show-raw-insn', str(elf)],
                                        text=True).splitlines():
        m = re.match(r'\s*([0-9a-f]+):\s+(.*)', line)
        if not m:continue
        address,op=int(m.group(1),16),m.group(2)
        if re.match(r'dsb\s+sy',op):dsb.append(address)
        if any(first<=address<end for first,end in ring):
            if re.match(r'str\s+x\d+, \[x(?!29|31)\d+',op):stores['uncached-store64'].append(address)
            if re.match(r'strb\s+w\d+, \[x(?!29|31)\d+',op):stores['uncached-store8'].append(address)
    entries = {}
    for address, _size, name in funcs:
        if name in COUNTED:
            entries.setdefault(name, []).append(address)
    entries.update(stores)
    return dsb, entries


def owner(funcs, address):
    for start, size, name in funcs:
        if start <= address < start + size:
            return name
    return '?'


def parse(log, dsb, entries, funcs):
    """Core-0 events in order: ('dsb', function) or ('call', name)."""
    entry_of = {a: n for n, addresses in entries.items() for a in addresses}
    dsb_set = set(dsb)
    names = {a: owner(funcs, a) for a in dsb}
    events = []
    for line in log.read_text(errors='replace').splitlines():
        m = re.match(r'Trace (\d+): 0x[0-9a-f]+ \[[0-9a-f]+/([0-9a-f]+)/', line)
        if not m or m.group(1) != '0':
            continue
        pc = int(m.group(2), 16)
        if pc in entry_of:
            events.append(('call', entry_of[pc]))
        if pc in dsb_set:
            events.append(('dsb', names[pc]))
    return events


def windows(events):
    """Split at fixture_mark calls: OPEN before the first, one window per job."""
    jobs, current, started = [], None, False
    for kind, name in events:
        if kind == 'call' and name == 'fixture_mark':
            if current is not None:
                jobs.append(current)
            current = collections.Counter()
            started = True
            continue
        if started and current is not None:
            current[(kind, name)] += 1
    return jobs


SOURCE = {'aarch64_device_cache': 'maintenance', 'gicv3_spi_mask': 'interrupt mask/rearm',
          'gicv3_spi_rearm': 'interrupt mask/rearm', 'a733_gpu_memory_read': 'gpu_memory.c',
          'a733_gpu_memory_write': 'gpu_memory.c', 'a733_gpu_memory_barrier': 'gpu_memory.c',
          'ring_read': 'client ring', 'ring_barrier': 'client ring', 'pvr_compute_queue_prepare': 'session',
          'publish': 'model', 'model': 'model'}
# FreeRTOS kernel and SMP port: critical sections, the kernel lock (with its
# spin), interrupt masking, and the assembly exception entries ('?').
PORT = ('vPortRecursiveLock', 'vPortClearInterruptMask', 'uxPortSetInterruptMask', 'vTaskExitCritical',
        'vTaskEnterCritical', 'uxPortSetInterruptMaskFromISR', 'vPortClearInterruptMaskFromISR',
        'xTaskGetSchedulerState', 'xTaskCheckForTimeOut', 'xTaskResumeAll', '?')


def summarize(job_windows, segment):
    """Mean per job of every counter, the DSBs by source, and the slot-to-kick window."""
    total = collections.Counter()
    for w in job_windows:
        total.update(w)
    n = len(job_windows)
    per = {('%s:%s' % k): round(v / n, 3) for k, v in sorted(total.items())}
    by_source = collections.Counter()
    for (kind, name), v in total.items():
        if kind == 'dsb':
            by_source['FreeRTOS kernel and port' if name in PORT else SOURCE.get(name, name)] += v
    return {'jobs': n, 'per_job': per, 'dsb_per_job': round(sum(v for (k, _), v in total.items() if k == 'dsb') / n, 3),
            'dsb_by_source_per_job': {k: round(v / n, 3) for k, v in by_source.most_common()}, **segment}


def segments(events):
    """Per job: from gpu_queue_submit to the kick, and from the GPU
    interrupt to the native completion post, counting DSBs, translations,
    distributor accesses (mask/rearm), critical sections and advance() calls."""
    out = collections.defaultdict(list)
    state, counter = None, None
    for kind, name in events:
        if kind == 'call' and name == 'gpu_queue_submit':
            state, counter = 'kick', collections.Counter()
            continue
        if kind == 'call' and name == 'fixture_gpu_irq':
            state, counter = 'post', collections.Counter()
        if state is None:
            continue
        if (state == 'kick' and kind == 'call' and name == 'kick') or \
           (state == 'post' and kind == 'call' and name == 'post'):
            out[state].append(counter)
            state = None
            continue
        counter['dsb' if kind == 'dsb' else name] += 1
    result = {}
    for state, items in out.items():
        keys = sorted(set().union(*items)) if items else []
        result['%s_window' % state] = {'samples': len(items),
                                      **{k: round(statistics.mean(i[k] for i in items), 3) for k in keys}}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, help='External generic tensor/job JSON; default is an eight-job synthetic grid')
    parser.add_argument('--cases', nargs='*', type=int, choices=sorted(CASES), default=list(range(1, 25)))
    parser.add_argument('--trace', nargs='*', type=int, default=[], help='cases to run instrumented')
    parser.add_argument('--latency', action='store_true', help='Enable latency hooks and check client receipt samples')
    parser.add_argument('--timing', action='store_true', help='compile optional GPU IRQ counter capture')
    for name in ('freertos', 'tinyusb', 'firmware', 'qemu', 'lld'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    performance = any(25 <= c <= 30 or c==68 for c in args.cases)
    returning = any(31 <= c <= 33 for c in args.cases)
    recovery = any(41 <= c <= 49 or c==52 for c in args.cases)
    assert not recovery or all(41 <= c <= 49 or c==52 for c in args.cases) and not args.trace
    assert not returning or all(31 <= c <= 33 for c in args.cases) and not args.trace
    assert not performance or all(25 <= c <= 30 or c==68 for c in args.cases) and not args.trace
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    project, build, fixtures = out / 'project', out / 'build', out / 'fixtures'
    for d in (project, fixtures):
        d.mkdir(exist_ok=True)
    shutil.copy2(args.firmware, fixtures / 'gpu.fw')
    assert sha(fixtures / 'gpu.fw') == '1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2'
    durations = fixture(args.fixture, fixtures)
    native = (ROOT / 'platform/a733/link.ld').read_text()
    reservations = native[native.index('    /* An absent pool'):native.index('    /DISCARD/')]
    linker = project / 'link.ld'
    linker.write_text((ROOT / 'platform/qemu-smp/link.ld').read_text().replace(
        '    /DISCARD/', '    __heap_end = .;\n' + reservations + '    /DISCARD/'))
    text = (ROOT / 'platform/qemu-smp/platform.c').read_text()
    for old, new in (('#include "platform.h"', '#include "platform.h"\n#include "a733_memory_boot.h"\nvoid fixture_gpu_irq(void);'),
                     ('uint64_t mair = 0xff00', 'uint64_t mair = 0x44ff00'),
                     ('    // Code is read-only', '    configASSERT(a733_memory_boot_uncached(level2));\n    // Code is read-only'),
                     ('    else if (interrupt < 1020)', '    else if (interrupt == 95) fixture_gpu_irq();\n    else if (interrupt < 1020)'),
                     # The instrumented runs execute core 0's preparation many times slower.
                     ('    uint64_t end = counter() + frequency();\n    while (!__atomic_load_n(&ucPrimaryCoreInitDoneFlag',
                      '    uint64_t end = counter() + 600 * frequency();\n    while (!__atomic_load_n(&ucPrimaryCoreInitDoneFlag')):
        assert text.count(old) == 1, old
        text = text.replace(old, new)
    platform = project / 'platform.c'
    platform.write_text(text)
    extra = ' '.join('"%s"' % (ROOT / p) for p in (
        'checks/gpu-queue.c', 'platform/a733/gpu_memory.c', 'platform/a733/gpu_reserved.c',
        'platform/a733/gpu_queue.c', 'drivers/gpu/pvr_compute_queue.c', 'drivers/gpu/pvr_compute_fast.c',
        'drivers/gpu/gpu_queue.c', 'runtime/gpu_call.S', 'drivers/gpu/gicv3_spi.c'))
    if performance:
        extra += ' \"%s\"' % (ROOT / 'drivers/gpu/pvr_gpu_clock.c')
    if args.latency:
        extra += ' "' + str(ROOT / 'runtime/latency.c') + '"'
    (project / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.24)
project(gpu_queue_check LANGUAGES C ASM)
add_subdirectory("{ROOT}" sunstone)
get_target_property(sources gpu-service-model SOURCES)
list(FILTER sources EXCLUDE REGEX "checks/pvr-gpu-service.c$|platform/qemu-smp/platform.c$")
list(APPEND sources "{platform}" {extra})
list(REMOVE_DUPLICATES sources)
set_property(TARGET gpu-service-model PROPERTY SOURCES "${{sources}}")
target_include_directories(gpu-service-model PRIVATE "{fixtures}" "{ROOT}/checks")
target_compile_definitions(gpu-service-model PRIVATE SUNSTONE_GPU_NATIVE_MEMORY=1 {"SUNSTONE_A733_OS_PERF=1" if performance else "SUNSTONE_A733_OS_CPU_RETURN=1" if returning else "SUNSTONE_A733_OS_GPU_RECOVERY_TEST=1" if recovery else ""})
{'target_compile_definitions(gpu-service-model PRIVATE SUNSTONE_LATENCY=1)' if args.latency else ''}
target_compile_definitions(gpu-service-model PRIVATE INCLUDE_vTaskPrioritySet=1)
target_compile_definitions(gpu-service-model PRIVATE {"SUNSTONE_GPU_FAST_TIMING=1" if args.timing else ""})
get_target_property(options gpu-service-model LINK_OPTIONS)
list(FILTER options EXCLUDE REGEX "-Wl,-T,")
list(APPEND options "-Wl,-T,{linker}" -Wl,--defsym=__device_pool_size=2097152
  -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--wrap=aarch64_device_cache -Wl,--wrap=aarch64_device_translate
  -Wl,--wrap=pvr_cccb_poll
  -Wl,--wrap=pvr_gpu_service_now -Wl,--wrap=pvr_compute_queue_poll -Wl,--wrap=a733_gpu_backend_stopped
  -Wl,--wrap=xQueueGenericSend)
list(APPEND options -Wl,--wrap=pvr_compute_fast_collect -Wl,--wrap=pvr_compute_fast_unlock)
set_property(TARGET gpu-service-model PROPERTY LINK_OPTIONS "${{options}}")
set_property(TARGET gpu-service-model PROPERTY LINK_DEPENDS "{linker}")
''')
    env = environment()
    env['PATH'] = str(args.lld.resolve().parent) + os.pathsep + env.get('PATH', '')
    commands_path=out/'commands.json'
    commands=json.loads(commands_path.read_text()) if commands_path.exists() else []
    results={'cases': {}, 'traces': {}}
    if (out/'results.json').exists():
        attempts=list(out.glob('results-attempt*.json'))
        shutil.copy2(out/'results.json',out/('results-attempt%d.json' % (len(attempts)+1)))
    logs={}

    def run(name, command, timeout=None):
        commands.append(command)
        r = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout, env=env)
        log=out/(name+'.log')
        if log.exists():
            attempt=1
            while (out/('%s-attempt%d.log' % (name,attempt))).exists(): attempt+=1
            log=out/('%s-attempt%d.log' % (name,attempt))
        log.write_text(r.stdout)
        logs[name]=str(log.resolve())
        if r.returncode:
            raise RuntimeError('%s failed (%d): %s' % (name, r.returncode, r.stdout[-4000:]))
        return r.stdout
    try:
        run('configure', ['cmake', '-S', str(project), '-B', str(build),
            '-DCMAKE_TOOLCHAIN_FILE=' + str(ROOT / 'cmake/aarch64-clang.cmake'),
            '-DFREERTOS_DIR=' + str(args.freertos.resolve()), '-DTINYUSB_DIR=' + str(args.tinyusb.resolve()),
            '-DSUNSTONE_GPU_FIRMWARE=' + str(fixtures / 'gpu.fw'), '-DSUNSTONE_LLD=' + str(args.lld.absolute()),
            '-DCMAKE_EXE_LINKER_FLAGS=--ld-path=' + str(args.lld.absolute())])
        run('build', ['nice', '-n', '10', 'cmake', '--build', str(build), '--target', 'gpu-service-model', '-j6'])
        binary = out / 'gpu-queue.elf'
        shutil.copy2(build / 'sunstone/gpu-service-model.elf', binary)
        funcs = symbols(binary)
        data = binary.read_bytes()
        nm = subprocess.check_output(['llvm-nm-21', '-S', str(binary)], text=True)
        address = [int(l.split()[0], 16) for l in nm.splitlines() if l.split()[-1] == 'gpu_queue_case']
        slow = [int(l.split()[0], 16) for l in nm.splitlines() if l.split()[-1] == 'gpu_queue_slow']
        assert len(address) == 1 and len(slow) == 1
        phdrs = subprocess.check_output(['llvm-readelf-21', '-lW', str(binary)], text=True)
        offset = None
        for line in phdrs.splitlines():
            p = line.split()
            if p and p[0] == 'LOAD':
                off, va, filesz = int(p[1], 16), int(p[2], 16), int(p[4], 16)
                if va <= address[0] < va + filesz:
                    offset = off + address[0] - va
        assert offset is not None and int.from_bytes(data[offset:offset + 4], 'little') == 1
        slow_offset = offset + slow[0] - address[0]
        assert int.from_bytes(data[slow_offset:slow_offset + 4], 'little') == 0
        dsb, entries = trace_filter(binary, funcs)
        results.update(elf_sha256=sha(binary), dsb_sites=len(dsb),
                       counted_entries={k: len(v) for k, v in entries.items()},
                       modeled_job_us={'total': sum(durations), 'median': statistics.median(durations),
                                       'max': max(durations)})
        qemu = [str(args.qemu.resolve()), '-machine', 'virt,gic-version=3,secure=off,virtualization=off',
                '-cpu', 'cortex-a55', '-smp', '2', '-m', '128M', '-nographic', '-monitor', 'none',
                '-semihosting-config', 'enable=on,target=native']
        for case in sorted(set(args.cases) | set(args.trace)):
            image = bytearray(data)
            image[offset:offset + 4] = case.to_bytes(4, 'little')
            path = out / ('case-%d.elf' % case)
            path.write_bytes(image)
            if case in args.cases:
                log = run('case-%d' % case, qemu + ['-kernel', str(path)], timeout=15 if case>=53 else 1800)
                marker = 'PASS GPU queue case 0x%016x;' % case
                assert marker in log, log[-3000:]
                results['cases'][case] = {'name': CASES[case], 'metrics': dict(
                    (m.group(1), int(m.group(2), 16)) for m in re.finditer(r'metric (\S+) (0x[0-9a-f]+)', log)),
                    'pass': log.strip().splitlines()[-1], 'log': logs['case-%d' % case]}
            if case in args.trace:
                image[slow_offset:slow_offset + 4] = (1).to_bytes(4, 'little')
                path = out / ('case-%d-traced.elf' % case)
                path.write_bytes(image)
                trace = out / ('trace-%d.log' % case)
                ranges = ','.join('0x%x+4' % a for a in sorted(set(dsb) | {a for v in entries.values() for a in v}))
                log = run('traced-%d' % case, qemu[:2] + ['virt,gic-version=3,secure=off,virtualization=off'] + qemu[3:] +
                          ['-accel', 'tcg,thread=single,one-insn-per-tb=on', '-icount', 'shift=0,sleep=off',
                           '-d', 'exec,nochain',
                           '-dfilter', ranges, '-D', str(trace), '-kernel', str(path)], timeout=3600)
                assert 'PASS GPU queue case 0x%016x;' % case in log, log[-3000:]
                events = parse(trace, dsb, entries, funcs)
                all_windows = windows(events)
                metrics = dict((m.group(1), int(m.group(2), 16)) for m in re.finditer(r'metric (\S+) (0x[0-9a-f]+)', log))
                if case == 10:
                    results['traces'][case] = {'open': dict(collections.Counter(
                        '%s:%s' % e for e in events)), 'metrics': metrics}
                else:
                    # Window 0 runs from the first marker to the first job's marker.
                    jobs = all_windows[1:]
                    assert len(jobs)==len(durations)
                    assert all(w[('call','uncached-store8')]==0 and
                               w[('call','uncached-store64')] in (23,28) for w in jobs), 'client ring store width/count'
                    results['traces'][case] = {**summarize(jobs, segments(events)), 'metrics': metrics}
                (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    finally:
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps({k: v for k, v in results.items() if k != 'counted_entries'}, indent=2))


if __name__ == '__main__':
    main()
