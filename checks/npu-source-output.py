#!/usr/bin/env python3
"""Exercise NPU generator outputs and saved SDK checks without device access.

When ONNX is unavailable, model its graph writer only. NumPy calculations and
production directory/report handling still execute; real ONNX validation remains
unrun in that mode. SDK execution is modeled by producing one bounded dump.
"""
import argparse
from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import runpy
import sys
import types
from unittest.mock import patch

from harness import artifact_refused, refused
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from host_evidence import unique_directory

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
out = unique_directory(parser.parse_args().output)
try:
    import onnx
    real_onnx = True
except ImportError:
    real_onnx = False
    onnx = types.ModuleType('onnx')
    def construct(*args, **kwargs):
        return types.SimpleNamespace(args=args, kwargs=kwargs)
    onnx.helper = types.SimpleNamespace(**{name: construct for name in
        ('make_node', 'make_graph', 'make_tensor_value_info', 'make_opsetid', 'make_model')})
    onnx.numpy_helper = types.SimpleNamespace(from_array=construct)
    onnx.TensorProto = types.SimpleNamespace(FLOAT=1)
    onnx.checker = types.SimpleNamespace(check_model=lambda model: None)
    onnx.save = lambda model, path: Path(path).write_bytes(b'Modeled ONNX serialization\n')
    onnx.__version__ = 'modeled writer; actual ONNX validation unrun'

# Execute each generator at a fresh path and require an existing path to fail.
generators = ('npu-conv-fixture.py', 'npu-dual-fixture.py',
              'npu-relative-shift-fixture.py', 'npu-two-stage-fixture.py')
for name in generators:
    directory = out / name.removesuffix('.py')
    def generate():
        capture = io.StringIO()
        with patch.dict(sys.modules, {'onnx': onnx}), patch.object(sys, 'argv', [name, str(directory)]), redirect_stdout(capture):
            runpy.run_path(str(ROOT / 'tools' / name), run_name='__main__')
        return capture.getvalue()
    text = generate()
    (out / (name + '.log')).write_text(text)
    assert f'Output: {directory}' in text
    assert list(directory.glob('*.onnx')) and list(directory.glob('*.npy'))
    for path in directory.glob('*.npy'):
        assert np.isfinite(np.load(path, allow_pickle=False)).all()
    artifact_refused(generate, directory)

spec = importlib.util.spec_from_file_location('npu_conv_input', ROOT / 'tools/npu-conv-input.py')
sdk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sdk)
fixture = out / 'sdk-inputs'
fixture.mkdir()
sdk.__file__ = str(fixture / 'npu-conv-input.py')
model, executable, data = (fixture / n for n in ('model.nb', 'simulator', 'input.bin'))
model.write_bytes(b'Modeled fixed-size NBG')
executable.write_bytes(b'Modeled SDK executable; never executed')
data.write_bytes(bytes(range(12)))
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
(fixture / 'npu-models.json').write_text(json.dumps(dict(models=[dict(name='fixture',
    model=dict(bytes=model.stat().st_size, sha256=sha(model)), input=dict(bytes=12), reference=dict(bytes=4))])))
requested = fixture / 'runs'
requested.mkdir()
(requested / 'earlier.txt').write_text('Preserve SDK evidence.\n')
calls = []
def simulate(command, *, cwd, **kwargs):
    calls.append(command)
    assert command[0] == str(executable.resolve()) and command[1] == str(model.resolve())
    assert Path(command[2]).name == 'input.qnt.tensor'
    consumed = bytes(int(v) for v in Path(command[2]).read_text().split())
    (cwd / 'input_0.dat').write_bytes(consumed)
    (cwd / 'output0_2_2_1_1.dat').write_bytes(bytes((3, 4, 5, 6)))
    return types.SimpleNamespace(stdout=b'Modeled SDK completion\n', check_returncode=lambda: None)
base_args = ['npu-conv-input', '--executable', str(executable), '--model', str(model), '--input', str(data)]
def invoke(directory, saved=False):
    capture = io.StringIO()
    argv = base_args + ['--output', str(directory)] + (['--check-saved'] if saved else [])
    with patch.object(sys, 'argv', argv), patch.object(sdk.subprocess, 'run', simulate), redirect_stdout(capture):
        sdk.main()
    return capture.getvalue()
refused(lambda: invoke(requested), str(requested), FileExistsError)
assert list(requested.iterdir()) == [requested / 'earlier.txt'] and not calls
run = fixture / 'fresh-run'
invoke(run)
assert (run / 'input-check.json').is_file() and len(calls) == 1
artifact_refused(lambda: invoke(run), run)
baseline = {p.relative_to(run): p.read_bytes() for p in run.iterdir() if p.is_file()}
for attempt in range(2):
    text = invoke(run, saved=True)
    assert 'Report:' in text
assert len(calls) == 1
reports = list((run / 'input-checks').rglob('input-check.json'))
assert len(reports) == 2 and all(json.loads(p.read_text())['reexecuted'] is False for p in reports)
assert {p: (run / p).read_bytes() for p in baseline} == baseline
assert (requested / 'earlier.txt').read_text() == 'Preserve SDK evidence.\n'
(run / 'input_0.dat').write_bytes(bytes(12))
refused(lambda: invoke(run, saved=True), 'SDK consumed input differs')
(run / 'input_0.dat').write_bytes(data.read_bytes())
(run / 'output0_2_2_1_1.dat').write_bytes(bytes(3))
refused(lambda: invoke(run, saved=True), 'SDK output binding or extent differs')
(run / 'output0_2_2_1_1.dat').write_bytes(bytes(4))
(run / 'input.qnt.tensor').write_text('0\n')
refused(lambda: invoke(run, saved=True), 'Saved quantized input differs')
assert len(calls) == 1
(out / 'result.json').write_text(json.dumps(dict(passed=True, real_onnx=real_onnx,
    generators=list(generators), sdk_execution='modeled', saved_checks_reexecute=False,
    original_evidence_preserved=True), indent=2) + '\n')
print('PASS: four generator paths refuse existing output and SDK saved checks preserve reports and reject changed data.')
print('ONNX validation: ' + ('executed' if real_onnx else 'unrun; graph writer modeled'))
