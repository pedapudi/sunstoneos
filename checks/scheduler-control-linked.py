#!/usr/bin/env python3
"""Reject old owner-control behavior in the linked eight-core FreeRTOS fixture."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import threading
import time
from elftools.elf.elffile import ELFFile
from harness import CACHE, environment

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build-dir', '{build:scheduler-control-models}', '--output', '{output}', '--positive', '--perturb']]
# Perturbation sweep: kernel/scheduler/perturb.h names the sites. Each point delays one site on every core by a
# number of loop turns; on QEMU's deterministic clock every delay is a different interleaving of the same run.
# A QEMU run is killed after this many host seconds: the multiple of the slowest measured point that tools/gate-slots.json records. The
# runs count instructions (-icount), so the guest result does not depend on the limit; it only ends a run that hangs.
_PERTURB = json.loads((ROOT / 'tools/gate-slots.json').read_text())['perturb']
QEMU_KILL_SECONDS = _PERTURB['kill_multiplier'] * _PERTURB['slowest_point_seconds']
# The wake latency of the yield models is bounded by the critical path of each wake, in instructions (docs/validation.md, "Scheduler QEMU
# models"). The model's wake period is 400 us (checks/scheduler-yield-qemu.c) and the bound is half of it, 200 us; the runs use
# -icount shift=0, one nanosecond per instruction (1000 per microsecond, 16 per tick of the 62.5 MHz counter), so the bound is 200,000
# instructions, the value the old 12,500-tick bound had. The owner of every object in these models is core 0.
INSTRUCTIONS_PER_US = 1000
WAKE_BOUND_US = 200
CRITICAL_PATH_BOUND = WAKE_BOUND_US * INSTRUCTIONS_PER_US
OWNER_CORE = 0
YIELD_MODELS = ('scheduler-yield-qemu', 'scheduler-yield-storm-qemu')
# Extra work on the wake path for the regression case: the word after the sites of kernel/scheduler/perturb.h in the table, which the
# yield models read (checks/scheduler-yield-qemu.c), as this many loop turns (about 5 instructions each) before every served mark.
# The models' own end-of-run accounting fails above about 3,000 turns, so the case does not need the critical path to be the only
# detector: it needs the critical path to cross the bound, which it does at 6,000 turns (217,260 instructions measured, 8.6 percent above the bound; the run is deterministic).
WAKE_PATH_REGRESSION_TURNS = 6000      # in the table word after the last site (WAKE_PATH_WORD, below)
PLUGIN_HEADER = CACHE / 'host/qemu-plugin-10.2.1/qemu-plugin.h'
PLUGIN_HEADER_SHA256 = 'aebc639d01685893a9d71b9ef3afa7ea67e0be6a8959781c4057bd4974d05d4d'
PERTURB_SITES = {'api-entry': 0, 'owner-publish': 1, 'owner-drain': 2, 'control-open': 3, 'object-walk': 4}
# Each model's pass marker and the sites it is swept at. The partitioned models link no event group, so the walk site
# runs only in the objects model.
SCHEDULER_SITES = ('api-entry', 'owner-publish', 'owner-drain', 'control-open')
PERTURB_MODELS = {'scheduler-partitioned-qemu': ('PASS partitioned concurrent:', SCHEDULER_SITES),
                  'scheduler-partitioned-storm-qemu': ('PASS partitioned concurrent:', SCHEDULER_SITES),
                  'scheduler-objects-qemu': ('PASS objects:', ('object-walk',))}
PERTURB_TURNS = (300, 1000, 3000)
WAKE_PATH_WORD = len(PERTURB_SITES)             # derived from the site count, so a new site cannot collide with it
CHECK_INPUTS = ['tools/qemu-critical-path.c', 'tools/qemu-plugin-glib', 'kernel/scheduler', 'patches/freertos-smp-c.patch',
                'patches/freertos-partitioned-api.patch', 'patches/freertos-partitioned-objects.patch',
                'checks/scheduler-partitioned-qemu.c',
                'checks/scheduler-partitioned-qemu-config/FreeRTOSConfig.h',
                'checks/scheduler-objects-qemu.c', 'checks/scheduler-yield-qemu.c']


def main():
    # The sites of the table are the enumerators of perturb.h; the yield models' wake-path word follows the last (SS_PERTURB_SITES).
    enumerators = re.search(r'enum ss_kernel_perturb_site \{(.*?)\};', (ROOT / 'kernel/scheduler/perturb.h').read_text(), re.S).group(1)
    assert len(re.findall(r'^\s*SS_PERTURB_(?!SITES)\w+', enumerators, re.M)) == len(PERTURB_SITES), 'PERTURB_SITES differs from perturb.h'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True,
                        help='configured build with scheduler-partitioned-qemu built')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--qemu', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/bin/qemu-system-aarch64')
    parser.add_argument('--case', action='append')
    parser.add_argument('--positive', action='store_true', help='run the six unmodified linked models first')
    parser.add_argument('--perturb', action='store_true',
                        help='run the linked models at every perturbation site and delay; each must pass')
    parser.add_argument('--jobs', type=int, default=8, help='concurrent QEMU runs in the perturbation sweep')
    args = parser.parse_args()
    build, output = args.build_dir.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = dict(environment(), TMPDIR=str(output))
    target = build / 'CMakeFiles/scheduler-partitioned-qemu.dir'
    flags = dict(re.findall(r'^(C_(?:DEFINES|INCLUDES|FLAGS)) = (.*)$',
                            (target / 'flags.make').read_text(), re.M))
    compile_flags = shlex.split(' '.join(flags[k] for k in ('C_DEFINES', 'C_INCLUDES', 'C_FLAGS')))
    link = shlex.split((target / 'link.txt').read_text())
    adapter = (ROOT / 'kernel/scheduler/freertos.c').read_text()
    tasks = (build / 'freertos-smp/tasks.c').read_text()
    port = (build / 'freertos-smp/port.c').read_text()
    fixture = (ROOT / 'checks/scheduler-partitioned-qemu.c').read_text()

    times, times_lock = {}, threading.Lock()      # host seconds of each QEMU run, written to qemu-times.json

    def execute(image, directory, cores=8, perturb=None, plugin=None):
        (directory / 'image.sha256').write_text(hashlib.sha256(image.read_bytes()).hexdigest() + '\n')
        command = [str(args.qemu.resolve()), '-machine', 'virt,gic-version=3,secure=off,virtualization=off',
                   '-accel', 'tcg,thread=single', '-icount', 'shift=0,sleep=off', '-cpu', 'cortex-a76',
                   '-smp', str(cores), '-m', '128M', '-nographic', '-monitor', 'none',
                   '-semihosting-config', 'enable=on,target=native', '-device', f'loader,file={image},cpu-num=0',
                   '-device', 'loader,addr=0x47ff0000,data=0,data-len=4']
        # Word 0 at 0x47ff0040 selects the delaying cores (zero: all); word 1 + site holds that site's turns.
        table = [0] * (2 + len(PERTURB_SITES))          # cores, one word per site, then the yield models' wake-path word
        if perturb:
            table[1 + perturb[0]] = perturb[1]
            if len(perturb) > 2:
                table[0] = perturb[2]               # the cores that delay, one bit each
        for index, word in enumerate(table):
            command += ['-device', f'loader,addr=0x{0x47ff0040 + 4 * index:x},data={word},data-len=4']
        if plugin:
            command += ['-plugin', plugin]
        process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, start_new_session=True)
        started = time.monotonic()
        try:
            stdout, stderr = process.communicate(timeout=QEMU_KILL_SECONDS)
            with times_lock:
                times[directory.name] = round(time.monotonic() - started, 2)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate()
            (directory / 'qemu.log').write_text(stdout + stderr + '\nQEMU timed out\n')
            if image.parent != directory:
                shutil.copy2(image, directory / 'failed.elf')
            raise AssertionError((image, 'timeout'))
        (directory / 'qemu.log').write_text(stdout + stderr)
        if process.returncode and image.parent != directory:
            shutil.copy2(image, directory / 'failed.elf')
        return process.returncode, stdout + stderr

    def build_plugin():
        """tools/qemu-critical-path.c against the unmodified QEMU 10.2.1 plugin header."""
        if hashlib.sha256(PLUGIN_HEADER.read_bytes()).hexdigest() != PLUGIN_HEADER_SHA256:
            raise SystemExit(f'{PLUGIN_HEADER}: not the QEMU 10.2.1 plugin header')
        library = output / 'qemu-critical-path.so'
        subprocess.run(['clang-21', '-shared', '-fPIC', '-O2', '-fvisibility=hidden', '-Wall', '-Werror',
                        '-I', str(PLUGIN_HEADER.parent), '-I', str(ROOT / 'tools/qemu-plugin-glib'),
                        str(ROOT / 'tools/qemu-critical-path.c'), '-o', str(library)], check=True)
        return library

    def critical_paths(trace):
        """The critical path of every ordinary wake in a plugin trace: the owner's instructions plus the victim's own instructions
        outside the spin function, between the wake interrupt and the served task. Both are counted outside the spin function."""
        paths, wake = [], {}
        for text in trace.read_text().splitlines():
            fields = text.split()
            if fields[0] != 'M':
                continue
            offset, executing = int(fields[1], 16), int(fields[2])
            total, spin = [int(v) for v in fields[3:11]], [int(v) for v in fields[11:19]]
            core = offset // 16
            # Each marker is called on the core it names. The plugin reads the executing core from the guest (TPIDR_EL1), because the
            # vcpu argument of QEMU 10.2.1's block callback names the vCPU that translated the block; a marker credited to another core
            # means the attribution is wrong and every count after it is too.
            assert executing == core, ('marker of core', core, 'ran on core', executing)
            if offset % 16 == 0:
                wake[core] = (total, spin)
            elif core in wake:
                (t0, s0), (t1, s1) = wake.pop(core), (total, spin)
                work = lambda c: (t1[c] - s1[c]) - (t0[c] - s0[c])
                paths.append((work(OWNER_CORE) + work(core), core))
        return paths

    def yield_critical_path(name, directory, plugin, perturb=None, must_pass=True):
        """Run one yield model under the plugin; returns (worst critical path, its core, wakes)."""
        directory.mkdir(exist_ok=True)
        image = build / (name + '.elf')
        with ELFFile(image.open('rb')) as elf:
            symbols = {sym.name: (sym['st_value'], sym['st_size']) for sym in elf.get_section_by_name('.symtab').iter_symbols()}
        low, high = symbols['ymark_isr_0'][0], symbols['ymark_srv_7'][0] + 8
        spin_low, spin_size = symbols['busy_task']
        trace = directory / 'critical-path.txt'
        status, log = execute(image, directory, 8, perturb,
                              f'{plugin},lo={low:x},hi={high:x},spin={spin_low:x}-{spin_low + spin_size:x},out={trace}')
        assert not must_pass or (status == 0 and 'PASS yield:' in log and 'FAIL' not in log), (name, directory / 'qemu.log')
        paths = critical_paths(trace)
        assert len(paths) > 300, (name, 'too few paired wakes', len(paths))
        worst = max(paths)
        (directory / 'critical-path.json').write_text(json.dumps(
            {'bound_instructions': CRITICAL_PATH_BOUND, 'wakes': len(paths), 'worst': worst[0], 'worst_core': worst[1]}, indent=1))
        return worst[0], worst[1], len(paths)

    if args.positive:
        plugin = build_plugin()
        for name, marker, cores in (
            ('scheduler-partitioned-qemu', 'PASS partitioned concurrent:', 8),
            ('scheduler-partitioned-storm-qemu', 'PASS partitioned concurrent:', 8),
            ('scheduler-objects-qemu', 'PASS objects:', 8),
            ('scheduler-yield-qemu', 'PASS yield:', 8),
            ('scheduler-yield-storm-qemu', 'PASS yield:', 8),
            ('scheduler-partitioned-heap-qemu', 'PASS partitioned heap lifecycle:', 2),
        ):
            directory = output / name
            directory.mkdir(exist_ok=True)
            if name in YIELD_MODELS:
                worst, core, wakes = yield_critical_path(name, directory, plugin)
                assert worst <= CRITICAL_PATH_BOUND, (name, 'critical path', worst, 'instructions on core', core, 'exceeds', CRITICAL_PATH_BOUND)
                print(f'PASS linked model: {name} (critical path {worst} instructions of {CRITICAL_PATH_BOUND}, core {core}, {wakes} wakes)', flush=True)
                continue
            status, log = execute(build / (name + '.elf'), directory, cores)
            assert status == 0 and marker in log and 'FAIL' not in log, (name, directory / 'qemu.log')
            print('PASS linked model: ' + name, flush=True)
        # The bound must still catch a regression: extra work on the wake path, with every other input equal, crosses it. The model may
        # also fail its own accounting at this size; the critical path of the wakes it did serve is what is asserted.
        directory = output / 'yield-wake-path-regression'
        worst, core, wakes = yield_critical_path('scheduler-yield-qemu', directory, plugin,
                                                 (WAKE_PATH_WORD, WAKE_PATH_REGRESSION_TURNS), must_pass=False)
        assert worst > CRITICAL_PATH_BOUND, ('the wake-path regression did not cross the bound', worst, CRITICAL_PATH_BOUND)
        print(f'PASS wake-path regression: {WAKE_PATH_REGRESSION_TURNS} extra turns give a critical path of {worst} instructions, '
              f'above {CRITICAL_PATH_BOUND}', flush=True)

    if args.perturb:
        from concurrent.futures import ThreadPoolExecutor
        points = [(model, site, turns) for model, (_, sites) in PERTURB_MODELS.items()
                  for site in sites for turns in PERTURB_TURNS]

        def perturbed(point):
            model, site, turns = point
            directory = output / f'perturb-{model}-{site}-{turns}'
            directory.mkdir(exist_ok=True)
            try:
                status, log = execute(build / (model + '.elf'), directory, 8, (PERTURB_SITES[site], turns))
            except AssertionError:
                return point, 'QEMU timed out'
            if status == 0 and PERTURB_MODELS[model][0] in log and 'FAIL' not in log:
                return point, None
            failed = re.search(r'FAIL [^\s]+', log)
            return point, failed.group(0) if failed else f'status {status}'

        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(perturbed, points))
        for (model, site, turns), failure in results:
            print(f'{"PASS" if failure is None else "FAIL"} perturbed linked model: {model} {site} {turns} turns'
                  + ('' if failure is None else f' ({failure})'), flush=True)
        (output / 'qemu-times.json').write_text(json.dumps({'jobs': args.jobs, 'seconds': times, 'slowest': max(times.values())}, indent=1))
        assert all(failure is None for _, failure in results), 'perturbed linked models failed'

    def line(text, needle):
        matches = list(re.finditer(r'^' + re.escape(needle), text, re.M))
        assert len(matches) == 1, needle
        begin = matches[0].start()
        end = text.index(';', begin) if needle.lstrip().startswith('configASSERT(') else begin
        return text[:end].count('\n') + 1

    def failure(file, number):
        return f'{file}:0x{number:016x}'

    cases = {
        'gate-admission-acquiring': ('adapter',
            [('if (gate != SS_GATE_ACQUIRING && gate != SS_GATE_HELD) return irq;',
              'if (true && gate != SS_GATE_HELD) return irq;')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '    configASSERT(admission_waiting == kind);'))),
        'gate-admission-held': ('adapter',
            [('if (gate != SS_GATE_ACQUIRING && gate != SS_GATE_HELD) return irq;',
              'if (gate != SS_GATE_ACQUIRING && true) return irq;')],
            failure('freertos.c', line(adapter, '    configASSERT(gate != SS_GATE_HELD &&'))),
        'gate-admission-delay': ('tasks',
            [('traceENTER_vTaskDelay( xTicksToDelay );\nUBaseType_t irq = ssKernelTaskMask();',
              'traceENTER_vTaskDelay( xTicksToDelay );\nUBaseType_t irq = portSET_INTERRUPT_MASK();')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '    configASSERT(admission_waiting == kind);'))),
        'gate-admission-notification': ('adapter',
            [('    configASSERT(index < configTASK_NOTIFICATION_ARRAY_ENTRIES);\n    UBaseType_t irq = ssKernelTaskMask();',
              '    configASSERT(index < configTASK_NOTIFICATION_ARRAY_ENTRIES);\n    UBaseType_t irq = portSET_INTERRUPT_MASK();')],
            failure('freertos.c', line(adapter, '    configASSERT(gate != SS_GATE_HELD &&'))),
        'gate-admission-object-suspend': ('adapter',
            [('static __attribute__((noinline)) void ssKernelObjectSuspendOnAt(const void *object, uintptr_t site)\n{\n    UBaseType_t irq = ssKernelTaskMask();',
              'static __attribute__((noinline)) void ssKernelObjectSuspendOnAt(const void *object, uintptr_t site)\n{\n    UBaseType_t irq = portSET_INTERRUPT_MASK();')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '            configASSERT(admission_waiting == kind && admission_completed < kind);'))),
        'gate-inherited-mask': ('adapter',
            [('if (cpu->preempt || ssKernelHolds(cpu) || irq <= portMAX_API_PRIORITY_MASK) return irq;',
              'if (cpu->preempt || ssKernelHolds(cpu)) return irq;')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '    configASSERT(irq > portMAX_API_PRIORITY_MASK);'))),
        'local-holder-block': ('fixture',
            [('        xTaskNotifyGive(controller);\n        configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1);',
              '        xTaskNotifyGive(controller);\n        configASSERT(ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 1); vTaskDelay(1);')],
            failure('freertos.c', line(adapter, '    configASSERT(gate != SS_GATE_HELD &&'))),
        'delay-abort-isolation': ('adapter',
            [('if (trace_ready && s->wait_kind == SS_K_EVENT) t->ucDelayAborted = pdTRUE;',
              'if (true) t->ucDelayAborted = pdTRUE;')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '    configASSERT(!ss_kernel_test_delay_aborted());'))),
        'yield-held-object': ('adapter',
            [('bool defer = cpu->preempt || ssKernelHolds(cpu);', 'bool defer = false;')],
            failure('freertos.c', line(adapter,
                '    configASSERT(irq <= portMAX_API_PRIORITY_MASK && cpu->o_critical == 1 && cpu->preempt == 1);'))),
        'deferred-voluntary-yield': ('adapter',
            [('return ssKernelCPUs[ssKernelCPUId()].explicit_yield;', 'return false;')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '    configASSERT(deferred_peer_ran == 1);'))),
        'automatic-tail-preemption-disable': ('adapter',
            [('bool voluntary = cpu->explicit_yield;', 'bool voluntary = cpu->yield_tail;')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '    configASSERT(deferred_peer_ran == 0);'))),
        'empty-sgi': ('port', [('#if !configSS_PARTITIONED_SCHEDULER', '#if 1')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '        configASSERT(__atomic_load_n(&switches[0], __ATOMIC_ACQUIRE) == before);'))),
        'freeze-after-mark': ('adapter',
            [('if (cpu->preempt || (!mandatory && ss_control_checkpoint(&ssKernelControl, c))) {',
              'if (cpu->preempt || ss_control_checkpoint(&ssKernelControl, c)) {')],
            # Two detections of one defect: the selection hook sees the early acknowledgement, or the yield
            # returns without a selection and the suspending task finds itself not running. Which comes first
            # depends on the image layout's interleaving.
            (failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '        configASSERT(mandatory && ss_kernel_test_freeze_unacked(cpu));')),
             failure('freertos.c', line(adapter,
                '        configASSERT(atomic_load_explicit(&s->execution, memory_order_acquire) == SS_K_RUNNING);')))),
        'freeze-after-remote': ('adapter',
            [('if (cpu->preempt || (!mandatory && ss_control_checkpoint(&ssKernelControl, c))) {',
              'if (!direct && (cpu->preempt || ss_control_checkpoint(&ssKernelControl, c))) {')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '        configASSERT(mandatory && ss_kernel_test_freeze_unacked(cpu));'))),
        'drain-during-freeze': ('adapter',
            [('unsigned gate = atomic_load_explicit(&ssKernelControl.gate_phase, memory_order_acquire);\n    if (xSchedulerRunning && (gate == SS_GATE_ACQUIRING || gate == SS_GATE_HELD)) {',
              'unsigned gate = atomic_load_explicit(&ssKernelControl.gate_phase, memory_order_acquire);\n    if (false && (gate == SS_GATE_ACQUIRING || gate == SS_GATE_HELD)) {')],
            failure('freertos.c', line(adapter,
                '    configASSERT(ssKernelTasks[slot].ready.priority == priority);'))),
        'freeze-after-block': ('adapter',
            [('if (!cpu->direct_suspend && !ssKernelNeedsFrame(cpu) && !cpu->preempt &&',
              'if (!cpu->direct_suspend && !false && !cpu->preempt &&')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '        configASSERT(mandatory && ss_kernel_test_freeze_unacked(cpu));'))),
        'stale-abort': ('adapter',
            [('if (s->generation == state->abort_wait_generation &&\n            atomic_load_explicit(&s->execution, memory_order_acquire) == SS_K_BLOCKED)',
              'if (/* old: accept a later wait */ true &&\n            atomic_load_explicit(&s->execution, memory_order_acquire) == SS_K_BLOCKED)')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '    __atomic_store_n(&abort_phase, 2, __ATOMIC_RELEASE);') + 1)),
        'shielded-repair': ('adapter',
            [('if (ssKernelCPUs[c].object_repair[slot / 64] & bit) return false;',
              'if (ssKernelCPUs[c].object_repair[slot / 64] & bit) return true;')],
            failure('freertos.c', line(adapter, '        configASSERT(!orphan);'))),
        'incoming-during-detach': ('adapter',
            [('!s->ready.queued && !s->detach_pending &&', '!s->ready.queued && true &&')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '        configASSERT(!queued);'))),
        'ineligible-adoption': ('adapter',
            [('(atomic_load_explicit(&ssKernelControl.slot[s->ready.slot].affinity, memory_order_acquire) & (UINT64_C(1) << c))', 'true'),
             ('s->detach_pending = !eligible;', 's->detach_pending = false;'),
             ('if (!eligible && s->ready.queued) configASSERT(ss_owner_remove(&cpu->run, &s->ready));',
              'if (false && s->ready.queued) configASSERT(ss_owner_remove(&cpu->run, &s->ready));')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '        configASSERT(!queued && cpu == 3);'))),
        'automatic-queue-preemption-disable': ('adapter',
            [('if (yield && ((TCB_t *)ssKernelCPUs[owner].run.current->context)->xPreemptionDisable)',
              'if (false)')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '            configASSERT(local_wake_ran == 0);'))),
        'enable-preemption-ready-work': ('adapter',
            [('if (enabling_preemption && cpu->run.current == &s->ready && cpu->run.ready_bitmap &&',
              'if (false && cpu->run.current == &s->ready && cpu->run.ready_bitmap &&')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '            vTaskPreemptionEnable(NULL);') + 1)),
        'frozen-local-publication': ('adapter',
            [('gate != SS_GATE_ACQUIRING && gate != SS_GATE_HELD)\n            ssKernelInspect',
              'true)\n            ssKernelInspect')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '            configASSERT((ss_kernel_test_task_state(local_wake_target) & 257) == 1);'))),
        'held-self-mandatory-frame': ('adapter',
            [('            ssKernelCancel(s);\n            s->suspended = (flags & SS_CONTROL_SUSPENDED) &&\n                (!atomic_load_explicit(&s->cleanup_shield, memory_order_acquire) || (flags & SS_CONTROL_SELF));\n            atomic_store_explicit(&s->execution, SS_K_SUSPENDED, memory_order_release);\n            cpu->direct_suspend = cpu->reschedule = true;',
              '            cpu->reschedule = true;')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '    configASSERT(!held_self_returned && xTaskGetCurrentTaskHandleForCore(1) != held_self_target);'))),
        'remote-frame-final-resume': ('adapter',
            [('bool pending_remote = deferred && (suspension & SS_CONTROL_REMOTE_FRAME) &&',
              'bool pending_remote = deferred && live.suspended &&')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '    configASSERT((ss_kernel_test_suspension(self) &'))),
        'self-frame-fulfilled-before-mask': ('adapter',
            [('if (!(suspension & (SS_CONTROL_SELF | SS_CONTROL_SELF_FRAME))) {',
              'if (!(suspension & (SS_CONTROL_SELF | SS_CONTROL_SELF_FRAME)) && !(suspension & SS_CONTROL_REMOTE_FRAME)) {')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '        configASSERT(shield_self_phase != 6);'))),
        'task-resume-shielded-self': ('adapter',
            [('if (command == SS_K_RESUME && (from_isr || resume_cleanup_self)) {',
              'if (command == SS_K_RESUME && from_isr) {')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '    configASSERT(shield_self_phase == 9 || !(ss_kernel_test_suspension(shield_self_target) & SS_CONTROL_SELF));'))),
        'shielded-self-preservation': ('adapter',
            [('guarded.suspended = (suspension & SS_CONTROL_SELF) && state->suspended;',
              'guarded.suspended = false;')],
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '    configASSERT(shield_self_phase == 8);'))),
        'self-delete-gate-admission': ('adapter',
            [('UBaseType_t irq = from_isr ? portSET_INTERRUPT_MASK_FROM_ISR() : ssKernelTaskMask();',
              'UBaseType_t irq = from_isr ? portSET_INTERRUPT_MASK_FROM_ISR() : (command == SS_K_DELETE && handle == current ? portSET_INTERRUPT_MASK() : ssKernelTaskMask());')],
            failure('freertos.c', line(adapter,
                '        configASSERT(gate != SS_GATE_HELD &&'))),
        'self-delete-before-frame-freeze': ('adapter',
            [('atomic_store_explicit(&s->execution, SS_K_DEAD, memory_order_release);',
              '/* old: logical publication alone leaves the current frame valid */')],
            failure('freertos.c', line(adapter,
                '    configASSERT(ssKernelNeedsFrame(&ssKernelCPUs[c]) && !ssKernelCPUs[c].preempt &&'))),
        'recursive-self-delete-shield': ('fixture',
            [('    vTaskDelete(NULL);\n    platform_panic("self deletion returned", 0);',
              '    ss_kernel_test_cleanup_shield(xTaskGetCurrentTaskHandle(), true);\n    vTaskDelete(NULL);\n    platform_panic("self deletion returned", 0);')],
            failure('freertos.c', line(adapter,
                '        configASSERT(!atomic_load_explicit(&s->cleanup_shield, memory_order_acquire));'))),
    }
    for name in args.case or cases:
        kind, changes, expected = cases[name]
        directory = output / name
        directory.mkdir(exist_ok=True)
        code = {'adapter': adapter, 'port': port, 'tasks': tasks, 'fixture': fixture}[kind]
        for before, after in changes:
            assert code.count(before) == 1, (name, before)
            code = code.replace(before, after)
        source = directory / {'adapter': 'freertos.c', 'port': 'port.c',
                              'tasks': 'tasks.c', 'fixture': 'fixture.c'}[kind]
        source.write_text(code)
        if kind == 'adapter':
            unit = directory / 'tasks.c'
            original = '#include "freertos.c"'
            assert tasks.count(original) == 1
            unit.write_text(tasks.replace(original, '#include "' + str(source) + '"'))
            original_object = 'CMakeFiles/scheduler-partitioned-qemu.dir/freertos-smp/tasks.c.obj'
        elif kind == 'tasks':
            unit = source
            original_object = 'CMakeFiles/scheduler-partitioned-qemu.dir/freertos-smp/tasks.c.obj'
        elif kind == 'fixture':
            unit = source
            original_object = 'CMakeFiles/scheduler-partitioned-qemu.dir/checks/scheduler-partitioned-qemu.c.obj'
        else:
            unit = source
            original_object = 'CMakeFiles/scheduler-partitioned-qemu.dir/freertos-smp/port.c.obj'
        obj, elf = directory / 'changed.o', directory / 'negative.elf'
        command = [link[0], '--target=aarch64-none-elf', *compile_flags,
                   *(['-mgeneral-regs-only'] if kind in ('adapter', 'tasks') else []), '-c', str(unit), '-o', str(obj)]
        compiled = subprocess.run(command, cwd=build, env=env, text=True, capture_output=True)
        (directory / 'compile.log').write_text(compiled.stdout + compiled.stderr)
        assert compiled.returncode == 0, (name, 'compile.log')
        command = [str(obj) if arg == original_object else
                   '-Wl,-Map,' + str(directory / 'negative.map') if arg.startswith('-Wl,-Map,') else arg
                   for arg in link]
        command[command.index('-o') + 1] = str(elf)
        linked = subprocess.run(command, cwd=build, env=env, text=True, capture_output=True)
        (directory / 'link.log').write_text(linked.stdout + linked.stderr)
        assert linked.returncode == 0, (name, 'link.log')
        status, log = execute(elf, directory)
        expected = (expected,) if isinstance(expected, str) else expected
        seen = [site for site in expected if site in log]
        assert status and seen, (name, expected, directory / 'qemu.log')
        print(f'PASS linked old-behavior negative: {name} ({seen[0]})', flush=True)


if __name__ == '__main__':
    main()
