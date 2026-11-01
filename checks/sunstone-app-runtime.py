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
p.add_argument('--service-session',type=Path,required=True,help='normalized session bytes from the nominal OS service producer')
p.add_argument('--cc', default='/usr/bin/clang-21')
a = p.parse_args()
a.build.mkdir(parents=True, exist_ok=False)
fixture=a.service_session.read_bytes()
(a.build/'session-fixture.h').write_text('static const _Alignas(8) unsigned char service_fixture[]={'+','.join(str(b) for b in fixture)+'};\n')
flags = a.native_flags.read_text()
def value(name):
    return shlex.split(next(line.split('=', 1)[1] for line in flags.splitlines() if line.startswith(name + ' =')))
args = value('C_DEFINES') + value('C_INCLUDES') + value('C_FLAGS')
old_root = next(arg[2:-len('/apps/os')] for arg in args if arg.startswith('-I') and arg.endswith('/apps/os'))
args = ['-I'+str(a.build)]+[arg.replace(old_root, str(ROOT)) for arg in args if not arg.startswith(('-march=', '-mstrict-align', '-mgeneral-regs-only'))]
obj, executable = a.build / 'application.o', a.build / 'runtime'
session_obj=a.build/'session.o'
digest_obj=a.build/'digest.o'
admission_obj=a.build/'admission.o'
commands = [[a.cc, *args, '-c', str(ROOT / 'apps/os/application.c'), '-o', str(obj)],
            [a.cc,*args,'-c',str(ROOT/'apps/os/session.c'),'-o',str(session_obj)],
            [a.cc,*args,'-c',str(ROOT/'drivers/storage/storage_digest.c'),'-o',str(digest_obj)],
            [a.cc,*args,'-c',str(ROOT/'apps/os/admission.c'),'-o',str(admission_obj)],
            [a.cc, *args, str(ROOT / 'checks/sunstone-app-runtime.c'), str(obj),str(digest_obj), '-Wl,--gc-sections', '-o', str(executable)],
            [str(executable)]]
for noapp in (False,True):
    target=a.build/('session-core' if noapp else 'session-application')
    options=['-DNO_APPLICATION','-no-pie','-Wl,--defsym=__start_sunstone_manifest=0','-Wl,--defsym=__stop_sunstone_manifest=0'] if noapp else []
    commands += [[a.cc,*args,*options,str(ROOT/'checks/sunstone-session-runtime.c'),str(session_obj),str(digest_obj),str(admission_obj),'-Wl,--gc-sections','-o',str(target)],[str(target)]]
for name,definitions in [('run-budget',['-DTEST_RUN_US=60000000']),('reserve-budget',['-DTEST_RESERVE_US=60000000'])]:
    target=a.build/name
    commands += [[a.cc,*args,*definitions,'-DEXPECT_BUDGET_REFUSAL',str(ROOT/'checks/sunstone-session-runtime.c'),
                  str(session_obj),str(digest_obj),str(admission_obj),'-Wl,--gc-sections','-o',str(target)],[str(target)]]
with (a.build / 'result.log').open('w') as log:
    for command in commands:
        result = subprocess.run(command, capture_output=True, text=True)
        log.write(shlex.join(command) + '\n' + result.stdout + result.stderr)
        if result.returncode:
            raise SystemExit(result.stdout + result.stderr)
print('Application mailbox boundaries and native typed sessions with and without an application passed.')
