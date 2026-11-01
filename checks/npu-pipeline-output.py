#!/usr/bin/env python3
"""Check that NPU compiler pipelines use the generator's selected directory.

The generator and first compiler command are modeled at their process boundaries.
The real shell callers execute; no ONNX compiler, SDK or board is opened.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

import harness

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from host_evidence import unique_directory

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
out = unique_directory(parser.parse_args().output)
bin_dir = out / 'bin'
bin_dir.mkdir()
(bin_dir / 'python3').write_text(f'#!{sys.executable}\n' + '''import json, os, sys
from pathlib import Path
assert Path(sys.argv[1]).name == os.environ['SUNSTONE_TEST_GENERATOR']
root = Path(sys.argv[2])
generated = root / 'generated fixture'
generated.mkdir()
(generated / 'generation.json').write_text(json.dumps(sys.argv[1:]))
print('Generation completed.')
print('Output: ' + str(generated))
if os.environ.get('SUNSTONE_TEST_MULTIPLE_OUTPUTS'):
    print('Output: ' + str(root))
''')
(bin_dir / 'timeout').write_text(f'#!{sys.executable}\n' + '''import json, os, sys
from pathlib import Path
Path(os.environ['SUNSTONE_TEST_COMPILER_BOUNDARY']).write_text(json.dumps(
    dict(directory=str(Path.cwd()), command=sys.argv[1:])))
raise SystemExit(73)
''')
for path in bin_dir.iterdir():
    path.chmod(0o755)
cases = (
    ('conv-default', 'npu-conv-pipeline.sh', [], 'npu-conv-fixture.py', []),
    ('conv-shape', 'npu-conv-pipeline.sh', ['12', '4'], 'npu-conv-fixture.py', ['--width', '12', '--channels', '4']),
    ('two-stage', 'npu-conv-pipeline.sh', ['--two-stage'], 'npu-two-stage-fixture.py', []),
    ('dual-default', 'npu-dual-pipeline.sh', [], 'npu-dual-fixture.py', []),
    ('dual-int8', 'npu-dual-pipeline.sh', ['--perchannel-int8'], 'npu-dual-fixture.py', []),
)
for name, pipeline, arguments, generator, generated_arguments in cases:
    requested = out / (name + ' existing output')
    requested.mkdir()
    sentinel = requested / 'earlier-model.onnx'
    sentinel.write_bytes(b'Earlier fixture and compiler inputs')
    boundary = out / (name + '-compiler.json')
    env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ['PATH'],
               SUNSTONE_TEST_GENERATOR=generator, SUNSTONE_TEST_COMPILER_BOUNDARY=str(boundary))
    command = ['bash', str(ROOT / 'tools' / pipeline), str(requested), *arguments]
    result = subprocess.run(command, capture_output=True, text=True, env=env)
    (out / (name + '.log')).write_text(result.stdout + result.stderr)
    assert result.returncode == 73, result.stdout + result.stderr
    observed = json.loads(boundary.read_text())
    generated = requested / 'generated fixture'
    assert observed['directory'] == str(generated)
    invocation = json.loads((generated / 'generation.json').read_text())
    assert invocation[1:] == [str(requested), *generated_arguments]
    assert observed['command'][2:4] == ['/root/acuity-toolkit-whl-6.30.22/bin/pegasus.py', 'import']
    assert sentinel.read_bytes() == b'Earlier fixture and compiler inputs'
    assert not (requested / 'import.log').exists()
    # Ambiguous output must stop before a compiler can use the stale parent.
    multiple = out / (name + ' ambiguous output')
    multiple.mkdir()
    refused_boundary = out / (name + '-must-not-compile.json')
    env.update(SUNSTONE_TEST_MULTIPLE_OUTPUTS='1', SUNSTONE_TEST_COMPILER_BOUNDARY=str(refused_boundary))
    result = subprocess.run(['bash', str(ROOT / 'tools' / pipeline), str(multiple), *arguments],
                            capture_output=True, text=True, env=env)
    assert result.returncode == 1 and 'did not report one output directory' in result.stderr
    assert not refused_boundary.exists()
(out / 'result.json').write_text(json.dumps(dict(passed=True, cases=[c[0] for c in cases],
    compiler_execution='modeled boundary; not executed'), indent=2) + '\n')
print('PASS: five pipeline selections compile from the generated child and refuse ambiguous output.')
