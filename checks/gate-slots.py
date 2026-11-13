#!/usr/bin/env python3
"""The host's gate slots, the landing priority and the timing lock (tools/gate_slots.py, tools/gate-slots.json).

Every case runs real processes on a temporary home directory, so the file locks and the tickets are the ones a gate uses:
  derivation   the slot count and the jobs per slot follow from the measured numbers of tools/gate-slots.json: the stated host gives
               three slots, a host with less memory or fewer cores gives fewer, and a different measurement gives a different count;
  two-gates    three worker gates start: two run at once in slots 1 and 2, never in slot 0, and the third waits until one ends;
  timing-lock  two gates enter the timing lock: the second waits until the first leaves, and a gate that holds a slot is not
               blocked by it; the scheduler perturbation sweep, the soak and the SERIAL_CHECKS of the index are timing steps;
  landing-jump slots held by a landing and two workers; an older worker and a newer landing wait: the slot a worker frees goes to
               the landing, then the next free slot to the worker, and a landing takes slot 0 when it frees;
  stale-ticket a waiting gate that is killed leaves no ticket behind that holds the queue.
Usage: gate-slots.py --output DIR
"""
import argparse
import importlib.util
import json
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import gate_slots

CONFIG = gate_slots.load_config()

HOLDER = '''
import sys
sys.path.insert(0, %r)
import gate_slots
config = gate_slots.load_config()
queue = gate_slots.SlotQueue(%r, config, slots=%d, poll=0.05)
slot = queue.acquire(%r, %r, announce=None)
print("SLOT %%d" %% slot.index, flush=True)
sys.stdin.readline()
slot.close()
print("DONE", flush=True)
'''
TIMER = '''
import sys
sys.path.insert(0, %r)
import gate_slots
with gate_slots.timing_lock(%r, %r, announce=None):
    print("IN", flush=True)
    sys.stdin.readline()
print("OUT", flush=True)
'''


