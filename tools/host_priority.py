"""Scheduling and I/O priority of the processes that share the host with the board runner.

Board runs time their host stages (DRAM bring-up, preparation) against the disk and the CPUs that the integration gate and the image
builds also use. With the gate's sweeps writing 170 to 290 MB/s, the stage times of runs on both ports went from 2.3 s of DRAM and 0.6 s
of preparation to 29 to 74 s and 11 to 18 s. Two settings keep the runs' timing independent of the gate:

  lower()  the gate and its children: nice 10 and the idle I/O class (ionice -c3). Called at the start of tools/integration-gate.py
           and tools/land.py, so every build, check and sweep they start inherits it.
  raise_runner()  the board runner, its run processes and the queue daemon: the best-effort I/O class at its highest level (0),
           above the default level 4. It lowers no CPU priority. Called at the start of tools/board.py, so a restart of the daemon
           keeps it; the processes the daemon starts inherit it.

Both use the ioprio_set system call. No privilege is needed to lower a priority or to set best-effort level 0.
"""
import ctypes
import os

IOPRIO_WHO_PROCESS = 1
IOPRIO_CLASS_BE, IOPRIO_CLASS_IDLE = 2, 3
SYS_IOPRIO_SET = {'x86_64': 251, 'aarch64': 30}


def set_io_priority(klass, level=0):
    """ioprio_set for this process; False where the system call is unavailable (an unsupported architecture)."""
    number = SYS_IOPRIO_SET.get(os.uname().machine)
    if number is None:
        return False
    libc = ctypes.CDLL(None, use_errno=True)
    return libc.syscall(number, IOPRIO_WHO_PROCESS, 0, (klass << 13) | level) == 0


def lower(niceness=10):
    """The gate's priority: nice 10 (added to the current value) and the idle I/O class."""
    os.nice(niceness)
    return set_io_priority(IOPRIO_CLASS_IDLE)


def raise_runner():
    """The runner's priority: best-effort I/O at level 0. CPU niceness is left as it is."""
    return set_io_priority(IOPRIO_CLASS_BE, 0)


def io_priority():
    """(class, level) of this process, from the ioprio_get system call: class 0 none, 1 real time, 2 best effort, 3 idle."""
    number = {'x86_64': 252, 'aarch64': 31}.get(os.uname().machine)
    value = ctypes.CDLL(None, use_errno=True).syscall(number, IOPRIO_WHO_PROCESS, 0)
    return value >> 13, value & 0x1fff


PRESSURE_FILES = ('io', 'cpu', 'memory')


def pressure():
    """The kernel's pressure-stall figures now: {'io': {'some': {'avg10': ..., 'avg60': ..., 'avg300': ..., 'total': ...}, 'full': {...}}, ...}.

    The averages are percentages of time that tasks stalled; 'total' counts microseconds, so the difference between the start and the
    end of a run gives the stall time in the run itself. Empty for a file the kernel does not provide."""
    result = {}
    for name in PRESSURE_FILES:
        try:
            lines = open('/proc/pressure/' + name).read().splitlines()
        except OSError:
            continue
        record = {}
        for line in lines:
            kind, *fields = line.split()
            record[kind] = {key: (int(value) if key == 'total' else float(value)) for key, value in (f.split('=') for f in fields)}
        result[name] = record
    return result
