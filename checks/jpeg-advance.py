#!/usr/bin/env python3
"""Check bounded JPEG programming batches with delayed and stuck device completion."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import resource
import shutil
import signal
import subprocess

from harness import CLANG, SANITIZE, environment

CHECK_INPUTS = ['drivers/video/a733_video_jpeg_run.c', 'include/a733_video_jpeg_run.h',
                'drivers/video/a733_video_jpeg.c', 'checks/a733-video-jpeg-run.c',
                'checks/register_latency.h', 'apps/os/usb-jpeg-native.c',
                'apps/jpeg/native.c', 'apps/jpeg/fixture.py', 'apps/jpeg/fixtures.json']

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
commands = []


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run(label, command, expected=0):
    command = list(map(str, command))
    result = subprocess.run(command, cwd=out, env=environment(), capture_output=True,
                            text=True, preexec_fn=no_core)
    (out/(label+'.log')).write_text(result.stdout+result.stderr)
    commands.append(dict(label=label, command=command, returncode=result.returncode))
    (out/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
    assert result.returncode == expected, (label, result.returncode, result.stdout, result.stderr)
    return result


source = root/'drivers/video/a733_video_jpeg_run.c'
caller = (root/'apps/os/usb-jpeg-native.c').read_text().split('int ss_usb_jpeg_native_poll(', 1)[1]
assert 'a733_video_jpeg_advance(&jpeg.run,1024)' in caller
hot = source.read_text().split('int a733_video_jpeg_advance(', 1)[1]+caller
assert not re.search(r'\b(vTaskDelay|pdMS_TO_TICKS|ulTaskNotifyTake|xQueueReceive)\s*\(', hot), 'tick wait in ready JPEG work'
spec = importlib.util.spec_from_file_location('jpeg_fixture', root/'apps/jpeg/fixture.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
(out/'input.jpg').write_bytes(fixture.nonconstant())

flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', *SANITIZE,
         '-I'+str(root/'include'), '-I'+str(root/'checks')]
compiled = ['checks/a733-video-jpeg-run.c', 'drivers/video/a733_video_jpeg.c',
            'drivers/video/a733_video_jpeg_run.c']
objects = []
inputs = set(CHECK_INPUTS+['checks/jpeg-advance.py', 'checks/harness.py'])
for name in compiled:
    obj = out/(Path(name).stem+'.o')
    deps = obj.with_suffix('.d')
    run(obj.stem+'-compile', [CLANG, *flags, '-MMD', '-MF', deps, '-c', root/name, '-o', obj])
    objects.append(obj)
    words = deps.read_text().replace('\\\n', '').split(':', 1)[1].split()
    inputs.update(str(Path(p).resolve().relative_to(root)) for p in words)
binary = out/'jpeg-advance'
run('link', [CLANG, *SANITIZE, *objects, '-o', binary])
result = run('delayed-completion', [binary, out/'input.jpg', out/'trace.bin'])

# A ready register per owner wake recreates the old tick-pacing defect. The
# same native fixture must reject that executor at its programming assertion.
mutation = out/'single-step.c'
original = source.read_text()
assert original.count('n<budget') == 1
mutation.write_text(original.replace('n<budget', 'n<1'))
mutant = out/'single-step'
run('mutation-compile', [CLANG, *flags, root/compiled[0], root/compiled[1], mutation, '-o', mutant])
failure = run('mutation', [mutant, out/'input.jpg', out/'mutation-trace.bin'], -signal.SIGABRT)
assert 'delayed.writes==659' in failure.stderr, failure.stderr

hashes = {}
for name in sorted(inputs):
    path = root/name
    target = out/'inputs'/name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(path, target)
    hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
record = dict(inputs=hashes, cases='1/17/2000 us completion, deferred copy, stuck completion, poll budget, single-step mutation',
              limits='Host register model; no decoded physical pixels or board timing',
              artifacts={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in out.iterdir() if p.is_file()},
              compiler=dict(path=str(CLANG), sha256=hashlib.sha256(CLANG.read_bytes()).hexdigest()))
(out/'results.json').write_text(json.dumps(record, indent=2)+'\n')
print(result.stdout, end='')
print('PASS: ready programming completes in one batch; delayed/stuck completion remains bounded; single-step mutation refused')
