"""The host's gate slots, the landing priority and the timing lock (docs/validation.md, "Gate slots").

Gates used to run one at a time behind CACHE/gate.lock. A gate is mostly one or two busy cores for most of its length, so the
host runs N of them at once: CACHE/gate.lock.0 to gate.lock.N-1, each held with flock by the gate that runs in it. N and the
-j that a slot gives its build and check workers come from the measurements in tools/gate-slots.json, not from constants in
code (slot_count, jobs_per_slot).

A gate that waits writes a ticket (CACHE/gate-queue/CLASS-TIME-PID.ticket) and holds a flock on it for as long as it waits, so a
ticket whose owner died is recognised and removed. Every waiter orders the live tickets the same way, landings before workers
and then oldest first, and gives each ticket in turn the first free slot it may use; it takes a slot only when that walk hands
it one. A landing may use any slot and prefers slot 0, which workers never use (config: landing_reserved_slots), so a landing
never waits behind workers and jumps a worker that has waited longer for a slot both may use.

The steps whose result depends on wall time (config: timing_checks, timing_steps) hold the exclusive CACHE/gate-timing.lock while
they run, so no two gates run such a step at once. Other work of other gates still runs beside them.
"""
import contextlib
import fcntl
import json
import os
from pathlib import Path
import time

CONFIG = Path(__file__).with_name('gate-slots.json')
LANDING, WORKER = 'landing', 'worker'
CLASS = {LANDING: 0, WORKER: 1}


def load_config(path=CONFIG):
    return json.loads(Path(path).read_text())


def host_facts():
    """The cores and the MemTotal in GiB of this host."""
    for line in Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemTotal:'):
            return os.cpu_count() or 1, int(line.split()[1]) / 1048576
    return os.cpu_count() or 1, 0.0


def usable_cores(config, cores):
    return max(1.0, cores - config['reserve']['other_cores'])


