"""Validate the complete returning CPU workload and its frozen instruction evidence."""
import importlib.util
import json
from pathlib import Path
import struct
from types import SimpleNamespace
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('first_task_host', ROOT/'tools/all-core-first-task-host.py')
base = importlib.util.module_from_spec(spec); spec.loader.exec_module(base)
require, digest = base.require, base.digest
PROFILE = 'all-core-task-workload-return-v1'
SOFTWARE_SCHEMA = 'a733-all-core-task-workload-software-v1'
HOST_SOURCES = base.HOST_SOURCES+('tools/all-core-task-workload-host.py',)
MODEL_SOURCES = base.MODEL_SOURCES+('checks/all-core-task-workload-probe.py',)
CASES = base.CASES+('wrong-workload-result-refused', 'wrong-arrival-refused',
                    'timer-priority-restore-refused', 'changed-instruction-evidence-refused')


def decode(raw, nonce, manifest):
    result = base.decode(raw, nonce, manifest, task_workload=True)
    workers = []
    for core in range(8):
        rounds, background, sgis, failed, ticks = struct.unpack_from('<5Q', raw, 0x4280+core*128)
        require(rounds == 32 and background > 0 and sgis > 0 and failed == 0,
                'Worker rounds, progress, SGIs or failure differ.')
        require(ticks >= 32 if core == 0 else ticks == 0, 'Workload timer counts differ.')
        workers.append(dict(core=core, rounds=rounds, background_progress=background,
                            sgis=sgis, failure=failed, ticks=ticks))
    result.update(task_workload_completed=True, worker_bodies_executed=True,
                  timer_and_sgi_workload_executed=True, workers=workers,
                  secure_timer_priorities_restored=True)
    return result


probe = SimpleNamespace(NATIVE=base.probe.NATIVE, REPORT=base.probe.REPORT, decode=decode)
initial_report = base.initial_report


def pinned_symbol(image, name):
    """The address of NAME in the pinned native ELF, which must be that image."""
    require(digest(image) == probe.NATIVE, 'Unreviewed native image.')
    with image.open('rb') as stream:
        found = [s['st_value'] for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()
                 if s.name == name and s['st_shndx'] != 'SHN_UNDEF']
    require(len(found) == 1, 'Missing or ambiguous native symbol '+name)
    return found[0]


def load(build, image, manifest, **deadline):
    require(manifest.get('task_switch') == {
        'physical_eligible': False, 'worker_body': True, 'idle_body': True,
        'timer_and_sgi_workload': True,
        'pending': 'Instruction execution, host report validation and root review required.'},
        'Workload task-switch scope changed.')
    require(manifest.get('task_workload') == {
        'physical_eligible': False, 'rounds_per_core': 32, 'completion_patch': pinned_symbol(image, 'worker')+0x6c,
        'pending': 'Returning workload instruction checks, host validation and root review required.'},
        'Workload scope or completion patch changed.')
    require(manifest['native_symbols'].get('a733_smp_core_results') == 0x41201080,
            'Worker result location changed.')
    return base.load(build, image, manifest, task_workload=True, **deadline)


def validate_software(software, manifest_hash):
    require(software.get('schema') == SOFTWARE_SCHEMA and software.get('physical_access') is False and
            software.get('manifest_sha256') == manifest_hash and
            software.get('host_sources') == {n: digest(ROOT/n) for n in HOST_SOURCES} and
            software.get('host_cases') == dict.fromkeys(CASES, True), 'Workload host evidence differs.')
    evidence = software['instruction_evidence']
    require(set(evidence) == {'success', 'wrong-rounds'}, 'Workload instruction cases incomplete.')
    for case, item in evidence.items():
        path = Path(item['path'])
        require(path.is_absolute() and digest(path) == item['sha256'], 'Instruction evidence changed.')
        result = json.loads(path.read_text())
        require(result.get('physical_access') is False and result.get('pass') is True and
                result.get('case') == case and result.get('native_sha256') == probe.NATIVE and
                result.get('manifest_sha256') == manifest_hash and
                result.get('sources') == {n: digest(ROOT/n) for n in MODEL_SOURCES},
                'Workload instruction inputs or result differ.')
        report = path.with_suffix('.bin')
        require(digest(report) == result['report_sha256'], 'Instruction report changed.')
        if case == 'success':
            require(result.get('off') == list(range(1, 8)) and
                    result.get('clean_complete') == result.get('el0_wait_cores') == list(range(8)),
                    'Workload completion or permitted EL0 waits incomplete.')
            actual = [list(struct.unpack_from('<5Q', report.read_bytes(), 0x4280+c*128)) for c in range(8)]
            require(result.get('workload_results') == actual and len(result.get('interrupt_counts', [])) == 8,
                    'Workload summary differs from copied memory.')
            for core, (rounds, background, sgis, failed, ticks) in enumerate(actual):
                # A late tick IRQ makes up to TICK_CALLS_PER_IRQ (3) tick handler
                # calls (platform/a733-smp-cached/platform.c), so CPU0 takes
                # between ticks/3 and ticks tick IRQs; every other IRQ is an SGI.
                irqs = result['interrupt_counts'][core]
                require(rounds == 32 and background > 0 and sgis > 0 and failed == 0 and
                        (ticks >= 32 if core == 0 else ticks == 0) and
                        sgis+(ticks+2)//3 <= irqs <= sgis+ticks,
                        'Instruction worker or interrupt counts differ.')
        else:
            require(result.get('off') == [] and result.get('clean_complete') == [],
                    'Damaged workload result did not retain all cores.')
            checkpoint = Path(result['workload_checkpoint'])
            saved = json.loads(checkpoint.with_suffix('.json').read_text())
            require(checkpoint.is_absolute() and digest(checkpoint) == saved['snapshot_sha256'] and
                    saved['native_sha256'] == probe.NATIVE and saved['manifest_sha256'] == manifest_hash and
                    saved['checker_sha256'] == digest(ROOT/'checks/all-core-task-workload-probe.py') and
                    saved['sources'] == {n: digest(ROOT/n) for n in base.MODEL_SOURCES
                                        if n != 'checks/all-core-first-task-probe.py'},
                    'Completed-worker checkpoint inputs changed.')
