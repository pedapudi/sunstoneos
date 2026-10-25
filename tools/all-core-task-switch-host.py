"""Bind native idle-to-worker switching to its report and returning runner."""
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('first_task_host', ROOT/'tools/all-core-first-task-host.py')
base = importlib.util.module_from_spec(spec); spec.loader.exec_module(base)
require, digest = base.require, base.digest
PROFILE = 'all-core-task-switch-return-v1'
HOST_SOURCES = base.HOST_SOURCES+('tools/all-core-task-switch-host.py',)
MODEL_SOURCES, CASES = base.MODEL_SOURCES, base.CASES
SOFTWARE_SCHEMA = 'a733-all-core-task-switch-software-v1'


def decode(raw, nonce, manifest):
    return base.decode(raw, nonce, manifest, task_switch=True)


probe = SimpleNamespace(NATIVE=base.probe.NATIVE, REPORT=base.probe.REPORT, decode=decode)
initial_report = base.initial_report


def load(build, image, manifest):
    require(manifest.get('task_switch') == {
        'physical_eligible': False, 'worker_body': False, 'idle_body': True,
        'timer_and_sgi_workload': False,
        'pending': 'Instruction execution, host report validation and root review required.'},
        'Task-switch scope changed.')
    return base.load(build, image, manifest, task_switch=True)


def validate_software(software, manifest_hash):
    require(software.get('schema') == SOFTWARE_SCHEMA and software.get('physical_access') is False and
            software.get('manifest_sha256') == manifest_hash and
            software.get('host_sources') == {n:digest(ROOT/n) for n in HOST_SOURCES} and
            software.get('host_cases') == dict.fromkeys(CASES, True), 'Task-switch host evidence differs.')
    evidence = software['instruction_evidence']
    require(set(evidence) == {'success','wrong-task-argument','idle-resumes'}, 'Task-switch instruction cases incomplete.')
    for case,item in evidence.items():
        path = Path(item['path'])
        require(path.is_absolute() and digest(path) == item['sha256'], 'Instruction evidence changed.')
        result = json.loads(path.read_text())
        require(result.get('physical_access') is False and result.get('pass') is True and
                result.get('native_sha256') == probe.NATIVE and
                result.get('manifest_sha256') == manifest_hash and
                result.get('sources') == {n:digest(ROOT/n) for n in MODEL_SOURCES},
                'Task-switch instruction inputs or result differ.')
        if case in ('success', 'idle-resumes'):
            # idle-resumes: one idle task's first yield returns to it, and its
            # loop must still reach the worker without an interrupt.
            require(case == 'success' or result.get('case') == case and result.get('idle_resumed_core') == 3,
                    'Resumed idle task case differs.')
            require(result.get('idle_yield_cores') == result.get('worker_selection_cores') == list(range(8)) and
                    result.get('worker_bodies_executed') is False and
                    result.get('clean_lines_per_core') == 9280 and result.get('off') == list(range(1,8)),
                    'Task-switch instruction completion differs.')
            report = path.with_suffix('.bin')
            require(digest(report) == result['report_sha256'], 'Instruction report changed.')
        else:
            require(result.get('case') == case and result.get('off') == [] and
                    result.get('clean_complete') == [], 'Invalid task argument did not retain all cores.')
