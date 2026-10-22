#!/usr/bin/env python3
"""Compare NPU outputs from the SDK simulator with independent references.

  run OUT      On the host: verify every pinned identity, stage the inputs in
               OUT/work, run one container from the pinned compiler image and
               write OUT/report.json. OUT must not exist.
  worker WORK  Inside that container: build one simulator runner per model,
               run every vector as successive jobs on one prepared graph, run
               the references, compare, and write WORK/report.json.

The model set is tools/npu-session-models.json; the references and the files
they read are pinned in tools/npu-conformance-references.json. Every
comparison requires bit-for-bit equality. docs/npu-conformance.md describes
the vectors, the references and the report. The harness performs no device
I/O and never contacts USB devices or the board.

The worker runs under Python 3.8 in the compiler image, so this file avoids
newer syntax. Functions that decide the result use only the standard library,
which lets checks/npu-conformance.py exercise them on the host.
"""
import argparse
import concurrent.futures
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import resource
import struct
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
CATALOGUE = 'tools/npu-session-models.json'
REFERENCES = 'tools/npu-conformance-references.json'
RUNNER = 'tools/npu-conformance-runner.c'
CACHE = Path(os.environ.get('SUNSTONE_CACHE', str(Path.home() / '.cache/sunstoneos')))
SDK = '/root/Vivante_IDE/VivanteIDE5.11.0/cmdtools'
COMPILER = '/root/acuity-toolkit-whl-6.30.22/bin/pegasus.py'
# The environment that tools/npu-conv-pipeline.sh exports for the simulator.
SIMULATOR = {'VSIMULATOR_CONFIG': 'VIP9000NANODI_PLUS_PID0X1000003B', 'VSIMULATOR_SHADER_CORE_COUNT': '1'}
LINE = 64
WORKERS = 6
BUILD_LIMIT = 120       # seconds per runner build (tools/npu-conv-pipeline.sh)
REFERENCE_LIMIT = 120   # seconds per compiler inference (tools/npu-conv-pipeline.sh)
INSPECT_LIMIT = 60      # seconds per nbinfo report
START_LIMIT = 120       # seconds for container start
SEEDED_BASE = 4         # vector index of the seeded base vector
SEEDED_LINE = 5         # vector index of every input-line vector
AFFINE = 2              # catalogue quantization: affine asymmetric
# Catalogue buffer format -> struct code, minimum, maximum, ovxlib type, quantize-file qtype.
FORMATS = {2: ('B', 0, 255, 'VSI_NN_TYPE_UINT8', 'u8'),
           3: ('b', -128, 127, 'VSI_NN_TYPE_INT8', 'i8'),
           5: ('h', -32768, 32767, 'VSI_NN_TYPE_INT16', 'i16')}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load(relative, root=ROOT):
    return json.loads((Path(root) / relative).read_text())


# ---------------------------------------------------------------- tensors

def tensor_format(tensor):
    if tensor['format'] not in FORMATS or tensor['quantization'] != AFFINE:
        raise ValueError('unsupported tensor type in ' + tensor['name'])
    return FORMATS[tensor['format']]


def tensor_scale(tensor):
    return struct.unpack('<f', struct.pack('<I', tensor['scale_bits']))[0]


