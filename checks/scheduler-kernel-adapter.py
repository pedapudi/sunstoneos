#!/usr/bin/env python3
"""Compile the private API patch and link real FreeRTOS owner transitions."""
import argparse
import os
from pathlib import Path
import subprocess
import importlib.util
import json
import re
ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--freertos', required=True, type=Path)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--sysroot', required=True, type=Path)
    parser.add_argument('--cc', default='/usr/bin/clang-21')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(build))
    def run(command):
        result = subprocess.run(list(map(str, command)), text=True, capture_output=True, env=env)
        if result.returncode:
            raise RuntimeError(f'{command[0]} failed:\n{result.stdout}{result.stderr}')
        return result.stdout
    # Keep the admitted layout chain independently buildable in both modes.
    spec = importlib.util.spec_from_file_location('layout', ROOT / 'checks/scheduler-kernel-layout.py')
    layout = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(layout)
    layout.check(args.freertos.resolve(), build, args.cc, args.sysroot.resolve())
    inventory = json.loads((ROOT / 'checks/scheduler-kernel-api.json').read_text())
    source = (build / 'tasks.c').read_text()
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    public = set(re.findall(r'\b((?:vTask|xTask|uxTask|ulTask|pvTask|pcTask|eTask)\w*)\s*\([^;{}]*\)\s*\{', source))
    assert public == {entry['name'] for entry in inventory['apis']}, 'task API inventory drift'
    # Select every converted optional branch, including timer-service waits.
    (build / 'FreeRTOSConfig.h').write_text(
        '#include "' + str(ROOT / 'platform/qemu-smp/FreeRTOSConfig.h') + '"\n'
        '#define INCLUDE_xTaskDelayUntil 1\n#define INCLUDE_eTaskGetState 1\n'
        '#define INCLUDE_uxTaskPriorityGet 1\n#define INCLUDE_vTaskPrioritySet 1\n'
        '#define INCLUDE_xTaskAbortDelay 1\n#define INCLUDE_xTaskResumeFromISR 1\n'
        '#undef configUSE_TIMERS\n#define configUSE_TIMERS 1\n'
        '#define configTIMER_TASK_PRIORITY 1\n#define configTIMER_QUEUE_LENGTH 8\n'
        '#define configTIMER_TASK_STACK_DEPTH configMINIMAL_STACK_SIZE\n'
        '#define configUSE_TRACE_FACILITY 1\n'
        '#define configUSE_APPLICATION_TASK_TAG 1\n'
        '#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 2\n'
        '#define INCLUDE_uxTaskGetStackHighWaterMark 1\n'
        '#define INCLUDE_uxTaskGetStackHighWaterMark2 1\n')
    patched = build / 'tasks-api.c'
    run(['patch', '--fuzz=0', '--reject-file=-', '--output', patched,
         build / 'tasks.c', ROOT / 'patches/freertos-partitioned-api.patch'])
    audit_spec = importlib.util.spec_from_file_location('api_audit', ROOT / 'checks/scheduler-kernel-api-audit.py')
    api_audit = importlib.util.module_from_spec(audit_spec)
    audit_spec.loader.exec_module(api_audit)
    api_audit.audit(patched.read_text(), inventory)
    for name in ('queue.c', 'event_groups.c', 'stream_buffer.c', 'timers.c'):
        (build / name).write_text((args.freertos / name).read_text())
    run(['patch', '--fuzz=0', '--reject-file=-', '-d', build, '-p0',
         '-i', ROOT / 'patches/freertos-partitioned-objects.patch'])
    common = [args.cc, '-std=c11', '-O1', '-g', '-ffunction-sections', '-fdata-sections',
              '-DconfigSS_PARTITIONED_SCHEDULER=1', '-DSUNSTONE_SMP_CORES=8',
              '-I' + str(build / 'include'), '-I' + str(build),
              '-I' + str(ROOT / 'platform/qemu-smp'), '-I' + str(ROOT / 'include'),
              '-I' + str(ROOT / 'kernel/scheduler'), '-I' + str(args.freertos / 'include')]
    run([*common, '--target=aarch64-none-elf', '-march=armv8.1-a+lse', '-ffreestanding',
         '-Werror', '-isystem', args.sysroot / 'include', '-c', patched, '-o', build / 'tasks-api.o'])
    ast = json.loads(run([*common, '--target=aarch64-none-elf', '-march=armv8.1-a+lse',
                          '-ffreestanding', '-isystem', args.sysroot / 'include',
                          '-Xclang', '-ast-dump=json', '-fsyntax-only', patched]))
    (build / 'tasks-api-ast.json').write_text(json.dumps(ast))
    enabled = {node.get('name') for node in ast['inner'] if node['kind'] == 'FunctionDecl'
               and any(child['kind'] == 'CompoundStmt' for child in node.get('inner', []))}
    selection = [{**entry, 'selected': entry['name'] in enabled} for entry in inventory['apis']]
    (build / 'api-selection.json').write_text(json.dumps(selection, indent=2) + '\n')
    pending = [entry['name'] for entry in selection if entry['selected'] and entry['state'] == 'pending']
    assert not pending, f'selected task APIs remain pending: {pending}'
    reachable = api_audit.audit_reachability(ast, enabled & public)
    print(f'private task API AST: {sum(entry["selected"] for entry in selection)} selected routes; '
          f'{reachable} reachable functions contain no legacy scheduler lists')

    disabled = [flag.replace('configSS_PARTITIONED_SCHEDULER=1',
                             'configSS_PARTITIONED_SCHEDULER=0') for flag in common]
    run([*disabled, '--target=aarch64-none-elf', '-march=armv8.1-a+lse', '-ffreestanding',
         '-Werror', '-isystem', args.sysroot / 'include', '-c', patched, '-o', build / 'tasks-api-off.o'])
    for name in ('queue', 'event_groups', 'stream_buffer', 'timers'):
        run([*common, '--target=aarch64-none-elf', '-march=armv8.1-a+lse', '-ffreestanding',
             '-Werror', '-isystem', args.sysroot / 'include', '-c', build / (name + '.c'),
             '-o', build / (name + '.o')])
    translation = build / 'linked.c'
    translation.write_text('#include "' + str(ROOT / 'checks/scheduler-kernel-host-port.h') + '"\n'
                           '#include "tasks-api.c"\n'
                           '#include "' + str(ROOT / 'checks/scheduler-kernel-linked.c') + '"\n')
    binary = build / 'linked'
    sources = [args.freertos / 'list.c', *[build / name for name in
               ('queue.c', 'event_groups.c', 'stream_buffer.c', 'timers.c')],
               *[ROOT / 'kernel/scheduler' / name for name in
               ('owner.c', 'notification.c', 'deadline.c', 'freertos_registry.c', 'freertos_control.c')]]
    run([*common, '-fsanitize=address,undefined', '-Wl,--gc-sections', translation, *sources, '-o', binary])
    print(run([binary]), end='')
    print(run([binary, 'startup']), end='')
    race = build / 'control-reuse'
    run([args.cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
         '-fsanitize=address,undefined', ROOT / 'kernel/scheduler/owner.c',
         ROOT / 'kernel/scheduler/freertos_control.c',
         ROOT / 'checks/scheduler-kernel-control-races.c', '-o', race])
    print(run([race]), end='')
    print('private adapter compiled for AArch64 and linked with actual FreeRTOS tasks/list code; live enablement remains gated')

if __name__ == '__main__':
    main()
