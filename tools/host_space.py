"""Keep the gate, the landing and the board runner off /tmp, and refuse to start where the space is short.

/tmp is a 62 GB tmpfs with a per-user quota, and it has stopped work twice: it ran out of inodes on 2026-11-01, and on 2026-11-02 agents'
scratch filled 46 GB of it, so the landing gate of lost-wake-audit-1102 failed at central-build with "Disk quota exceeded".
  use_disk_tmp(base)  points TMPDIR (and TMP, TEMP) at base/tmp on the main disk for this process and for everything it starts: the
                      compilers, CMake, the checks and the board runner's helpers. A base under /tmp or /dev/shm is refused.
  preflight(...)      refuses to start when a volume lacks free space or free inodes, and names which one.
Called at the start of main() in tools/integration-gate.py, tools/land.py and tools/board.py.
"""
import os
from pathlib import Path
import tempfile

GIB = 1 << 30
# What a gate needs: builds and sweeps on the main disk, and enough of /tmp left for the tools that still write there.
BUILD_ROOT_MINIMUM = (30 * GIB, 500_000)
TMP_MINIMUM = (2 * GIB, 20_000)
VOLATILE = (Path('/tmp'), Path('/dev/shm'), Path('/var/tmp'))
# Checks that run the gate or the landing against a fake cache in a scratch directory set SUNSTONE_HOST_SPACE=off; nothing else does.
OFF = 'SUNSTONE_HOST_SPACE'


def disabled():
    return os.environ.get(OFF) == 'off'


class SpaceShortage(RuntimeError):
    """A volume the gate needs is short of space or inodes; the message names it."""


def under(path, roots=VOLATILE):
    resolved = Path(os.path.realpath(path))
    return any(resolved == root or root in resolved.parents for root in roots)


def use_disk_tmp(base):
    """TMPDIR for this process and its children: base/tmp, which must not be on /tmp."""
    directory = Path(base) / 'tmp'
    if disabled():
        return Path(tempfile.gettempdir())
    if under(directory):
        raise SpaceShortage('%s is on a memory-backed volume; the temporary directory must be on the main disk' % directory)
    directory.mkdir(parents=True, exist_ok=True)
    for key in ('TMPDIR', 'TMP', 'TEMP'):
        os.environ[key] = str(directory)
    tempfile.tempdir = None            # tempfile caches the directory it first found
    return directory


def volume(path):
    """(free bytes, free inodes) for an unprivileged writer on the volume holding path."""
    stat = os.statvfs(path)
    return stat.f_bavail * stat.f_frsize, stat.f_favail


def preflight(build_root, tmp='/tmp', build_minimum=BUILD_ROOT_MINIMUM, tmp_minimum=TMP_MINIMUM):
    """Raise SpaceShortage naming the first volume that is short of space or inodes."""
    if disabled():
        return
    probe = Path(build_root)
    while not probe.exists() and probe != probe.parent:
        probe = probe.parent
    for label, path, (need_bytes, need_inodes) in (('the build root %s' % build_root, probe, build_minimum),
                                                    ('/tmp (%s)' % tmp, Path(tmp), tmp_minimum)):
        if not path.exists():
            continue
        free_bytes, free_inodes = volume(path)
        if free_bytes < need_bytes:
            raise SpaceShortage('%s has %.1f GiB free; %.1f GiB are needed' % (label, free_bytes / GIB, need_bytes / GIB))
        if free_inodes < need_inodes:
            raise SpaceShortage('%s has %d free inodes; %d are needed' % (label, free_inodes, need_inodes))