class Process:
    def __init__(self, source):
        self.process = subprocess.Popen([sys.executable, '-B', '-c', source], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def line(self, timeout):
        """The next output line, or None when none arrives within timeout seconds."""
        ready, _, _ = select.select([self.process.stdout], [], [], timeout)
        return self.process.stdout.readline().strip() if ready else None

    def release(self):
        self.process.stdin.write('\n')
        self.process.stdin.flush()

    def end(self):
        self.process.kill()
        self.process.wait()


def holder(home, slots, priority, label):
    return Process(HOLDER % (str(ROOT / 'tools'), str(home), slots, priority, label))


def timer(home, label):
    return Process(TIMER % (str(ROOT / 'tools'), str(home), label))


def case_derivation():
    n = gate_slots.slot_count(CONFIG, 24, 122)
    assert n == 3, 'the measured host should give three slots, not %d' % n
    assert gate_slots.slot_count(CONFIG, 24, 40) < n, 'less memory must give fewer slots'
    assert gate_slots.slot_count(CONFIG, 12, 122) < n, 'fewer cores must give fewer slots'
    assert gate_slots.slot_count(CONFIG, 8, 8) == 1, 'a small host keeps one slot'
    heavier = json.loads(json.dumps(CONFIG))
    heavier['gate']['mean_cores'] = CONFIG['gate']['mean_cores'] * 2
    assert gate_slots.slot_count(heavier, 24, 122) < n, 'a heavier measured gate must give fewer slots'
    jobs = gate_slots.jobs_per_slot(CONFIG, n, 24)
    assert 2 <= jobs <= 24 and jobs * n <= 24, 'the jobs per slot oversubscribe the cores: %d x %d' % (jobs, n)
    assert gate_slots.reserved_slots(CONFIG, n) == 1 and gate_slots.reserved_slots(CONFIG, 1) == 0


def case_two_gates(home):
    first, second, third = (holder(home, 3, gate_slots.WORKER, 'worker-%d' % i) for i in range(3))
    try:
        got = sorted([first.line(10), second.line(10)])
        assert got == ['SLOT 1', 'SLOT 2'], 'two workers should run at once in slots 1 and 2: %s' % got
        assert third.line(1.0) is None, 'a third worker must wait while both worker slots are held'
        first.release()
        assert first.line(10) == 'DONE'
        assert third.line(10) in ('SLOT 1', 'SLOT 2'), 'the waiting worker did not take the slot that was freed'
        for process in (second, third):
            process.release()
    finally:
        for process in (first, second, third):
            process.end()


def case_timing_lock(home):
    one, two = timer(home, 'one'), timer(home, 'two')
    worker = holder(home, 3, gate_slots.WORKER, 'beside the timing lock')
    try:
        assert one.line(10) == 'IN'
        assert worker.line(10) is not None, 'a gate in a slot was blocked by the timing lock'
        assert two.line(1.0) is None, 'two gates ran a wall-time step at once'
        one.release()
        assert one.line(10) == 'OUT'
        assert two.line(10) == 'IN', 'the second gate did not enter after the first left'
        two.release()
    finally:
        for process in (one, two, worker):
            process.end()
    serial = set(('cpu-pmu-smp.py', 'smp-port-cost.py', 'pcie-qemu.py', 'npu-runtime-bounds.py'))
    for name, script in (('image-scheduler-control-linked', 'scheduler-control-linked.py'), ('check-soak8', None),
                         ('indexed-smp-ticket-lock', 'smp-ticket-lock.py'), ('indexed-pcie-qemu', 'pcie-qemu.py')):
        assert gate_slots.is_timing_step(CONFIG, name, script, serial), name + ' is a wall-time step'
    assert not gate_slots.is_timing_step(CONFIG, 'indexed-memory-layout', 'memory-layout.py', serial)
    for script in CONFIG['timing_checks']:
        assert (ROOT / 'checks' / script).is_file(), script + ' is listed as a timing check but is not in checks/'
    spec = importlib.util.spec_from_file_location('check_index', ROOT / 'checks/check-index.py')
    index = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(index)
    assert set(index.SERIAL_CHECKS) <= gate_slots.timing_scripts(CONFIG, index.SERIAL_CHECKS)


def case_landing_jump(home):
    landing = holder(home, 3, gate_slots.LANDING, 'landing-1')
    worker_one = holder(home, 3, gate_slots.WORKER, 'worker-1')
    worker_two = holder(home, 3, gate_slots.WORKER, 'worker-2')
    waiting = []
    try:
        assert landing.line(10) == 'SLOT 0', 'a landing should take the slot that only landings use'
        assert worker_one.line(10) == 'SLOT 1' and worker_two.line(10) == 'SLOT 2'
        older_worker = holder(home, 3, gate_slots.WORKER, 'worker-3')
        waiting.append(older_worker)
        assert older_worker.line(1.0) is None
        time.sleep(0.2)
        newer_landing = holder(home, 3, gate_slots.LANDING, 'landing-2')
        waiting.append(newer_landing)
        assert newer_landing.line(1.0) is None, 'every slot is held; the landing must wait'
        worker_one.release()
        assert worker_one.line(10) == 'DONE'
        assert newer_landing.line(10) == 'SLOT 1', 'the landing did not go ahead of the worker that waited longer'
        assert older_worker.line(1.0) is None, 'the older worker took a slot before the landing'
        worker_two.release()
        assert worker_two.line(10) == 'DONE'
        assert older_worker.line(10) == 'SLOT 2', 'the worker did not take the next free worker slot'
        landing.release()
        assert landing.line(10) == 'DONE'
        for process in (newer_landing, older_worker):
            process.release()
    finally:
        for process in (landing, worker_one, worker_two, *waiting):
            process.end()


def case_stale_ticket(home):
    blocker = holder(home, 2, gate_slots.WORKER, 'blocker')
    dead = nxt = None
    try:
        assert blocker.line(10) == 'SLOT 1'
        dead = holder(home, 2, gate_slots.WORKER, 'killed while waiting')
        assert dead.line(1.0) is None
        time.sleep(0.2)
        nxt = holder(home, 2, gate_slots.WORKER, 'next')
        assert nxt.line(1.0) is None
        dead.end()
        blocker.release()
        assert blocker.line(10) == 'DONE'
        assert nxt.line(10) == 'SLOT 1', 'a killed gate left a ticket that held the queue'
        assert not [p for p in (home / 'gate-queue').glob('*.ticket') if 'killed' in p.read_text()], 'the stale ticket stayed'
        nxt.release()
    finally:
        for process in (blocker, dead, nxt):
            if process:
                process.end()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    case_derivation()
    print('PASS derivation')
    for name, case in (('two-gates', case_two_gates), ('timing-lock', case_timing_lock), ('landing-jump', case_landing_jump),
                       ('stale-ticket', case_stale_ticket)):
        home = args.output / name
        home.mkdir()
        case(home)
        print('PASS ' + name)


if __name__ == '__main__':
    main()