def float32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def codes(tensor, data):
    code = tensor_format(tensor)[0]
    if len(data) != tensor['logical_bytes']:
        raise ValueError('%s holds %d bytes, not %d' % (tensor['name'], len(data), tensor['logical_bytes']))
    return list(struct.unpack('<%d%s' % (len(data) // struct.calcsize(code), code), data))


def pack(tensor, values):
    return struct.pack('<%d%s' % (len(values), tensor_format(tensor)[0]), *values)


def elements(tensor):
    return tensor['logical_bytes'] // struct.calcsize(tensor_format(tensor)[0])


def lines(tensor):
    """Cache lines counted from the tensor's first byte; the runtime aligns each allocation to 4 KiB."""
    return (tensor['logical_bytes'] + LINE - 1) // LINE


# ---------------------------------------------------------------- vectors

def seeded(model, vector, length, line=None):
    """SHA-256 in counter mode over the model name, the vector index and an optional line index."""
    prefix = model.encode() + b'\0' + str(vector).encode()
    if line is not None:
        prefix += b'\0' + str(line).encode()
    out = bytearray()
    counter = 0
    while len(out) < length:
        out += hashlib.sha256(prefix + b'\0' + counter.to_bytes(8, 'little')).digest()
        counter += 1
    return bytes(out[:length])


def split(model, data):
    result, offset = [], 0
    for tensor in model['inputs']:
        result.append(data[offset:offset + tensor['logical_bytes']])
        offset += tensor['logical_bytes']
    return tuple(result)


def base_vectors(model, reference):
    """Distinct base vectors in canonical order as (label, inputs); the seeded vector is last."""
    inputs = model['inputs']
    total = sum(t['logical_bytes'] for t in inputs)

    def fill(value):
        return tuple(pack(t, [value(t)] * elements(t)) for t in inputs)

    candidates = [('reference', tuple(reference)),
                  ('zero point', fill(lambda t: t['zero_point'])),
                  ('minimum', fill(lambda t: tensor_format(t)[1])),
                  ('maximum', fill(lambda t: tensor_format(t)[2])),
                  ('seeded', split(model, seeded(model['name'], SEEDED_BASE, total)))]
    result = []
    for label, vector in candidates:
        if len(vector) != len(inputs) or any(len(v) != t['logical_bytes'] for v, t in zip(vector, inputs)):
            raise ValueError('%s %s vector has the wrong size' % (model['name'], label))
        if all(vector != earlier for _, earlier in result):
            result.append((label, vector))
    if result[-1][0] != 'seeded':
        raise ValueError(model['name'] + ': the seeded vector repeats an earlier vector')
    return result


def line_vectors(model, start):
    """One vector per 64-byte line of every input, each equal to the previous job's but in that line."""
    result, current, line = [], list(start), 0
    for index, tensor in enumerate(model['inputs']):
        for offset in range(0, tensor['logical_bytes'], LINE):
            end = min(offset + LINE, tensor['logical_bytes'])
            data = bytearray(current[index])
            data[offset:end] = seeded(model['name'], SEEDED_LINE, end - offset, line)
            current[index] = bytes(data)
            result.append(('input %d line %d' % (index, offset // LINE), tuple(current)))
            line += 1
    return result


def plan_jobs(model, reference, with_lines):
    """The runner's job sequence: base vectors, input-line vectors, then the seeded vector again."""
    base = base_vectors(model, reference)
    jobs = [dict(label=label, kind='base', vector=vector) for label, vector in base]
    if with_lines:
        jobs += [dict(label=label, kind='line', vector=vector) for label, vector in line_vectors(model, base[-1][1])]
    jobs.append(dict(label='seeded again', kind='repeat', vector=base[-1][1]))
    return jobs


# ---------------------------------------------------------------- comparison

def difference(tensor, expected, actual):
    """Differing codes, at most 16 of their positions and the largest code difference."""
    a, b = codes(tensor, expected), codes(tensor, actual)
    positions = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
    return dict(differing=len(positions), positions=positions[:16],
                largest=max([abs(a[i] - b[i]) for i in positions] or [0]))


def saturated(model, outputs):
    """True when every output code sits at a clamp limit of its type."""
    for tensor, data in zip(model['outputs'], outputs):
        _, low, high, _, _ = tensor_format(tensor)
        if any(v != low and v != high for v in codes(tensor, data)):
            return False
    return True


def board_order(outputs):
    """Order of the base vectors that maximizes consecutive pairs with different
    expected outputs; the seeded vector stays last, ties keep canonical order."""
    count = len(outputs)
    best = None
    for permutation in itertools.permutations(range(count - 1)):
        order = list(permutation) + [count - 1]
        score = sum(outputs[a] != outputs[b] for a, b in zip(order, order[1:]))
        if best is None or score > best[0]:
            best = (score, order)
    return best[1]


def changed_lines(before, after):
    return {offset // LINE for offset in range(0, len(after), LINE)
            if before[offset:offset + LINE] != after[offset:offset + LINE]}


def line_coverage(model, sequence):
    """Input and output lines not covered by a consecutive job pair with different
    expected outputs. sequence holds (inputs, outputs) per job in board order."""
    inputs = {(i, n) for i, t in enumerate(model['inputs']) for n in range(lines(t))}
    outputs = {(o, n) for o, t in enumerate(model['outputs']) for n in range(lines(t))}
    for (previous_in, previous_out), (current_in, current_out) in zip(sequence, sequence[1:]):
        if previous_out == current_out:
            continue
        for i, (a, b) in enumerate(zip(previous_in, current_in)):
            inputs -= {(i, n) for n in changed_lines(a, b)}
        for o, (a, b) in enumerate(zip(previous_out, current_out)):
            outputs -= {(o, n) for n in changed_lines(a, b)}
    return dict(input_lines=sum(lines(t) for t in model['inputs']),
                output_lines=sum(lines(t) for t in model['outputs']),
                uncovered_input_lines=sorted(inputs), uncovered_output_lines=sorted(outputs))


def mimus_int32_bias(x, rows, k, weights, n, weight_scales, bias, input_scale, scale, zero, low, high):
    """The int32-bias rule of tools/npu-mimus-linear.py compare(): an exact integer
    accumulator, the bias added as an int32 code in units of input_scale * ws[n],
    and one rounding (half to even) of the result to the output type."""
    out = []
    for r in range(rows):
        row = x[r * k:(r + 1) * k]
        for j in range(n):
            total = sum(a * b for a, b in zip(row, weights[j * k:(j + 1) * k]))
            bias_scale = weight_scales[j] * input_scale
            accumulated = (total + round(bias[j] / bias_scale)) * bias_scale
            out.append(min(max(round(accumulated / scale) + zero, low), high))
    return out


def evaluate(model, jobs, results, references, catalogued):
    """Decide one model's state from its simulator results.

    results holds, per job, dict(outputs=..., consumed=...) as tuples of bytes,
    or None when the runner failed. references holds, per base vector, the
    reference outputs, or None when the model has no reference. catalogued is
    the catalogue's output SHA-256 list for the reference input."""
    failures, rows = [], []
    if results is None or len(results) != len(jobs):
        return dict(state='fail', failures=['no simulator output'], vectors=[])
    for index, (job, result) in enumerate(zip(jobs, results)):
        sizes = [len(v) for v in result['outputs']]
        if sizes != [t['logical_bytes'] for t in model['outputs']]:
            failures.append('job %d: output sizes %s differ from the catalogue' % (index, sizes))
            continue
        if tuple(result['consumed']) != tuple(job['vector']):
            failures.append('job %d: the simulator consumed an input that differs from the vector' % index)
    if failures:
        return dict(state='fail', failures=failures, vectors=[])
    base = [i for i, job in enumerate(jobs) if job['kind'] == 'base']
    seeded_job, repeat = base[-1], len(jobs) - 1
    if results[seeded_job]['outputs'] != results[repeat]['outputs']:
        failures.append('the repeated seeded vector gave different outputs')
    first = [sha256(v) for v in results[0]['outputs']]
    if jobs[0]['label'] == 'reference' and first != list(catalogued):
        failures.append('the simulator output for the catalogued input differs from the catalogue')
    for position, index in enumerate(base):
        outputs = results[index]['outputs']
        row = dict(vector=jobs[index]['label'], job=index, saturated=saturated(model, outputs),
                   input_sha256=[sha256(v) for v in jobs[index]['vector']],
                   output_sha256=[sha256(v) for v in outputs])
        if references is not None:
            expected = references[position]
            if expected is None:
                failures.append('%s: no reference output' % row['vector'])
            else:
                row['differences'] = [difference(t, e, a) for t, e, a in zip(model['outputs'], expected, outputs)]
                if any(d['differing'] for d in row['differences']):
                    failures.append('%s: simulator output differs from the reference' % row['vector'])
        rows.append(row)
    order = [base[i] for i in board_order([results[i]['outputs'] for i in base])]
    order += [i for i, job in enumerate(jobs) if job['kind'] != 'base']
    sequence = [(jobs[i]['vector'], results[i]['outputs']) for i in order]
    pairs = sum(a[1] != b[1] for a, b in zip(sequence, sequence[1:]))
    return dict(state='fail' if failures else 'pass', failures=failures, vectors=rows,
                board_job_order=[jobs[i]['label'] for i in order],
                pairs_with_different_outputs=pairs, coverage=line_coverage(model, sequence),
                unsaturated_vectors=sum(not r['saturated'] for r in rows))


def bindings(model):
    """The runner's tensor bindings, generated from the catalogue descriptors."""
    rows = []
    for tensor in model['inputs'] + model['outputs']:
        dims = tensor['dimensions']
        if not 1 <= len(dims) <= 4:
            raise ValueError('unsupported rank in ' + tensor['name'])
        size = ', '.join(str(d) for d in dims + [0] * (4 - len(dims)))
        rows.append('    { %d, { %s }, %s, %sf, %d, %d },' % (
            len(dims), size, tensor_format(tensor)[3], tensor_scale(tensor).hex(),
            tensor['zero_point'], tensor['logical_bytes']))
    return '\n'.join(['/* Generated by tools/npu-conformance.py for %s. */' % model['name'],
                      '#define NPU_CONFORMANCE_INPUTS %d' % len(model['inputs']),
                      '#define NPU_CONFORMANCE_OUTPUTS %d' % len(model['outputs']),
                      'static const struct npu_conformance_tensor npu_conformance_tensors[] = {']
                     + rows + ['};', ''])


def invocation_limit(entry, jobs):
    """The one-vector limit plus four times the measured per-vector time for each further vector."""
    return entry['one_vector_limit'] + 4 * entry['per_vector_seconds'] * (jobs - 1)


def container_limit(plan):
    total = START_LIMIT
    for name, entry in plan['models'].items():
        total += BUILD_LIMIT + INSPECT_LIMIT + invocation_limit(entry, entry['jobs'])
        if entry['reference'] == 'compiler':
            total += REFERENCE_LIMIT * entry['base_vectors']
    return total


# ---------------------------------------------------------------- host

def pinned(path, digest, what):
    data = Path(path).read_bytes()
    if sha256(data) != digest:
        raise ValueError('%s changed: %s' % (what, path))
    return data


def stage(work, root=ROOT, cache=CACHE):
    """Verify every pinned identity and copy the inputs into work/inputs."""
    catalogue = load(CATALOGUE, root)
    references = load(REFERENCES, root)
    models = {m['name']: m for m in catalogue['models']}
    if set(models) != set(references['models']):
        raise ValueError('the reference table and the catalogue name different models')
    inspector = references['inspector']
    (work / 'inputs').mkdir(parents=True)
    (work / 'inputs/nbinfo').write_bytes(pinned(cache / inspector['path'], inspector['sha256'], 'inspector'))
    (work / 'inputs/nbinfo').chmod(0o755)
    plan = dict(models={}, inspector_version=inspector['version'],
                runner_sha256=sha256((Path(root) / RUNNER).read_bytes()))
    for name, model in models.items():
        entry = references['models'][name]
        directory = work / 'inputs' / name
        directory.mkdir()
        (directory / 'model.nb').write_bytes(pinned(cache / entry['nbg'], model['model']['sha256'], name + ' NBG'))
        if len(entry['inputs']) != len(model['inputs']):
            raise ValueError(name + ': one reference input file is required per input')
        for index, (path, tensor) in enumerate(zip(entry['inputs'], model['inputs'])):
            (directory / ('input%d.bin' % index)).write_bytes(
                pinned(cache / path, tensor['reference']['sha256'], name + ' reference input'))
        reference = entry['reference']
        files = {}
        for role, item in sorted((reference or {}).get('files', {}).items()):
            base = root if item.get('repository') else cache
            data = pinned(Path(base) / item['path'], item['sha256'], '%s pinned file %s' % (name, role))
            (directory / role).write_bytes(data)
            files[role] = item['sha256']
        with_lines = reference is not None
        jobs = plan_jobs(model, [(directory / ('input%d.bin' % i)).read_bytes()
                                 for i in range(len(model['inputs']))], with_lines)
        plan['models'][name] = dict(
            catalogue=model, reference=reference['kind'] if reference else None,
            layer=(reference or {}).get('layer'), files=files, lines=with_lines,
            jobs=len(jobs), base_vectors=sum(j['kind'] == 'base' for j in jobs),
            per_vector_seconds=entry['per_vector_seconds'], one_vector_limit=entry['one_vector_limit'])
    return plan


def staged_unchanged(work, plan):
    for name, entry in plan['models'].items():
        directory = work / 'inputs' / name
        if sha256((directory / 'model.nb').read_bytes()) != entry['catalogue']['model']['sha256']:
            raise ValueError(name + ': the staged NBG changed during the run')
        for role, digest in entry['files'].items():
            if sha256((directory / role).read_bytes()) != digest:
                raise ValueError('%s: staged file %s changed during the run' % (name, role))


def run(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    work = out / 'work'
    lock = load('tools/npu-compiler.lock.json')
    plan = stage(work)
    (work / 'plan.json').write_text(json.dumps(plan, indent=1) + '\n')
    # The container runs as its root without capabilities; it writes only here.
    work.chmod(0o777)
    limit = container_limit(plan)
    name = 'npu-conformance-%d-%d' % (int(time.time()), os.getpid())
    command = ['docker', 'run', '--name', name, '--network', 'none', '--cpus', str(WORKERS),
               '--memory', '32g', '--pids-limit', '1024', '--cap-drop', 'ALL',
               '--security-opt', 'no-new-privileges', '-v', '%s:/source:ro' % ROOT,
               '-v', '%s:/work' % work, lock['loaded_image_id'],
               'nice', '-n', '10', 'python3', '/source/tools/npu-conformance.py', 'worker', '/work']
    failure = None
    start = time.monotonic()
    with open(str(out / 'container.log'), 'w') as log:
        try:
            status = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=limit).returncode
            if status:
                failure = 'the container exited with status %d' % status
        except subprocess.TimeoutExpired:
            subprocess.run(['docker', 'kill', name], stdout=log, stderr=subprocess.STDOUT)
            failure = 'the container exceeded %d s' % limit
        finally:
            subprocess.run(['docker', 'rm', '-f', name], stdout=log, stderr=subprocess.STDOUT)
    wall = time.monotonic() - start
    staged_unchanged(work, plan)
    report_path = work / 'report.json'
    report = json.loads(report_path.read_text()) if report_path.exists() else dict(result='fail', models={})
    if failure:
        report['result'] = 'fail'
        report.setdefault('failures', []).append(failure)
    report['host'] = dict(container_starts=1, container_name=name, container_limit_seconds=limit,
                          wall_seconds=round(wall, 3), image=lock['loaded_image_id'])
    (out / 'report.json').write_text(json.dumps(report, indent=1) + '\n')
    for model, state in sorted(report.get('models', {}).items()):
        print('%-26s %s' % (model, state['state']) + ''.join('\n    ' + f for f in state['failures']))
    for line in report.get('failures', []):
        print('run:', line)
    print('%s in %.1f s; report %s' % (report['result'].upper(), wall, out / 'report.json'))
    return 0 if report['result'] == 'pass' else 1


# ---------------------------------------------------------------- worker

class Processes:
    """Runs child processes from at most WORKERS threads and counts them."""

    def __init__(self):
        self.lock = threading.Lock()
        self.active = self.peak = 0
        self.counts = {}
        self.seconds = {}

    def run(self, kind, command, timeout, cwd, log, env=None):
        with self.lock:
            self.active += 1
            self.peak = max(self.peak, self.active)
            self.counts[kind] = self.counts.get(kind, 0) + 1
        start = time.monotonic()
        try:
            with open(str(log), 'w') as output:
                status = subprocess.run(command, cwd=str(cwd), env=env, stdout=output,
                                        stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            raise RuntimeError('%s exceeded %d s; see %s' % (kind, timeout, log))
        finally:
            elapsed = time.monotonic() - start
            with self.lock:
                self.active -= 1
                self.seconds.setdefault(kind, []).append(round(elapsed, 3))
        if status:
            raise RuntimeError('%s exited with status %d; see %s' % (kind, status, log))
        return elapsed


def simulate(processes, work, name, entry, jobs):
    model = entry['catalogue']
    directory = work / 'sim' / name
    for index, job in enumerate(jobs):
        (directory / 'jobs' / str(index)).mkdir(parents=True)
        for i, data in enumerate(job['vector']):
            (directory / 'jobs' / str(index) / ('input%d.bin' % i)).write_bytes(data)
    (directory / 'npu-conformance-bindings.h').write_text(bindings(model))
    sdk = SDK + '/vsimulator'
    processes.run('runner build', [
        'gcc', '-std=gnu99', '-O2', '-Wall', '-Wextra', '-Werror', '-D__linux__', '-DLINUX', '-I.',
        '-I' + sdk + '/include', '-I' + sdk + '/include/ovxlib', str(ROOT / RUNNER), '-o', 'runner',
        '-L' + sdk + '/lib', '-lovxlib', '-lOpenVX', '-lOpenVXU', '-lCLC', '-lVSC', '-lGAL', '-lEmulator',
        '-lvdtproxy', '-lArchModelSw', '-lNNArchPerf', '-L' + SDK + '/common/lib', '-lvdtproxy'],
        BUILD_LIMIT, directory, directory / 'build.log')
    env = dict(os.environ, LD_LIBRARY_PATH=sdk + '/lib:' + SDK + '/common/lib', **SIMULATOR)
    limit = invocation_limit(entry, len(jobs))
    processes.run('runner invocation', ['./runner', str(work / 'inputs' / name / 'model.nb'), 'jobs', str(len(jobs))],
                  limit, directory, directory / 'runner.log', env)
    results = []
    for index in range(len(jobs)):
        job = directory / 'jobs' / str(index)
        results.append(dict(outputs=tuple((job / ('output%d.bin' % o)).read_bytes() for o in range(len(model['outputs']))),
                            consumed=tuple((job / ('consumed%d.bin' % i)).read_bytes() for i in range(len(model['inputs'])))))
    times = (directory / 'jobs/times').read_text().split('\n')
    prepare = int(times[0].split()[1]) / 1e9
    per_job = [int(line.split()[2]) / 1e9 for line in times[1:] if line]
    return results, dict(prepare_seconds=round(prepare, 6), job_seconds_max=round(max(per_job), 6),
                         job_seconds_mean=round(sum(per_job) / len(per_job), 6), invocation_limit_seconds=limit)


def tensor_dump(directory, prefix, index, tensor):
    pattern = re.compile(r'^iter_0_%s(\d*)_.*\.qnt\.tensor$' % prefix)
    found = [p for p in directory.iterdir() if pattern.match(p.name)
             and int(pattern.match(p.name).group(1) or 0) == index]
    if len(found) != 1:
        raise RuntimeError('expected one %s%d dump in %s' % (prefix, index, directory))
    values = []
    for text in found[0].read_text().split():
        value = float(text)
        if value != int(value):
            raise RuntimeError('non-integer code in ' + found[0].name)
        values.append(int(value))
    if len(values) != elements(tensor):
        raise RuntimeError('%s holds %d codes, not %d' % (found[0].name, len(values), elements(tensor)))
    return pack(tensor, values)


def compiler_reference(processes, work, name, entry, position, vector):
    """Compiler quantized CPU inference of one base vector."""
    import numpy as np
    import yaml
    model = entry['catalogue']
    inputs = work / 'inputs' / name
    directory = work / 'ref' / name / str(position)
    directory.mkdir(parents=True)
    meta = yaml.safe_load((inputs / 'inputmeta.yml').read_text())
    quantize = yaml.safe_load((inputs / 'model.quantize').read_text())['quantize_parameters']
    databases = meta['input_meta']['databases']
    if len(databases) != len(model['inputs']):
        raise RuntimeError('input meta and catalogue disagree on the input count')
    for index, (tensor, database, data) in enumerate(zip(model['inputs'], databases, vector)):
        port = database['ports'][0]
        shape = list(reversed(tensor['dimensions']))
        q = quantize['@%s:out0' % port['lid']]
        if (port['shape'] != shape or q['qtype'] != tensor_format(tensor)[4] or q['zero_point'] != tensor['zero_point']
                or float32(q['scale']) != tensor_scale(tensor)):
            raise RuntimeError('input %d quantization or shape differs from the catalogue' % index)
        values = np.array([(c - tensor['zero_point']) * tensor_scale(tensor) for c in codes(tensor, data)], np.float32)
        np.save(str(directory / ('input%d.npy' % index)), values.reshape(shape), allow_pickle=False)
        database['path'] = str(directory / ('input%d.npy' % index))
    (directory / 'inputmeta.yml').write_text(yaml.safe_dump(meta, sort_keys=False))
    processes.run('compiler inference', [
        'python3', COMPILER, 'inference', '--model', str(inputs / 'model.json'),
        '--model-data', str(inputs / 'model.data'), '--model-quantize', str(inputs / 'model.quantize'),
        '--device', 'CPU', '--with-input-meta', 'inputmeta.yml', '--dtype', 'quantized', '--iterations', '1',
        '--postprocess', 'dump_results', '--output-dir', 'out'], REFERENCE_LIMIT, directory, directory / 'inference.log')
    out = directory / 'out'
    for index, (tensor, data) in enumerate(zip(model['inputs'], vector)):
        if tensor_dump(out, 'input', index, tensor) != data:
            raise RuntimeError('the compiler consumed input %d differently from the vector' % index)
    return tuple(tensor_dump(out, 'attach_output', o, t) for o, t in enumerate(model['outputs']))


def mimus_reference(work, name, entry, base):
    """The int32-bias rule over the pinned mimus weights, checked against the model file."""
    import importlib.util
    import numpy as np
    import yaml
    model = entry['catalogue']
    inputs = work / 'inputs' / name
    spec = importlib.util.spec_from_file_location('mimus_linear', str(inputs / 'npu-mimus-linear.py'))
    mimus = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mimus)
    meta = json.loads((inputs / 'meta.json').read_text())
    layers = mimus.tensors(str(inputs / 'model.mimus'))
    _, _, shape, weight_codes, weight_scales = layers[entry['layer'] + '.weight']
    bias = layers[entry['layer'] + '.bias'][3]
    n, k = shape
    weights = (inputs / 'w.i8').read_bytes()
    scales = struct.unpack('<%df' % n, (inputs / 'ws.f32').read_bytes())
    biases = struct.unpack('<%df' % n, (inputs / 'bias.f32').read_bytes())
    if (weights != np.ascontiguousarray(weight_codes).astype(np.int8).tobytes()
            or list(scales) != [float(v) for v in weight_scales.astype(np.float32)]
            or list(biases) != [float(v) for v in bias.astype(np.float32)]):
        raise RuntimeError('pinned mimus weights differ from the model file')
    quantize = yaml.safe_load((inputs / 'model.quantize').read_text())['quantize_parameters']
    q = next(v for key, v in quantize.items() if key.startswith('@attach_output'))
    (tensor,), (output,) = model['inputs'], model['outputs']
    _, low, high, _, qtype = tensor_format(output)
    if (q['qtype'] != qtype or q['zero_point'] != output['zero_point'] or float32(q['scale']) != tensor_scale(output)
            or tensor['dimensions'] != [k, meta['rows']] or output['dimensions'] != [n, meta['rows']]):
        raise RuntimeError('mimus quantization or shape differs from the catalogue')
    weights = list(struct.unpack('<%db' % len(weights), weights))
    return [(pack(output, mimus_int32_bias(codes(tensor, vector[0]), meta['rows'], k, weights, n, scales, biases,
                                           meta['input_scale'], q['scale'], q['zero_point'], low, high)),)
            for _, vector in base]


def inspect(processes, work, name):
    """Hardware layer names and operation kinds from the SDK inspector."""
    log = work / 'sim' / ('%s.nbinfo' % name)
    processes.run('inspector', [str(work / 'inputs/nbinfo'), '-a', str(work / 'inputs' / name / 'model.nb')],
                  INSPECT_LIMIT, work, log)
    text = log.read_text()
    version = re.search(r'nbinfo version (\S+)', text)
    layers = sorted(set(re.findall(r'^Layer Name:\s+(\S+)', text, re.M)))
    operations = sorted(set(re.findall(r'^Operation Type:\s+(\S+)', text, re.M)))
    return dict(version=version.group(1) if version else None, layers=layers or None, operations=operations or None)


def worker(args):
    os.umask(0)
    work = args.work
    plan = json.loads((work / 'plan.json').read_text())
    processes = Processes()
    start = time.monotonic()
    jobs, models, tasks = {}, {}, {}
    for name, entry in plan['models'].items():
        model = entry['catalogue']
        reference = [(work / 'inputs' / name / ('input%d.bin' % i)).read_bytes() for i in range(len(model['inputs']))]
        jobs[name] = plan_jobs(model, reference, entry['lines'])
    # Longest runner invocations first, so they start before the references.
    order = sorted(plan['models'], key=lambda n: -invocation_limit(plan['models'][n], len(jobs[n])))
    with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as pool:
        for name in order:
            entry = plan['models'][name]
            tasks[(name, 'simulator')] = pool.submit(simulate, processes, work, name, entry, jobs[name])
        for name in order:
            entry = plan['models'][name]
            if entry['reference'] == 'compiler':
                base = [j['vector'] for j in jobs[name] if j['kind'] == 'base']
                for position, vector in enumerate(base):
                    tasks[(name, position)] = pool.submit(compiler_reference, processes, work, name, entry, position, vector)
            tasks[(name, 'inspector')] = pool.submit(inspect, processes, work, name)
    for name in plan['models']:
        entry, model = plan['models'][name], plan['models'][name]['catalogue']
        failures = []

        def outcome(key):
            try:
                return tasks[key].result()
            except Exception as error:  # every failure is reported and fails the run
                failures.append('%s: %s' % (key[1], error))
                return None

        simulated = outcome((name, 'simulator'))
        results, timing = simulated if simulated else (None, None)
        base = [(j['label'], j['vector']) for j in jobs[name] if j['kind'] == 'base']
        references = None
        if entry['reference'] == 'compiler':
            references = [outcome((name, position)) for position in range(len(base))]
        elif entry['reference'] == 'mimus-int32-bias':
            try:
                references = mimus_reference(work, name, entry, base)
            except Exception as error:
                failures.append('mimus reference: %s' % error)
                references = [None] * len(base)
        inventory = outcome((name, 'inspector'))
        if inventory and inventory['version'] != plan['inspector_version']:
            failures.append('inspector: version %s' % inventory['version'])
        if entry['reference'] and not (inventory and inventory['layers']):
            failures.append('inspector: no layer table for a pinned-compiler model')
        state = evaluate(model, jobs[name], results, references,
                         [t['reference']['sha256'] for t in model['outputs']])
        state['failures'] = failures + state['failures']
        if state['failures']:
            state['state'] = 'fail'
        if not entry['lines']:
            state['coverage'] = None
        state.update(reference=entry['reference'], jobs=len(jobs[name]), timing=timing,
                     inventory=inventory or dict(layers=None, operations=None),
                     tensor_types=sorted({tensor_format(t)[4] for t in model['inputs'] + model['outputs']}))
        models[name] = state
    compared = {l for m in models.values() if m['reference'] for l in (m['inventory']['layers'] or [])}
    every = {l for m in models.values() for l in (m['inventory']['layers'] or [])}
    counts = processes.counts
    report = dict(
        result='pass' if all(m['state'] == 'pass' for m in models.values()) else 'fail',
        physical_access=False, models=models,
        layers=dict(simulator_against_reference=sorted(compared), uncovered=sorted(every - compared),
                    unknown=sorted(n for n, m in models.items() if not m['inventory']['layers'])),
        board='pending: no board capture exists',
        counters=dict(
            simulator_jobs=sum(len(j) for j in jobs.values()),
            base_vectors=sum(j['kind'] == 'base' for js in jobs.values() for j in js),
            line_vectors=sum(j['kind'] == 'line' for js in jobs.values() for j in js),
            repeats=sum(j['kind'] == 'repeat' for js in jobs.values() for j in js),
            runner_builds=counts.get('runner build', 0), runner_invocations=counts.get('runner invocation', 0),
            compiler_commands=counts.get('compiler inference', 0), inspector_commands=counts.get('inspector', 0),
            peak_concurrent_processes=processes.peak,
            peak_child_rss_mib=resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss // 1024),
        step_seconds=processes.seconds, worker_seconds=round(time.monotonic() - start, 3),
        runner_sha256=plan['runner_sha256'])
    (work / 'report.json').write_text(json.dumps(report, indent=1) + '\n')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command')
    sub.required = True
    r = sub.add_parser('run')
    r.add_argument('output', type=Path, help='new output directory')
    w = sub.add_parser('worker')
    w.add_argument('work', type=Path)
    args = parser.parse_args()
    return run(args) if args.command == 'run' else worker(args)


if __name__ == '__main__':
    sys.exit(main())