def slot_count(config, cores=None, mem_gb=None):
    """The slots the host can run: the fewer of what the cores and the memory allow, at least one.

    A gate averages `gate.mean_cores` busy cores and peaks briefly above that while it builds, so the cores are divided by the mean;
    its peak resident set (the whole process tree) is added up, because the gates peak together in the worst case, and the
    memory the checks' own guard keeps free (reserve.mem_gb) is left alone."""
    if cores is None or mem_gb is None:
        cores, mem_gb = host_facts()
    by_cores = int(usable_cores(config, cores) // config['gate']['mean_cores'])
    by_memory = int((mem_gb - config['reserve']['mem_gb']) // config['gate']['peak_rss_gb'])
    return max(1, min(by_cores, by_memory, config.get('max_slots', by_cores)))


def jobs_per_slot(config, slots, cores=None):
    """The -j of a gate's builds and the default --jobs of its checks: the usable cores divided across the slots."""
    if cores is None:
        cores = host_facts()[0]
    return max(2, int(usable_cores(config, cores) // slots))


def reserved_slots(config, slots):
    """Slots that only landings use; always leaves at least one for workers."""
    return min(config['landing_reserved_slots'], slots - 1)


def timing_scripts(config, serial=()):
    return frozenset(config['timing_checks']) | frozenset(serial)


def is_timing_step(config, name, script, serial=()):
    return (script in timing_scripts(config, serial)) or name in config['timing_steps']


def _open(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    return open(path, 'a+')


def _describe(handle, text):
    handle.seek(0)
    handle.truncate()
    handle.write(text)
    handle.flush()


def _held_by(handle):
    handle.seek(0)
    return handle.read().strip() or 'another gate'


class Slot:
    """A held gate slot; the flock lasts as long as this object stays open."""
    def __init__(self, index, handle):
        self.index, self.handle = index, handle

    def close(self):
        if self.handle:
            self.handle.close()
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class SlotQueue:
    def __init__(self, cache, config=None, slots=None, poll=1.0):
        self.cache = Path(cache)
        self.config = config or load_config()
        self.slots = slots if slots is not None else slot_count(self.config)
        self.poll = poll
        self.directory = self.cache / 'gate-queue'

    def slot_path(self, index):
        return self.cache / ('gate.lock.%d' % index)

    def order(self, priority):
        """The slots a gate of this priority may take, best first."""
        reserved = reserved_slots(self.config, self.slots)
        return list(range(self.slots)) if priority == LANDING else list(range(reserved, self.slots))

    def _queue_lock(self):
        handle = _open(self.cache / 'gate-queue.lock')
        fcntl.flock(handle, fcntl.LOCK_EX)
        return handle

    def _live(self, mine):
        """The tickets whose owners still wait, oldest landing first; a ticket whose owner died is removed."""
        live = []
        for path in sorted(self.directory.glob('*.ticket')):
            if path == mine:
                live.append(path)
                continue
            try:
                handle = open(path, 'a+')
            except OSError:
                continue
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                live.append(path)
            else:
                path.unlink(missing_ok=True)
            finally:
                handle.close()
        return live

    def _priority_of(self, ticket):
        return LANDING if ticket.name.startswith('0-') else WORKER

    def acquire(self, priority, label, announce=print):
        """Block until this gate holds a slot; returns the Slot."""
        self.directory.mkdir(parents=True, exist_ok=True)
        ticket = self.directory / ('%d-%020d-%d.ticket' % (CLASS[priority], time.time_ns(), os.getpid()))
        # Created and locked under the queue lock: a waiter that lists the tickets between the creation and the flock would take
        # this one for a dead gate's and remove it.
        queue = self._queue_lock()
        try:
            held = open(ticket, 'a+')
            fcntl.flock(held, fcntl.LOCK_EX)
            _describe(held, 'pid %d %s %s\n' % (os.getpid(), priority, label))
        finally:
            queue.close()
        announced = None
        try:
            while True:
                queue = self._queue_lock()
                try:
                    slot = self._try(ticket, priority, label)
                    if slot:
                        return slot
                    position = [t for t in self._live(ticket)].index(ticket)
                finally:
                    queue.close()
                if announce and announced != position:
                    announce('waiting for a gate slot (%d of %d slots, %s, %d ahead)%s' % (
                        self.slots, self.slots, priority, position, self._holders()))
                    announced = position
                time.sleep(self.poll)
        finally:
            ticket.unlink(missing_ok=True)
            held.close()

    def _holders(self):
        names = []
        for index in range(self.slots):
            handle = _open(self.slot_path(index))
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                names.append('slot %d: %s' % (index, _held_by(handle)))
            handle.close()
        return '; ' + '; '.join(names) if names else ''

    def _try(self, ticket, priority, label):
        free = {}
        for index in range(self.slots):
            handle = _open(self.slot_path(index))
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                free[index] = handle
            except BlockingIOError:
                handle.close()
        taken, mine = set(), None
        for other in self._live(ticket):
            kind = self._priority_of(other)
            choice = next((i for i in self.order(kind) if i in free and i not in taken), None)
            if choice is None:
                continue
            taken.add(choice)
            if other == ticket:
                mine = choice
                break
        for index, handle in list(free.items()):
            if index != mine:
                handle.close()
        if mine is None:
            return None
        _describe(free[mine], 'pid %d since %s %s %s\n' % (os.getpid(), time.strftime('%Y-%m-%dT%H:%M:%S'), priority, label))
        return Slot(mine, free[mine])


@contextlib.contextmanager
def timing_lock(cache, label='', announce=print):
    """Hold the exclusive timing lock; blocks while another gate runs a wall-time step."""
    handle = _open(Path(cache) / 'gate-timing.lock')
    try:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            if announce:
                announce('waiting for the timing lock, held by %s' % _held_by(handle))
            fcntl.flock(handle, fcntl.LOCK_EX)
        _describe(handle, 'pid %d since %s %s\n' % (os.getpid(), time.strftime('%Y-%m-%dT%H:%M:%S'), label))
        yield
    finally:
        handle.close()


# A board run is host-timed while the runner does its FEL work (rails, reset, DRAM, the launch upload, the restore): a gate that starts
# a build or a QEMU run at that moment slows the run, and a slow run can miss its deadlines (docs/validation.md, "Gates and board
# runs"). The runner holds a shared flock on CACHE/board-fel.lock for that work and releases it while the board runs the application;
# a gate asks for the exclusive lock without waiting before it starts each step and waits while a runner holds it. The kernel frees
# the lock when the runner dies, so a crashed runner stalls no gate, and a hung runner stalls a gate for at most the deadline that
# the runner wrote into the lock file (the run's own lifetime bound).
BOARD_FEL_LOCK = 'board-fel.lock'


class BoardFel:
    """The runner's side: hold the shared lock for the FEL work of one run."""
    def __init__(self, cache, deadline_seconds):
        self.path = Path(cache) / BOARD_FEL_LOCK
        self.deadline_seconds = deadline_seconds
        self.handle = None

    def acquire(self):
        if self.handle is not None:
            return
        try:
            handle = _open(self.path)
            fcntl.flock(handle, fcntl.LOCK_SH)
            _describe(handle, json.dumps({'pid': os.getpid(), 'deadline': time.time() + self.deadline_seconds}) + '\n')
            self.handle = handle
        except OSError:
            self.handle = None             # a runner never fails for want of this lock

    def release(self):
        if self.handle is not None:
            self.handle.close()
            self.handle = None

    def __enter__(self):
        self.acquire()
        return self

    def __exit__(self, *_):
        self.release()


def wait_for_board(cache, announce=print, poll=0.05, now=time.time):
    """The gate's side: return once no runner is in its FEL work, or its deadline has passed. Returns the seconds waited."""
    path = Path(cache) / BOARD_FEL_LOCK
    started, announced = now(), False
    while True:
        try:
            handle = _open(path)
        except OSError:
            return 0.0
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return now() - started if announced else 0.0
        except BlockingIOError:
            handle.seek(0)
            try:
                deadline = float(json.loads(handle.read() or '{}').get('deadline', 0))
            except ValueError:
                deadline = 0.0
            if deadline and now() >= deadline:
                return now() - started                # the runner outlived its own deadline: do not stall behind it
            if announce and not announced:
                announce('holding the next step: a board run is in its FEL work')
                announced = True
        finally:
            handle.close()
        time.sleep(poll)


def reserve_cpus(config):
    """Keep this process, and every process it starts, off the CPUs that config reserves for the xhci interrupt of the board's USB port."""
    reserved = set(config.get('reserved_cpus', ()))
    keep = sorted(set(os.sched_getaffinity(0)) - reserved)
    if reserved and keep:
        os.sched_setaffinity(0, keep)
    return keep
