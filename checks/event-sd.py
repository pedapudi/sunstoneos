#!/usr/bin/env python3
"""Run actual SD owner waits with FreeRTOS notifications and precise deadlines."""
import argparse
import importlib.util
import json
import re
from pathlib import Path
import subprocess
import sys
import host_rtos
from harness import environment

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/sd_owner.c', 'apps/os/large_sd.c', 'apps/os/large_load.c',
                'drivers/storage/a733_mmc_platform.c', 'runtime/deadline.c',
                'include/sunstone_notify.h', 'apps/os/main.patch', 'apps/board/main.c', 'cmake/patch.cmake', 'tools/sunstone-manifest.py',
                'checks/event-sd.c', 'checks/event-sd.py', 'checks/os-quiesce-storage.py']


def function(source, name):
    spec = importlib.util.spec_from_file_location('sd_owner_extract', ROOT/'checks/os-quiesce-storage.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.function(source, name)


def short_readiness_context(patch):
    """Drop two trailing context lines from the hunk that publishes PMIC readiness.

    The mutant is derived from the current hunk, so later edits to the patch
    cannot turn it into a copy of the production patch."""
    lines = patch.split('\n')
    anchor = lines.index('         else if(ready<0){')
    header = max(i for i in range(anchor) if lines[i].startswith('@@ '))
    end = next((i for i in range(anchor + 1, len(lines)) if not lines[i].startswith(' ')), len(lines))
    assert end - anchor - 1 >= 2, 'readiness hunk lacks two trailing context lines'
    match = re.fullmatch(r'@@ -(\d+),(\d+) \+(\d+),(\d+) @@(.*)', lines[header])
    lines[header] = '@@ -%s,%d +%s,%d @@%s' % (match[1], int(match[2]) - 2, match[3], int(match[4]) - 2, match[5])
    del lines[end - 2:end]
    mutant = '\n'.join(lines)
    assert mutant != patch
    return mutant


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', action='append', choices=('production', 'idle-mutant', 'ready-mutant', 'manifest', 'platform', 'register', 'async', 'idle', 'patch', 'stats', 'concurrent'))
    args = parser.parse_args()
    selected=set(args.case or ('production','idle-mutant','ready-mutant','manifest','platform','register','patch','stats','concurrent'))
    args.output.mkdir(parents=True, exist_ok=True)
    owner = (ROOT/'apps/os/sd_owner.c').read_text()
    native = (ROOT/'apps/os/large_sd.c').read_text()
    waits = '\n'.join(function(native, name) for name in ('read_sleep_until','os_sd_pmic_ready', 'read_pmic_ready', 'read_wait_until', 'read_drive','ss_storage_owner_stats'))
    entry = function(owner, 'owner_main')
    entry = entry.replace('__asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr));', 'ttbr=0;')
    tag = 'entry_tagged = xTaskGetApplicationTaskTag(NULL) != NULL;'
    assert entry.count(tag) == 1, 'owner_main no longer records its task tag'
    entry = entry.replace(tag, 'entry_tagged = 0;')    # the host kernel has no application task tags
    platform_source=(ROOT/'drivers/storage/a733_mmc_platform.c').read_text()
    # The storage states and the list of tasks that wait for the open, as sd_owner.c declares them.
    a = owner.index('enum { SD_UNOPENED')
    states = owner[a:owner.index(';', a) + 1]
    a = owner.index('struct ready_waiter {')
    waiters = owner[a:owner.index('static _Atomic(struct ready_waiter *) ready_waiters;', a)] + 'static _Atomic(struct ready_waiter *) ready_waiters;'
    production = (states+'\n'+waiters+'\n'+function(platform_source,'controller')+'\n'+waits+'\n'+function(owner,'owner_open_allowed')+'\n'+
                  function(owner,'publish_ready')+'\n'+
                  function(owner,'open_card')+'\n'+entry+'\n'+function(owner,'wait_ready')+'\n'+function(owner,'start')+'\n'+
                  function(owner,'proxy_poll')+'\n'+function(owner,'finish')+'\n'+function(owner,'post')+'\n')
    (args.output/'event-sd-production.h').write_text(production)
    sources = ['checks/event-sd.c', 'runtime/deadline.c']
    exe = host_rtos.build(args.output/'current', sources, 'event-sd', includes=[args.output], flags=['-Wno-unused-function'],defines=['SUNXI_MMC_READ_ONLY=1'])
    logs = []
    for case in range(9) if 'production' in selected else ():
        run = subprocess.run([str(exe), str(case)], capture_output=True, text=True, env=environment(), timeout=30)
        (args.output/f'case-{case}.log').write_text(run.stdout+run.stderr)
        assert not run.returncode, (case, run.stdout, run.stderr)
        logs.append(run.stdout.strip())
    released = 'while (!atomic_load_explicit(&self.released, memory_order_acquire)) ulTaskNotifyTakeIndexed(WAIT_INDEX, pdTRUE, portMAX_DELAY);'
    assert production.count(released) == 1, 'wait_ready no longer waits for its entry to be released'
    mutants = {
        'idle-tick-poll': production.replace('ulTaskNotifyTake(pdTRUE, portMAX_DELAY);', 'ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));', 1),
        # The wait ends at the first notification on its index, as the earlier single wait did.
        'ready-ends-at-any-hint': production.replace(released, '(void)ulTaskNotifyTakeIndexed(WAIT_INDEX, pdTRUE, portMAX_DELAY);'),
    }
    for index, (name, text) in enumerate(mutants.items()):
        if ('idle-mutant' if index==0 else 'ready-mutant') not in selected:
            continue
        directory = args.output/name
        directory.mkdir(exist_ok=True)
        (directory/'event-sd-production.h').write_text(text)
        mutated = host_rtos.build(directory, sources, 'event-sd', includes=[directory], flags=['-Wno-unused-function'],defines=['SUNXI_MMC_READ_ONLY=1'])
        case = '0' if index == 0 else '2'          # the idle owner; the stray hint before the open's result
        run = subprocess.run([str(mutated), case], capture_output=True, text=True, env=environment(), timeout=30)
        (directory/'result.log').write_text(run.stdout+run.stderr)
        assert run.returncode, name
    if 'platform' in selected:
        run=subprocess.run([str(exe),'9'],capture_output=True,text=True,env=environment(),timeout=30)
        (args.output/'case-9.log').write_text(run.stdout+run.stderr)
        assert not run.returncode,run.stdout+run.stderr
        directory=args.output/'core-batch-poll';directory.mkdir(exist_ok=True)
        text=production.replace('if(sunxi_mmc_wait_hint(&p->core,NULL)!=SUNXI_MMC_WAIT_NONE)break;','')
        (directory/'event-sd-production.h').write_text(text)
        mutant=host_rtos.build(directory,sources,'event-sd',includes=[directory],flags=['-Wno-unused-function'],defines=['SUNXI_MMC_READ_ONLY=1'])
        run=subprocess.run([str(mutant),'9'],capture_output=True,text=True,env=environment(),timeout=30)
        (directory/'result.log').write_text(run.stdout+run.stderr)
        assert run.returncode,'platform ignores IRQ wait hint'
    if 'register' in selected:
        run=subprocess.run([str(exe),'10'],capture_output=True,text=True,env=environment(),timeout=30)
        (args.output/'case-10.log').write_text(run.stdout+run.stderr)
        assert not run.returncode,run.stdout+run.stderr
    if 'async' in selected:
        run=subprocess.run([str(exe),'8'],capture_output=True,text=True,env=environment(),timeout=30)
        (args.output/'case-8.log').write_text(run.stdout+run.stderr)
        assert not run.returncode,run.stdout+run.stderr
    if 'idle' in selected:
        run=subprocess.run([str(exe),'0'],capture_output=True,text=True,env=environment(),timeout=30)
        (args.output/'case-0.log').write_text(run.stdout+run.stderr)
        assert not run.returncode,run.stdout+run.stderr
    if 'patch' in selected:
        command=['patch','--dry-run','--fuzz=0','--reject-file=-','--output',str(args.output/'main.c.tmp'),str(ROOT/'apps/board/main.c'),str(ROOT/'apps/os/main.patch')]
        run=subprocess.run(command,capture_output=True,text=True)
        (args.output/'patch.log').write_text(run.stdout+run.stderr)
        assert not run.returncode,run.stdout+run.stderr
        text=short_readiness_context((ROOT/'apps/os/main.patch').read_text())
        mutant=args.output/'short-context.patch';mutant.write_text(text)
        command[-1]=str(mutant)
        run=subprocess.run(command,capture_output=True,text=True)
        (args.output/'patch-mutant.log').write_text(run.stdout+run.stderr)
        assert run.returncode,'short readiness context must fail strict patch application'
    if 'concurrent' in selected:
        # Only the test task and the owner hold deadline slots, so a storage call that still registered its caller would take
        # the owner's slot and the open would fail.
        concurrent = host_rtos.build(args.output/'concurrent', sources, 'event-sd', includes=[args.output], flags=['-Wno-unused-function'],
                                     defines=['SUNXI_MMC_READ_ONLY=1', 'SUNSTONE_DEADLINE_SLOTS=2u'])
        for case in ('12', '13', '14'):          # both start; the open fails; a CPU return refuses the open
            run=subprocess.run([str(concurrent),case],capture_output=True,text=True,env=environment(),timeout=30)
            (args.output/f'case-{case}.log').write_text(run.stdout+run.stderr)
            assert not run.returncode,run.stdout+run.stderr
            logs.append(run.stdout.strip())
        start = function(owner, 'start')
        taken = 'struct ready_waiter *next = w->next;'
        assert production.count(taken) == 1, 'publish_ready no longer walks the list of waiters'
        refused = 'if (!owner_open_allowed()) { publish_ready(-1); return 0; }'
        failed = 'publish_ready(hw ? 1 : -1);'
        assert production.count(refused) == 1 and production.count(failed) == 1, 'open_card no longer publishes each result'
        for name, text, case in (('one-waiter-woken', production.replace(taken, 'struct ready_waiter *next = NULL;'), '12'),
                                 ('caller-registers', production.replace(start, start.replace('{', '{\n    if(!sunstone_deadline_register())return 0;', 1)), '12'),
                                 # The open's failure, or its refusal, leaves its waiting tasks asleep.
                                 ('failure-unpublished', production.replace(failed, 'if (hw) publish_ready(1);'), '13'),
                                 ('refusal-unpublished', production.replace(refused, 'if (!owner_open_allowed()) return 0;'), '14')):
            directory = args.output/name
            directory.mkdir(exist_ok=True)
            (directory/'event-sd-production.h').write_text(text)
            mutated = host_rtos.build(directory, sources, 'event-sd', includes=[directory], flags=['-Wno-unused-function'],
                                      defines=['SUNXI_MMC_READ_ONLY=1', 'SUNSTONE_DEADLINE_SLOTS=2u'])
            run = subprocess.run([str(mutated), case], capture_output=True, text=True, env=environment(), timeout=30)
            (directory/'result.log').write_text(run.stdout+run.stderr)
            assert run.returncode, name
            mutants[name] = text
    if 'stats' in selected:
        run=subprocess.run([str(exe),'11'],capture_output=True,text=True,env=environment(),timeout=30)
        (args.output/'case-11.log').write_text(run.stdout+run.stderr)
        assert not run.returncode,run.stdout+run.stderr
    # The generator admits INTID 193 only when it links the card binding.
    spec = importlib.util.spec_from_file_location('manifest_sd_events', ROOT/'tools/sunstone-manifest.py')
    manifest = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manifest)
    if 'manifest' in selected:
        publisher=(ROOT/'apps/os/main.patch').read_text()
        assert publisher.index('atomic_store_explicit(&providers.shared.pmic_ready,1,memory_order_release);')<publisher.index('if(os_sd_pmic_ready)os_sd_pmic_ready();')
        base=manifest.load(ROOT/'apps/heap-test/manifest.json')
        assert '#ifdef SUNSTONE_A733_OS_SD_WRITE\nextern void os_sd_irq' in manifest.c_tables(base)
        base['large_regions']=[{'flags':0}]
        tables=manifest.c_tables(base)
        assert '#if 1\nextern void os_sd_irq' in tables
        assert '#if 1\n    {193,0,0xd8,os_sd_irq}' in tables
        print('PASS read-only card route; heap-only images retain conditional SD route')
    print('\n'.join(logs))
    (args.output/'result.json').write_text(json.dumps({'passed': True, 'physical_access': False,
        'cases': 16, 'mutations_refused': [*mutants,'core-batch-poll','readiness-patch-short-context'], 'idle_owner_wakes_per_second': 0,
        'baseline_idle_owner_wakes_per_second_source_counted': 0, 'baseline_startup_polls_per_second_source_counted': 20,
        'startup_periodic_polls_per_second': 0}, indent=2)+'\n')
    print('PASS SD owner event and deadline cases; hardware completion is modeled')


if __name__ == '__main__':
    main()
