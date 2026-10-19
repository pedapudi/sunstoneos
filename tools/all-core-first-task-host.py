"""Validate first FreeRTOS task contexts and complete owned-memory cleanup."""
import importlib.util
import json
from pathlib import Path
import struct
from types import SimpleNamespace
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('pre_scheduler_host', ROOT/'tools/all-core-pre-scheduler-host.py')
base = importlib.util.module_from_spec(spec); spec.loader.exec_module(base)
require, digest = base.require, base.digest
PROFILE = 'all-core-first-task-return-v1'
HOST_SOURCES = base.HOST_SOURCES+('tools/all-core-first-task-host.py',)
MODEL_SOURCES = ('checks/all-core-first-task-probe.py',)+base.MODEL_SOURCES
CASES = ('complete-return', 'transport-failure-retained', 'invalid-report-restored',
         'changed-input-refused', 'incomplete-primary-refused', 'incomplete-participant-refused',
         'wrong-task-context-refused', 'wrong-saved-frame-refused', 'wrong-memory-result-refused')
# Earlier negative cases remain valid: the sole later checker change rejects
# writes by every CPU/EL after cleanup starts. Executable inputs are unchanged.
PRIOR_CHECKER = '113e633601cb7fba102f308369e07f41d3e350bbb3d61d307f3b02de02194210'
GEOMETRY = {
    'vApplicationGetIdleTaskMemory.xIdleTaskTCB': (0x41288618, 168),
    'vApplicationGetIdleTaskMemory.uxIdleTaskStack': (0x412886c0, 4096),
    'vApplicationGetPassiveIdleTaskMemory.xIdleTaskTCBs': (0x412896c0, 1176),
    'vApplicationGetPassiveIdleTaskMemory.uxIdleTaskStacks': (0x41289b58, 28672),
}


def task_context(core, task_switch=False):
    if task_switch:
        return 0x41241498+core*168, (0x41201498+(core+1)*16384-8) & ~15
    tcb = 0x41288618 if core == 0 else 0x412896c0+(core-1)*168
    stack = 0x412886c0 if core == 0 else 0x41289b58+(core-1)*4096
    return tcb, (stack+4096-8) & ~15


def decode(raw, nonce, manifest, task_switch=False, task_workload=False):
    task_switch = task_switch or task_workload
    hooks = manifest['hooks']
    result = base.decode(raw, nonce, hooks['el1_smc_return'], first_task=True, task_workload=task_workload)
    epoch, deadline = result['epoch'], result['startup_deadline']
    entries = []
    for core in range(8):
        tcb, stack = task_context(core, task_switch)
        if task_workload: stack -= 64  # Frozen worker prologue, retained at post-loop branch.
        record = struct.unpack_from('<8Q', raw, 0x3200+core*64)
        require(record == (epoch, 1, hooks['identity_first_task_el0']+4, stack, 0x60000000 if task_workload else 0, tcb, core if task_switch else 0, 0x30d41805),
                'First-task context, publication or SCTLR differs.')
        saved = struct.unpack_from('<32Q', raw, 0x3a00+core*256)
        require(saved[:8] == (hooks['el1_observe_checkpoint']+4, 0x4124b000+core*32768,
                core, epoch, deadline, deadline, 0x03460000+core*0x20000, 0x4100a000) and
                not any(saved[8:]), 'Saved returning context differs.')
        entries.append({'core': core, 'tcb': tcb, 'task_sp': stack, 'task_spsr': record[4]})
    if task_workload:
        require(struct.unpack_from('<8Q', raw, 0x3600) == (epoch,)*8,
                'Worker arrival epochs differ.')
        require(not any(raw[0x3400:0x3600]) and not any(raw[0x3640:0x3a00]),
                'Workload completion-page padding changed.')
    else:
        require(not any(raw[0x3400:0x3a00]), 'Completion-page padding changed.')
    require(struct.unpack_from('<19I', raw, 0x4200) == tuple([1]*8+[256, 0]+[1]*8+[1]),
            'Production pre-scheduler state differs.')
    # The remainder contains real task/kernel state, so it must not be zeroed
    # or compared with the earlier pre-scheduler-only page fixture.
    result.update(first_task_entry_completed=True, task_contexts=entries,
                  owned_writable_interval=[0x41200000, 0x41291000],
                  task_bodies_executed=False, workload_scheduling_qualified=False)
    if task_switch:
        result.update(task_switch_completed=True, task_bodies_executed=True,
                      idle_bodies_executed=True, worker_bodies_executed=False)
    return result


