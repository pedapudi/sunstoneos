#!/usr/bin/env python3
"""Run the OS application mailbox with the selected native image's headers.

The test compiles apps/os/application.c itself for the host, keeps the image's
FreeRTOS configuration and header dependencies, and substitutes only the task
notification boundary and board liveness. It does not model hardware.
"""
import argparse
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--native-flags', type=Path, required=True, help='CMake image flags.make')
p.add_argument('--cc', default='/usr/bin/clang-21')
a = p.parse_args()
a.build.mkdir(parents=True, exist_ok=False)
flags = a.native_flags.read_text()
def value(name):
    return shlex.split(next(line.split('=', 1)[1] for line in flags.splitlines() if line.startswith(name + ' =')))
args = value('C_DEFINES') + value('C_INCLUDES') + value('C_FLAGS')
old_root = next(arg[2:-len('/apps/os')] for arg in args if arg.startswith('-I') and arg.endswith('/apps/os'))
args = [arg.replace(old_root, str(ROOT)) for arg in args if not arg.startswith(('-march=', '-mstrict-align', '-mgeneral-regs-only'))]
obj, executable = a.build / 'application.o', a.build / 'runtime'
commands = [[a.cc, *args, '-c', str(ROOT / 'apps/os/application.c'), '-o', str(obj)],
            [a.cc, *args, str(ROOT / 'checks/sunstone-app-runtime.c'), str(obj), '-Wl,--gc-sections', '-o', str(executable)],
            [str(executable)]]
with (a.build / 'result.log').open('w') as log:
    for command in commands:
        result = subprocess.run(command, capture_output=True, text=True)
        log.write(shlex.join(command) + '\n' + result.stdout + result.stderr)
        if result.returncode:
            raise SystemExit(result.stdout + result.stderr)
print('Application mailbox serialization, notification boundaries and refusals passed.')
