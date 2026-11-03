#!/usr/bin/env python3
"""Check that the gate, the landing and the board runner keep their temporary files off /tmp and refuse to start where space is short.

/tmp is a tmpfs with a per-user quota. It ran out of inodes on 2026-11-01, and on 2026-11-02 agents' scratch filled 46 GB of it, so
a landing gate failed at central-build with "Disk quota exceeded". tools/host_space.py moves TMPDIR to the main disk and checks the
volumes first. This check requires:
  environment  integration-gate.gate_environment (the environment of every build, configure step and check) sets TMPDIR, TMP and TEMP to
               a directory on the main disk, not under /tmp, /var/tmp or /dev/shm, and a Python child started with that environment
               reports the same directory as its temporary directory;
  refusal      a build root under /tmp is refused, so the setup cannot put the gate's temporary files there;
  preflight    a volume short of space or of inodes is refused with the volume named: the build root, and /tmp;
  wiring       tools/land.py and tools/board.py call use_disk_tmp, and tools/integration-gate.py and tools/land.py call preflight, in main();
               no gate command line in them names /tmp.
Usage: host-space.py --output DIR
"""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
CHECK_INPUTS = ['tools/host_space.py', 'tools/integration-gate.py', 'tools/land.py', 'tools/board.py']
DISK_BUILD_ROOT = Path.home() / '.cache' / 'sunstoneos' / 'host-space-check'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=True)
    os.environ.pop('SUNSTONE_HOST_SPACE', None)             # the fixtures of other checks switch the guard off; this check needs it on
    gate = load('host_space_check_gate', ROOT / 'tools/integration-gate.py')
    space = gate.host_space            # the gate's own copy, so that the exception classes are the same
    assert not space.under(DISK_BUILD_ROOT), 'the home directory is expected on the main disk'
    env = gate.gate_environment(ROOT, Path.home() / '.cache' / 'sunstoneos', DISK_BUILD_ROOT)
    for key in ('TMPDIR', 'TMP', 'TEMP'):
        assert env[key] == str(DISK_BUILD_ROOT / 'tmp') and not space.under(env[key]), (key, env.get(key))
    reported = subprocess.run([sys.executable, '-c', 'import tempfile; print(tempfile.gettempdir())'], env=env, capture_output=True,
                              text=True, check=True).stdout.strip()
    assert reported == env['TMPDIR'], reported
    try:
        gate.gate_environment(ROOT, Path.home() / '.cache' / 'sunstoneos', out / 'under-tmp-or-not')
    except space.SpaceShortage:
        assert space.under(out), 'a build root on the main disk was refused'
    else:
        assert not space.under(out), 'a build root under /tmp was accepted'
    refused = harness.refused(lambda: space.use_disk_tmp('/tmp/host-space-check'), 'memory-backed', space.SpaceShortage)
    space.preflight(DISK_BUILD_ROOT)                        # this host has the space the gate needs
    huge = (1 << 60, 1)
    message = harness.refused(lambda: space.preflight(DISK_BUILD_ROOT, build_minimum=huge), 'the build root', space.SpaceShortage)
    assert 'GiB free' in message, message
    message = harness.refused(lambda: space.preflight(DISK_BUILD_ROOT, build_minimum=(0, 1 << 60)), 'free inodes', space.SpaceShortage)
    assert 'the build root' in message, message
    message = harness.refused(lambda: space.preflight(DISK_BUILD_ROOT, tmp_minimum=huge), '/tmp', space.SpaceShortage)
    message = harness.refused(lambda: space.preflight(DISK_BUILD_ROOT, tmp_minimum=(0, 1 << 60)), '/tmp', space.SpaceShortage)
    assert 'free inodes' in message, message
    for name, needs in (('tools/land.py', ('use_disk_tmp', 'preflight')), ('tools/board.py', ('use_disk_tmp',)),
                        ('tools/integration-gate.py', ('use_disk_tmp', 'preflight'))):
        text = (ROOT / name).read_text()
        main_text = text[text.index('def main(') if name != 'tools/integration-gate.py' else 0:]
        for call in needs:
            assert 'host_space.' + call in main_text or call + '(' in main_text, (name, call)
        if name != 'tools/board.py':          # the runner's per-port lock files are its one use of /tmp
            assert "'/tmp" not in text and '"/tmp' not in text, (name, 'names a path under /tmp')
    print('PASS host space: TMPDIR is %s for the gate\'s environment and its children; a build root under /tmp is refused; a volume short of '
          'space or inodes is refused with the build root or /tmp named; the gate, the landing and the runner call it' % env['TMPDIR'])


if __name__ == '__main__':
    main()