probe = SimpleNamespace(NATIVE=base.probe.NATIVE, REPORT=base.probe.REPORT, decode=decode)
initial_report = base.initial_report


def load(build, image, manifest, task_switch=False, task_workload=False):
    task_switch = task_switch or task_workload
    frozen, dram = base.cache.local.identity.load(build, image, manifest,
                                                  first_task=True, task_switch=task_switch,
                                                  task_workload=task_workload)
    require(manifest.get('first_task') == {
        'physical_eligible': False, 'writable_interval': [0x41200000, 0x41291000],
        'cache_lines_per_core': 9280, 'task_bodies': task_workload, 'timer_and_sgi_workload': task_workload,
        'pending': 'Instruction execution, return validation and source review required before physical use.'},
        'First-task scope changed.')
    with image.open('rb') as stream:
        elf = ELFFile(stream)
        symbols = list(elf.get_section_by_name('.symtab').iter_symbols())
        geometry = {'worker': (0x41001280,372), 'tasks': (0x41241498,2688),
                    'stacks': (0x41201498,262144)} if task_switch else GEOMETRY
        for name, expected in geometry.items():
            require([(s['st_value'], s['st_size']) for s in symbols if s.name == name] == [expected],
                    'Task layout changed: '+name)
        data, bss = (elf.get_section_by_name(n) for n in ('.data', '.bss'))
        require(data['sh_addr'] == 0x41200000 and data['sh_addr']+data['sh_size'] <= 0x41201000 and
                bss['sh_addr'] == 0x41201000 and bss['sh_addr']+bss['sh_size'] == 0x41290d10,
                'Owned writable extent changed.')
    return frozen, dram


def validate_software(software, manifest_hash):
    require(software.get('schema') == 'a733-all-core-first-task-software-v1' and
            software.get('physical_access') is False and software.get('manifest_sha256') == manifest_hash and
            software.get('host_sources') == {n: digest(ROOT/n) for n in HOST_SOURCES} and
            software.get('host_cases') == dict.fromkeys(CASES, True), 'First-task host evidence differs.')
    evidence = software['instruction_evidence']
    require(set(evidence) == {'success', 'wrong-task-argument', 'restore-el3'}, 'Instruction cases incomplete.')
    for case, item in evidence.items():
        path = Path(item['path'])
        require(path.is_absolute() and digest(path) == item['sha256'], 'Instruction evidence changed.')
        result = json.loads(path.read_text())
        sources = {n: digest(ROOT/n) for n in MODEL_SOURCES}
        if case != 'success':
            sources['checks/all-core-first-task-probe.py'] = PRIOR_CHECKER
        require(result.get('physical_access') is False and result.get('pass') is True and
                result.get('native_sha256') == probe.NATIVE and result.get('manifest_sha256') == manifest_hash and
                result.get('sources') == sources, 'Instruction inputs or result differ.')
        if case == 'success':
            require(result.get('clean_lines_per_core') == 9280 and result.get('off') == list(range(1, 8)) and
                    set(result.get('entries', {})) == set(map(str, range(8))), 'Instruction success is incomplete.')
            report = path.with_suffix('.bin')
            require(digest(report) == result['report_sha256'], 'Instruction report changed.')
        else:
            require(result.get('case') == case and result.get('off') == [] and
                    result.get('clean_complete') == ([] if case == 'wrong-task-argument' else list(range(8))),
                    'Instruction failure did not retain the expected state.')
