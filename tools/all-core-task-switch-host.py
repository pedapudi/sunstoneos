"""Bind native idle-to-worker switching to its report and returning runner."""
import importlib.util
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
    switching = {k:v for k,v in (manifest.get('task_switch') or {}).items()
                 if k not in ('pending','physical_eligible')}
    require(switching == {'worker_body': False,'idle_body': True,
            'timer_and_sgi_workload': False}, 'Task-switch scope changed.')
    return base.load(build, image, manifest, task_switch=True)
