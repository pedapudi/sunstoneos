#!/usr/bin/env python3
"""Reject old owner-control behavior in the linked eight-core FreeRTOS fixture."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
from harness import environment

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build-dir', '{build:scheduler-control-models}', '--output', '{output}', '--positive']]
CHECK_INPUTS = ['kernel/scheduler', 'patches/freertos-smp-c.patch',
                'patches/freertos-partitioned-api.patch', 'checks/scheduler-partitioned-qemu.c',
                'checks/scheduler-partitioned-qemu-config/FreeRTOSConfig.h',
                'checks/scheduler-objects-qemu.c', 'checks/scheduler-yield-qemu.c']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True,
                        help='configured build with scheduler-partitioned-qemu built')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--qemu', type=Path, default=Path.home() / '.cache/sunstoneos/host/usr/bin/qemu-system-aarch64')
    parser.add_argument('--case', action='append')
    parser.add_argument('--positive', action='store_true', help='run the six unmodified linked models first')
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

    def execute(image, directory, cores=8):
        (directory / 'image.sha256').write_text(hashlib.sha256(image.read_bytes()).hexdigest() + '\n')
        command = [str(args.qemu.resolve()), '-machine', 'virt,gic-version=3,secure=off,virtualization=off',
                   '-accel', 'tcg,thread=single', '-icount', 'shift=0,sleep=off', '-cpu', 'cortex-a76',
                   '-smp', str(cores), '-m', '128M', '-nographic', '-monitor', 'none',
                   '-semihosting-config', 'enable=on,target=native', '-device', f'loader,file={image},cpu-num=0',
                   '-device', 'loader,addr=0x47ff0000,data=0,data-len=4']
        process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, start_new_session=True)
        try:
            stdout, stderr = process.communicate(timeout=120)
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

    if args.positive:
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
            status, log = execute(build / (name + '.elf'), directory, cores)
            assert status == 0 and marker in log and 'FAIL' not in log, (name, directory / 'qemu.log')
            print('PASS linked model: ' + name, flush=True)

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
            failure('checks/scheduler-partitioned-qemu.c', line(fixture,
                '        configASSERT(mandatory && ss_kernel_test_freeze_unacked(cpu));'))),
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
        assert status and expected in log, (name, expected, directory / 'qemu.log')
        print(f'PASS linked old-behavior negative: {name} ({expected})', flush=True)


if __name__ == '__main__':
    main()
