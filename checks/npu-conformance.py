#!/usr/bin/env python3
"""Check the NPU conformance harness's vectors, comparisons and identity pins; host only.

Exercises tools/npu-conformance.py without the compiler image, the cache or a
device: the vector plan for the session catalogue, seeded bytes, line
coverage, the int32-bias rule, the per-model verdict, runner timing, runner
lanes and the refusal of changed pinned files.
"""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402  (assertion policy)

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('npu_conformance', ROOT / 'tools/npu-conformance.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
CATALOGUE = {m['name']: m for m in tool.load(tool.CATALOGUE)['models']}
REFERENCES = tool.load(tool.REFERENCES)['models']


def reference_input(model):
    """A stand-in for the catalogued input: seeded bytes that equal no fill vector."""
    return [tool.seeded('fixture ' + model['name'], i, t['logical_bytes']) for i, t in enumerate(model['inputs'])]


def plan(name):
    model = CATALOGUE[name]
    entry = REFERENCES[name]
    return model, tool.plan_jobs(model, reference_input(model), entry['reference'] is not None,
                                 entry.get('repeat', 'seeded'))


# Vector plan for the catalogue: the design's job budget and line counts.
assert set(CATALOGUE) == set(REFERENCES)
jobs = {name: plan(name)[1] for name in CATALOGUE}
kinds = [j['kind'] for js in jobs.values() for j in js]
assert kinds.count('base') == 24 and kinds.count('line') == 55 and kinds.count('repeat') == 5, kinds
assert {n: sum(j['kind'] == 'line' for j in js) for n, js in jobs.items()} == {
    'conv8': 3, 'dual-u8': 4, 'dual-i8': 4, 'mimus-linear-q': 44, 'shufflenet-v2-published': 0}
# ShuffleNetV2's input zero point equals its minimum code, so that vector runs
# once; it repeats the maximum vector, whose output is unsaturated.
assert [j['label'] for j in jobs['shufflenet-v2-published']] == [
    'reference', 'zero point', 'maximum', 'seeded', 'maximum again']
assert jobs['shufflenet-v2-published'][-1]['vector'] == jobs['shufflenet-v2-published'][2]['vector']
harness.refused(lambda: tool.plan_jobs(CATALOGUE['shufflenet-v2-published'],
                                       reference_input(CATALOGUE['shufflenet-v2-published']), False, 'minimum'),
                'no minimum base vector to repeat')
assert [j['label'] for j in jobs['conv8'][:5]] == ['reference', 'zero point', 'minimum', 'maximum', 'seeded']
mimus = CATALOGUE['mimus-linear-q']
assert jobs['mimus-linear-q'][2]['vector'] == (b'\x00\x80' * 1408,)    # INT16 minimum, little endian
assert jobs['dual-i8'][1]['vector'] == (b'\xff' * 192, b'\xff' * 36)  # INT8 zero point -1

# Each input-line vector equals the previous job's vector except in its own line.
for name, js in jobs.items():
    model = CATALOGUE[name]
    previous = [j for j in js if j['kind'] == 'base'][-1]['vector']
    for job in (j for j in js if j['kind'] == 'line'):
        changed = [(i, n) for i, (a, b) in enumerate(zip(previous, job['vector']))
                   for n in tool.changed_lines(a, b)]
        assert changed == [(job['input'], job['line'])], (name, job['label'], changed)
        previous = job['vector']
    assert js[-1]['vector'] == [j for j in js if j['kind'] == 'base'][js[-1]['of']]['vector']

# Seeded bytes: SHA-256 counter blocks over name, vector and optional line.
block = hashlib.sha256(b'conv8\x004\x00' + (0).to_bytes(8, 'little')).digest()
assert tool.seeded('conv8', 4, 40)[:32] == block and len(tool.seeded('conv8', 4, 40)) == 40
assert tool.seeded('conv8', 5, 64, 0) == tool.seeded('conv8', 5, 64, 0) != tool.seeded('conv8', 5, 64, 1)
assert tool.seeded('conv8', 4, 64) != tool.seeded('dual-u8', 4, 64)

# Tensor decoding, differences and saturation.
i8 = dict(CATALOGUE['dual-i8']['outputs'][1], logical_bytes=4)
assert tool.codes(i8, b'\x80\x7f\xff\x00') == [-128, 127, -1, 0]
harness.refused(lambda: tool.codes(i8, b'\x00'), 'holds 1 bytes, not 4')
harness.refused(lambda: tool.tensor_format(dict(i8, format=0)), 'unsupported tensor type')
harness.refused(lambda: tool.tensor_format(dict(i8, quantization=1)), 'unsupported tensor type')
u8 = dict(CATALOGUE['conv8']['outputs'][0], logical_bytes=40)
expected = bytes(range(40))
actual = bytes(v + (3 if v % 2 else 0) for v in range(40))
d = tool.difference(u8, expected, actual)
assert d == dict(differing=20, positions=list(range(1, 33, 2)), largest=3), d
assert tool.difference(u8, expected, expected) == dict(differing=0, positions=[], largest=0)
shuffle = CATALOGUE['shufflenet-v2-published']
assert tool.saturated(shuffle, (b'\x00\xff',)) and tool.saturated(shuffle, (b'\xff\xff',))
assert not tool.saturated(shuffle, (b'\xfb\x05',))
assert tool.saturated(mimus, (b'\xff\x7f\x00\x80' * 704,))
assert not tool.saturated(mimus, (b'\xff\x7f\x00\x80' * 703 + b'\x00\x00\x00\x80',))

# Line coverage. An input line is covered only by its own line job, when that
# job's output differs from the preceding job's; a base pair covers no input
# line. An output line is covered by any pair whose outputs differ in it.
model = dict(inputs=[dict(logical_bytes=130)], outputs=[dict(logical_bytes=70)])
base_job = dict(kind='base')
coverage = tool.line_coverage(model, [
    (base_job, (bytes(70),)), (base_job, (b'\x01' + bytes(69),)),
    (dict(kind='line', input=0, line=0), (b'\x02' + bytes(69),)),
    (dict(kind='line', input=0, line=1), (b'\x02' + bytes(69),)),
    (dict(kind='repeat'), (b'\x01' + bytes(69),))])
assert coverage == dict(input_lines=3, output_lines=2, uncovered_input_lines=[(0, 1), (0, 2)],
                        uncovered_output_lines=[(0, 1)]), coverage

# The int32-bias rule: exact sums, the bias rounded once to accumulator units
# (half to even), one rounding of the result, then the clamp.
assert tool.mimus_int32_bias([3, -2], 1, 2, [5, 7], 1, [0.5], [0.1875], 0.25, 0.25, 0, -8, 8) == [2]
assert tool.mimus_int32_bias([3, -2], 1, 2, [5, 7], 1, [0.5], [0.3125], 0.25, 0.125, 0, -8, 8) == [3]
assert tool.mimus_int32_bias([3, -2], 1, 2, [5, 7], 1, [0.5], [0.4375], 0.25, 0.125, 1, -8, 8) == [6]
assert tool.mimus_int32_bias([300, 0], 1, 2, [100, 0], 1, [1.0], [0.0], 1.0, 1.0, 0, -8, 8) == [8]
assert tool.mimus_int32_bias([1, 2, 3, 4], 2, 2, [1, 0, 0, 1, 1, 1], 3, [1.0] * 3, [0.0] * 3,
                             1.0, 1.0, 0, -99, 99) == [1, 2, 3, 3, 4, 7]

# Per-model verdict.
name = 'dual-u8'
model, js = plan(name)
outputs = [tuple(tool.seeded('out', i * 10 + o, t['logical_bytes']) for o, t in enumerate(model['outputs']))
           for i in range(len(js))]
base = [i for i, j in enumerate(js) if j['kind'] == 'base']
outputs[-1] = outputs[base[-1]]  # the repeat equals the seeded job
results = [dict(outputs=o, read_back=j['vector']) for o, j in zip(outputs, js)]
references = [outputs[i] for i in base]
catalogued = [hashlib.sha256(v).hexdigest() for v in outputs[0]]
state = tool.evaluate(model, js, results, references, catalogued)
assert state['state'] == 'pass' and state['failures'] == [], state['failures']
assert state['coverage']['uncovered_input_lines'] == [] and state['unsaturated_vectors'] == 5
assert state['board_job_order'] == [j['label'] for j in js] and state['pairs_with_equal_outputs'] == []


def verdict(change):
    r, ref, cat = copy.deepcopy(results), list(references), list(catalogued)
    change(r, ref, cat)
    return tool.evaluate(model, js, r, ref, cat)


def failed(change, reason):
    state = verdict(change)
    assert state['state'] == 'fail' and any(reason in f for f in state['failures']), state['failures']
    return state


failed(lambda r, ref, cat: r[3].update(outputs=(r[3]['outputs'][0][:-1], r[3]['outputs'][1])), 'output sizes')
failed(lambda r, ref, cat: r[7].update(read_back=(bytes(192), r[7]['read_back'][1])), 'read back after the run')
failed(lambda r, ref, cat: r[-1].update(outputs=outputs[0]), 'repeated seeded vector gave different outputs')


def saturate(r, ref, cat):
    clamped = tuple(bytes([0, 255]) * (len(v) // 2) for v in r[base[-1]]['outputs'])
    r[base[-1]].update(outputs=clamped)
    r[-1].update(outputs=clamped)
    ref[-1] = clamped


failed(saturate, 'repeated seeded vector has a saturated output')


def stale(r, ref, cat):
    """Every line vector and the repeat give the preceding job's output."""
    for index, job in enumerate(js):
        if job['kind'] != 'base':
            r[index].update(outputs=r[index - 1]['outputs'])


state = failed(stale, 'input lines [(0, 0), (0, 1), (0, 2), (1, 0)] are not covered')
assert state['coverage']['uncovered_output_lines'] == [] and len(state['pairs_with_equal_outputs']) == 5
failed(lambda r, ref, cat: cat.__setitem__(0, '0' * 64), 'differs from the catalogue')
failed(lambda r, ref, cat: ref.__setitem__(2, None), 'minimum: no reference output')
state = failed(lambda r, ref, cat: ref.__setitem__(1, (bytes([ref[1][0][0] ^ 1]) + ref[1][0][1:], ref[1][1])),
               'zero point: simulator output differs from the reference')
assert state['vectors'][1]['differences'][0]['differing'] == 1
assert tool.evaluate(model, js, None, references, catalogued)['failures'] == ['no simulator output']

# Generated runner bindings carry the catalogue's exact float32 scales.
text = tool.bindings(CATALOGUE['conv8'])
assert '#define NPU_CONFORMANCE_INPUTS 1' in text and '#define NPU_CONFORMANCE_OUTPUTS 1' in text
assert '{ 4, { 8, 8, 3, 1 }, VSI_NN_TYPE_UINT8, 0x1.0101020000000p-6f, 127, 192 },' in text, text
assert '{ 2, { 176, 8, 0, 0 }, VSI_NN_TYPE_INT16,' in tool.bindings(mimus)

# Timeouts: the one-vector limit plus four per-vector times for each further job.
assert tool.invocation_limit(dict(one_vector_limit=600, per_vector_seconds=8.5), 5) == 736
entry = dict(one_vector_limit=120, per_vector_seconds=0.5, jobs=3, reference='compiler', base_vectors=2)
assert tool.container_limit(dict(models=dict(m=entry))) == 120 + 120 + 60 + 124 + 240

# Runner timing: job 0 also prepares the graph and is reported apart.
t = tool.timing('prepare 5000000\njob 0 4000000000\njob 1 1000000000\njob 2 3000000000\n', 10.0, 99)
assert t == dict(prepare_seconds=0.005, first_job_seconds=4.0, steady_job_seconds_mean=2.0,
                 steady_job_seconds_max=3.0, outside_seconds=1.995, invocation_seconds=10.0,
                 invocation_limit_seconds=99), t

# Runner lanes: the catalogue's longest invocation runs alone.
lane_plan = dict(models={n: dict(per_vector_seconds=REFERENCES[n]['per_vector_seconds']) for n in CATALOGUE})
assert tool.lanes(lane_plan, jobs) == [['shufflenet-v2-published'],
                                       ['mimus-linear-q', 'dual-u8', 'dual-i8', 'conv8']], tool.lanes(lane_plan, jobs)
assert tool.lanes(dict(models=dict(a=dict(per_vector_seconds=1))), dict(a=[0])) == [['a']]

# Staging refuses any changed or missing pinned identity.
with tempfile.TemporaryDirectory() as temporary:
    root, cache = Path(temporary) / 'root', Path(temporary) / 'cache'
    (root / 'tools').mkdir(parents=True)
    cache.mkdir()
    files = dict(nbinfo=b'inspector', nbg=b'VPMN model', input=bytes(range(192)), json=b'{}', data=b'd',
                 quantize=b'q', meta=b'm')
    for key, value in files.items():
        (cache / key).write_bytes(value)
    digest = {k: hashlib.sha256(v).hexdigest() for k, v in files.items()}
    model = copy.deepcopy(CATALOGUE['conv8'])
    model['model']['sha256'] = digest['nbg']
    model['inputs'][0]['reference']['sha256'] = digest['input']
    (root / tool.CATALOGUE).write_text(json.dumps(dict(models=[model])))
    (root / tool.RUNNER).write_text('runner')
    table = dict(inspector=dict(path='nbinfo', sha256=digest['nbinfo'], version='v'), models=dict(conv8=dict(
        nbg='nbg', inputs=['input'], one_vector_limit=120, per_vector_seconds=0.01,
        reference=dict(kind='compiler', files={
            'model.json': dict(path='json', sha256=digest['json']), 'model.data': dict(path='data', sha256=digest['data']),
            'model.quantize': dict(path='quantize', sha256=digest['quantize']),
            'inputmeta.yml': dict(path='meta', sha256=digest['meta'])}))))

    def stage(name, change=None):
        t = copy.deepcopy(table)
        if change:
            change(t)
        (root / tool.REFERENCES).write_text(json.dumps(t))
        return tool.stage(Path(temporary) / name, root, cache)

    staged = stage('good')
    entry = staged['models']['conv8']
    assert entry['jobs'] == 9 and entry['base_vectors'] == 5 and entry['reference'] == 'compiler'
    assert entry['repeat'] == 'seeded'
    assert (Path(temporary) / 'good/inputs/conv8/model.nb').read_bytes() == files['nbg']
    harness.refused(lambda: stage('repeat', lambda t: t['models']['conv8'].update(repeat='absent')),
                    'no absent base vector to repeat')
    harness.refused(lambda: stage('nbg', lambda t: t['models']['conv8'].update(nbg='data')), 'conv8 NBG changed')
    harness.refused(lambda: stage('input', lambda t: t['models']['conv8'].update(inputs=['data'])),
                    'conv8 reference input changed')
    harness.refused(lambda: stage('count', lambda t: t['models']['conv8'].update(inputs=[])),
                    'one reference input file is required per input')
    harness.refused(lambda: stage('pin', lambda t: t['models']['conv8']['reference']['files']['model.data'].update(
        sha256='0' * 64)), 'conv8 pinned file model.data changed')
    harness.refused(lambda: stage('tool', lambda t: t['inspector'].update(sha256='0' * 64)), 'inspector changed')
    harness.refused(lambda: stage('set', lambda t: t['models'].update(extra=t['models']['conv8'])),
                    'name different models')
    harness.refused(lambda: stage('missing', lambda t: t['models']['conv8'].update(nbg='absent')),
                    'No such file', FileNotFoundError)

print('PASS NPU conformance harness: 84-job plan, 55 line vectors, coverage, comparisons, verdicts, timing and pins')
