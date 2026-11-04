#!/usr/bin/env python3
"""Build the compiled test graphs of the NPU checks (build/npu-graph-variants) from tools/npu-fixtures.json.

For each graph the specification names, one container of the pinned compiler image (offline, with a memory limit) runs
  1. the graph generator, tools/npu-graph-variants.py or tools/npu-shader-variants.py, which writes fragment.onnx and the
     input tensors from a fixed seed;
  2. tools/npu-graph-compile.sh, which compiles the fragment to export_nbg_unify/network_binary.nb;
  3. tools/npu-graph-simulate.sh, which runs the network binary in the SDK simulator. The simulator writes the tensors it
     was given (export_nbg_unify/input_N.dat) and the tensors it computed (output<N>_*.dat).
The graph directory then holds input<N>.bin and reference<N>.bin (those files, byte for byte), and artifacts.json maps each
catalogue name (graph-NAME) to its model, input and reference files, the form tools/npu-session-catalogue.py reads.

The build is idempotent. A graph is skipped when its directory holds build.json with the digest of its specification, the
three tools above and the compiler image. Every result is checked against the reviewed identity in
tools/npu-fixtures.json (model size and SHA-256, SHA-256 of each input and output tensor); a graph that differs is not
installed. Staging happens in OUTPUT/.staging (a failed graph stays there for inspection); a replaced directory moves to CACHE/trash.

Usage: npu-graph-build.py [--output DIR] [--jobs N] [--memory 12g] [--cpus N] [GRAPH ...]
"""
import argparse
import concurrent.futures
import fcntl
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
GENERATORS = {'variants': 'npu-graph-variants.py', 'shader': 'npu-shader-variants.py'}
TOOLS = ('npu-graph-variants.py', 'npu-shader-variants.py', 'npu-graph-compile.sh', 'npu-graph-simulate.sh')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def stamp(entry, image):
    """What a built graph depends on: its specification, the tools that make it and the compiler image."""
    parts = {'entry': entry, 'image': image, 'tools': {t: sha256((ROOT / 'tools' / t).read_bytes()) for t in TOOLS}}
    return sha256(json.dumps(parts, sort_keys=True).encode())


def numbered(directory, pattern):
    """The files of a directory that match pattern (one group: the tensor number), in tensor order."""
    found = [(int(m.group(1)), p) for p in directory.iterdir() if (m := re.fullmatch(pattern, p.name))]
    return [p for _, p in sorted(found)]


def identity(name):
    models = {m['name']: m for m in json.loads((ROOT / 'tools/npu-fixtures.json').read_text())['models']}
    return models.get('graph-' + name)


def verify(name, directory):
    """Compare the finished graph with its reviewed identity. Returns a list of differences."""
    wanted = identity(name)
    if wanted is None:
        return []
    differences = []
    model = (directory / 'export_nbg_unify/network_binary.nb').read_bytes()
    if (len(model), sha256(model)) != (wanted['model']['bytes'], wanted['model']['sha256']):
        differences.append('model %d bytes sha256 %s, reviewed %d bytes %s' % (len(model), sha256(model), wanted['model']['bytes'], wanted['model']['sha256']))
    for kind, field in (('input', 'inputs'), ('reference', 'outputs')):
        files = numbered(directory, kind + r'(\d+)\.bin')
        if len(files) != len(wanted[field]):
            differences.append('%d %s files, reviewed %d' % (len(files), kind, len(wanted[field])))
            continue
        for k, (path, tensor) in enumerate(zip(files, wanted[field])):
            data = path.read_bytes()
            if (len(data), sha256(data)) != (tensor['reference']['bytes'], tensor['reference']['sha256']):
                differences.append('%s%d.bin: %d bytes sha256 %s, reviewed %d bytes %s' % (kind, k, len(data), sha256(data), tensor['reference']['bytes'], tensor['reference']['sha256']))
    return differences


