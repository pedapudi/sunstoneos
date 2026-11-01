"""Validate the native layout and report of the complete returning CPU workload."""
import importlib.util
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
                    'timer-priority-restore-refused')


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


def native_symbol(image, name):
    """Read one defined symbol from the actual selected native ELF."""
    with image.open('rb') as stream:
        found = [s['st_value'] for s in ELFFile(stream).get_section_by_name('.symtab').iter_symbols()
                 if s.name == name and s['st_shndx'] != 'SHN_UNDEF']
    require(len(found) == 1, 'Missing or ambiguous native symbol '+name)
    return found[0]


def load(build, image, manifest, **deadline):
    switching = {k:v for k,v in (manifest.get('task_switch') or {}).items()
                 if k not in ('pending','physical_eligible')}
    workload = {k:v for k,v in (manifest.get('task_workload') or {}).items()
                if k not in ('pending','physical_eligible')}
    require(switching == {'worker_body': True,'idle_body': True,
            'timer_and_sgi_workload': True}, 'Workload task-switch scope changed.')
    require(workload == {'rounds_per_core': 32,
            'completion_patch': native_symbol(image,'worker')+0x6c},
            'Workload scope or completion patch changed.')
    require(manifest['native_symbols'].get('a733_smp_core_results') == 0x41201080,
            'Worker result location changed.')
    return base.load(build, image, manifest, task_workload=True, **deadline)