def build_one(entry, output, image, memory, cpus, token):
    name = entry['name']
    staging = output / '.staging' / ('%s-%d' % (name, time.time_ns()))
    staging.mkdir(parents=True)
    work = staging / name
    work.mkdir()
    item = {k: v for k, v in entry.items() if k not in ('tool', 'simulator_input')}
    uid, gid = os.getuid(), os.getgid()
    inside = ('trap "chown -R %d:%d /work" EXIT; chown -R 0:0 /work; cd /work && '
              'python3 /tools/%s /work %s && bash /tools/npu-graph-compile.sh /work/%s && bash /tools/npu-graph-simulate.sh /work/%s'
              % (uid, gid, GENERATORS[entry['tool']], shlex_quote(json.dumps([item])), name, name))
    # The graph directory is mounted as /work/NAME's parent: the generator creates NAME under /work.
    command = ['docker', 'run', '--rm', '--network', 'none', '--memory', memory, '--memory-swap', memory, '--cpus', str(cpus),
               '--pids-limit', '512', '--cap-drop', 'ALL', '--cap-add', 'CHOWN', '-e', 'NPU_SIM_INPUT=' + entry.get('simulator_input', 'half'),
               '-v', '%s:/work' % staging, '-v', '%s:/tools:ro' % (ROOT / 'tools'), image, 'bash', '-c', inside]
    started = time.monotonic()
    done = subprocess.run(command, capture_output=True, text=True)
    (staging / 'container.log').write_text(done.stdout + done.stderr)
    if done.returncode != 0 or 'SIMULATED' not in done.stdout:
        return name, 'container failed (exit %d); log %s' % (done.returncode, staging / 'container.log'), 0
    export = work / 'export_nbg_unify'
    for kind, pattern, target in (('input', r'input_(\d+)\.dat', 'input%d.bin'), ('reference', r'output(\d+)_.*\.dat', 'reference%d.bin')):
        files = numbered(export, pattern)
        if not files:
            return name, 'no %s tensors in %s' % (kind, export), 0
        for k, path in enumerate(files):
            shutil.copyfile(path, work / (target % k))
    differences = verify(name, work)
    if differences:
        return name, 'differs from the reviewed identity (kept in %s): %s' % (work, '; '.join(differences)), 0
    (work / 'build.json').write_text(json.dumps({'stamp': token, 'seconds': round(time.monotonic() - started, 1)}) + '\n')
    trash = CACHE / 'trash'
    if output.joinpath(name).exists():
        trash.mkdir(parents=True, exist_ok=True)
        output.joinpath(name).rename(trash / ('%s-%d' % (name, time.time_ns())))
    output.mkdir(parents=True, exist_ok=True)
    work.rename(output / name)
    return name, None, time.monotonic() - started


def shlex_quote(text):
    return "'" + text.replace("'", "'\\''") + "'"


def built(output, name, token):
    d = output / name
    try:
        recorded = json.loads((d / 'build.json').read_text())['stamp']
    except (OSError, ValueError, KeyError):
        return False
    return recorded == token and (d / 'export_nbg_unify/network_binary.nb').is_file() and (d / 'input0.bin').is_file() and (d / 'reference0.bin').is_file()


def write_artifacts(output, entries):
    artifacts = {}
    for entry in entries:
        d = output / entry['name']
        artifacts['graph-' + entry['name']] = {
            'model': str(d / 'export_nbg_unify/network_binary.nb'),
            'inputs': [str(p) for p in numbered(d, r'input(\d+)\.bin')],
            'references': [str(p) for p in numbered(d, r'reference(\d+)\.bin')]}
    text = json.dumps(artifacts, indent=1) + '\n'
    target = output / 'artifacts.json'
    if not target.is_file() or target.read_text() != text:
        target.write_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, default=CACHE / 'build/npu-graph-variants')
    parser.add_argument('--jobs', type=int, default=4, help='graphs compiled at once (each container is limited to --memory and --cpus)')
    parser.add_argument('--memory', default='12g')
    parser.add_argument('--cpus', type=float, default=3)
    parser.add_argument('graphs', nargs='*', help='build only these graphs (default: all)')
    args = parser.parse_args()
    output = args.output.resolve()
    entries = json.loads((ROOT / 'tools/npu-fixtures.json').read_text())['graphs']
    names = [e['name'] for e in entries]
    unknown = sorted(set(args.graphs) - set(names))
    if unknown:
        sys.exit('unknown graphs: ' + ' '.join(unknown))
    lock = json.loads((ROOT / 'tools/npu-compiler.lock.json').read_text())
    image = lock['loaded_image_id']
    if subprocess.run(['docker', 'image', 'inspect', image], capture_output=True).returncode != 0:
        sys.exit('the compiler image %s is not loaded (tools/npu-compiler.lock.json)' % image)
    output.mkdir(parents=True, exist_ok=True)
    with (output / '.build.lock').open('w') as guard:
        fcntl.flock(guard, fcntl.LOCK_EX)
        tokens = {e['name']: stamp(e, image) for e in entries}
        wanted = [e for e in entries if not args.graphs or e['name'] in args.graphs]
        todo = [e for e in wanted if not built(output, e['name'], tokens[e['name']])]
        print('%d graphs, %d built already, %d to build' % (len(wanted), len(wanted) - len(todo), len(todo)), flush=True)
        failures = []
        started = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            futures = [pool.submit(build_one, e, output, image, args.memory, args.cpus, tokens[e['name']]) for e in todo]
            for future in concurrent.futures.as_completed(futures):
                name, problem, seconds = future.result()
                print(('FAIL %s: %s' % (name, problem)) if problem else ('built %s in %.0f s' % (name, seconds)), flush=True)
                if problem:
                    failures.append(name)
        if failures:
            sys.exit('failed: ' + ' '.join(failures))
        present = [e for e in entries if built(output, e['name'], tokens[e['name']])]
        write_artifacts(output, present)
        print('PASS %d graphs in %s (%d rebuilt in %.0f s)' % (len(present), output, len(todo), time.monotonic() - started))


if __name__ == '__main__':
    main()
