#!/usr/bin/env python3
"""The NPU graph application (apps/npu-graphs/main.c) runs on the host against a model of the OS side of its SDK calls.

The model (checks/npu-graphs-app/sdk.c) applies the OS's rules: run-time registration answered by the NPU task and bounded
by the declaration (graphs, graph_bytes, and tensor_bytes for the read and write regions the OS layout gives each graph,
tools/npu_graph_layout.py); the EL0 ring, which opens only when every declared slot holds a graph, refuses submits while the
declared depth is in flight and joins linked tensors; and ss_finish, which stops at detail 0x75f unless the client was
closed. graph-add and graph-relu are computed, so a linked pair gives the output the device would.

The application is built with the vectors, the manifest header and the model list of the board images:
- the queue image: graph-relu under 31 names (apps/npu-graphs/queue-aliases.py) with apps/npu-graphs/manifest-queue.json.
  Its receipt must equal the board expectation (apps/npu-graphs/host.py queue_expectation for k = 1 to 128, 1,000 jobs, and
  the link), with no refusal for k up to 64 and refusals for k = 128;
- the one-run-each image: graph-add and graph-relu with apps/npu-graphs/manifest.json. Both graphs must match.
graph-add under 31 names is refused as a queue image: its linked pair cannot reproduce the reference. Negative and mutation
cases: a queue list shorter than the 31 declared slots has the open refused (fault 0xb001); a main.c
that opens the ring without registering faults the same way, and one that guards on the graphs the OS image links (none)
faults at 0xa100.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path.home() / '.cache/sunstoneos'
CHECK_INPUTS = ('apps/npu-graphs/main.c', 'apps/npu-graphs/vectors.py', 'apps/npu-graphs/host.py', 'apps/npu-graphs/receipt.h',
                'apps/npu-graphs/vectors.h', 'apps/npu-graphs/queue-aliases.py', 'apps/npu-graphs/manifest-queue.json',
                'apps/npu-graphs/manifest.json', 'include/sunstone_app.h', 'include/sunstone_npu_queue.h',
                'tools/sunstone-manifest.py', 'tools/npu_graph_layout.py', 'checks/npu-graphs-app/sdk.c')
GATE_RUNS = [['--output', '{output}', '--graphs', '{fixture:npu-graphs}']]
CLANG = '/usr/bin/clang-21'
MODEL = ROOT / 'checks/npu-graphs-app'
APP = ROOT / 'apps/npu-graphs'
DEPTHS = [1, 2, 4, 8, 16, 32, 64, 128]

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--graphs', type=Path, default=CACHE / 'build/npu-graph-variants', help='the test graphs (artifacts.json)')
p.add_argument('--main', type=Path, default=APP / 'main.c', help='the application source (a mutation case passes another)')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(ROOT / 'tools'))
import npu_graph_layout  # noqa: E402
spec = importlib.util.spec_from_file_location('npu_graphs_host', APP / 'host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
artifacts_path = a.graphs / 'artifacts.json'
artifacts = json.loads(artifacts_path.read_text())
fixtures = json.loads((ROOT / 'tools/npu-fixtures.json').read_text())
kinds = {next(m for m in fixtures['models'] if m['name'] == n)['model']['sha256']: k for n, k in (('graph-add', 1), ('graph-relu', 2))}


def run(command, **options):
    done = subprocess.run(command, capture_output=True, text=True, **options)
    if done.returncode:
        raise SystemExit('%s failed:\n%s%s' % (command[0], done.stdout, done.stderr))
    return done


def image(case, names, database, manifest, queue, main=None):
    """Build the application for one model list and run it: (exit status, stdout, receipt bytes)."""
    out = a.output / case
    out.mkdir(parents=True, exist_ok=True)
    (out / 'linked.json').write_text('{"npu_models": []}\n')
    (out / 'list.json').write_text(json.dumps({'npu_models': names}) + '\n')
    env = dict(os.environ, SUNSTONE_NPU_MODEL_DB=str(database))
    run([sys.executable, str(APP / 'vectors.py'), '--artifacts', str(database.parent / 'artifacts.json'), '--models',
         str(out / 'linked.json'), '--runtime-models', str(out / 'list.json'), '--output', str(out / 'vectors.c')], env=env)
    run([sys.executable, str(ROOT / 'tools/sunstone-manifest.py'), 'emit', '--manifest', str(manifest), '--output', str(out / 'app')])
    entries = {m['name']: m for m in json.loads(database.read_text())['models']}
    table = json.loads((database.parent / 'artifacts.json').read_text())
    layout = npu_graph_layout.regions([table[n]['model'] for n in names])
    rows = []
    for name, region in zip(names, layout):
        e = entries[name]
        sizes = lambda field: ','.join(str(t['logical_bytes']) for t in e[field]) or '0'
        rows.append('{%d,%d,%d,{%s},{%s},%d,%d}' % (kinds.get(e['model']['sha256'], 0), len(e['inputs']), len(e['outputs']),
                                                   sizes('inputs'), sizes('outputs'), region['read'], region['write']))
    (out / 'model_graphs.c').write_text(
        '#include <stddef.h>\nstruct model_graph { int kind; unsigned inputs, outputs; size_t in_bytes[4], out_bytes[4]; size_t read, write; };\n'
        'const struct model_graph model_graphs[] = {%s};\n' % ','.join(rows))
    npu = json.loads(manifest.read_text())['npu']
    decl = ['-DNPU_GRAPHS_DECL_%s=%du' % (k.upper(), npu[k]) for k in ('slots', 'in_flight', 'graphs', 'links', 'graph_bytes', 'tensor_bytes')]
    binary = out / 'application'
    run([CLANG, '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-w',
         '-I' + str(MODEL), '-I' + str(out / 'app'), '-I' + str(APP), '-I' + str(ROOT / 'include'), *decl,
         *(['-DNPU_GRAPHS_QUEUE=1'] if queue else []), str(main or a.main), str(MODEL / 'sdk.c'), str(out / 'vectors.c'),
         str(out / 'model_graphs.c'), '-lm', '-o', str(binary)])
    receipt = out / 'receipt.bin'
    if receipt.exists():
        receipt.replace(out / 'receipt.previous')
    done = subprocess.run([str(binary), str(receipt)], capture_output=True, text=True, timeout=600)
    return done.returncode, done.stdout + done.stderr, receipt.read_bytes() if receipt.exists() else b''


def matches(raw, spec):
    words = struct.unpack('<%dI' % (len(raw) // 4), raw)
    assert len(words) == len(spec['words']), (len(words), len(spec['words']))
    wrong = [(i, w, s) for i, (w, s) in enumerate(zip(words, spec['words']))
             if not (w == s if isinstance(s, int) else s['min'] <= w <= s['max'])]
    return wrong


# The queue image: graph-relu under 31 names, the database and artifacts as the board image is built from them.
aliases = a.output / 'aliases'
run([sys.executable, str(APP / 'queue-aliases.py'), '--graph', 'graph-relu', '--count', '31', '--artifacts', str(artifacts_path),
     '--output', str(aliases)])
names = json.loads((aliases / 'list.json').read_text())['npu_models']
status, text, raw = image('queue', names, aliases / 'models.json', APP / 'manifest-queue.json', True)
assert status == 0 and raw, text
wrong = matches(raw, host.queue_expectation(DEPTHS, 1000))
assert not wrong, ('queue receipt differs from the board expectation', wrong[:8])
result = host.decode(raw, queue=True)
rows = result['queue']
assert result['passed'] and rows[-1]['link'] and rows[-1]['status'] == 'match', result
assert all(r['refusals'] == 0 for r in rows[:-1] if r['k'] <= 64) and rows[-2]['k'] == 128 and rows[-2]['refusals'] > 0, rows
print('queue image: 31 slots, receipt equals the board expectation; refusals at k=128:', rows[-2]['refusals'])

# A list shorter than the declared slots: the open is refused.
status, text, raw = image('queue-short', names[:3], aliases / 'models.json', APP / 'manifest-queue.json', True)
assert status == 2 and 'FIRST_FAULT detail=0000b001 value=ffffffff' in text, text
print('queue image with 3 of 31 slots: the open is refused, fault 0xb001')

# graph-add under 31 names fails the link: its output (int16, fixed position 12) is not the representation of its input
# (float16), so the pair does not reproduce the reference. The queue image therefore lists graph-relu, whose input and output
# share one representation and relu(relu(x)) = relu(x).
adds = a.output / 'aliases-add'
run([sys.executable, str(APP / 'queue-aliases.py'), '--graph', 'graph-add', '--count', '31', '--artifacts', str(artifacts_path),
     '--output', str(adds)])
status, text, raw = image('queue-add', json.loads((adds / 'list.json').read_text())['npu_models'], adds / 'models.json',
                          APP / 'manifest-queue.json', True)
assert status == 0 and raw, text
link = host.decode(raw, queue=True)['queue'][-1]
assert link['link'] and link['status'] == 'mismatch', link
print('queue image of graph-add: the link pair mismatches (%d wrong bytes), as expected' % link['wrong_output_bytes'])

# The one-run-each image: graph-add and graph-relu with the default declaration.
plain = a.output / 'plain'
plain.mkdir(exist_ok=True)
(plain / 'models.json').write_text(json.dumps(fixtures))
(plain / 'artifacts.json').write_text(json.dumps(artifacts))
pair = ['graph-add', 'graph-relu']
status, text, raw = image('plain', pair, plain / 'models.json', APP / 'manifest.json', False)
assert status == 0 and raw, text
entries = {m['name']: m for m in fixtures['models']}
sizes = [tuple(sum(t['logical_bytes'] for t in entries[n][f]) for f in ('inputs', 'outputs')) for n in pair]
result = host.decode(raw, pair, sizes)
assert result['passed'] and result['matched'] == 2, result
print('one-run-each image: graph-add and graph-relu match')

# Mutations of main.c, each the defect of the main.c before registration (fa2e0c13 to this series): the queue image opens the
# ring without registering a graph, and the one-run-each image guards on the graphs the OS image links, which are none.
source = a.main.read_text()
for case, queue, listed, database, manifest, old, new, detail in (
        ('no-registration', True, names, aliases / 'models.json', APP / 'manifest-queue.json',
         '    register_graphs();\n    uint64_t begin', '    uint64_t begin', 'b001'),
        ('linked-guard', False, pair, plain / 'models.json', APP / 'manifest.json',
         'if (total == 0 || total > NPU_GRAPHS_MAX)', 'if (npu_graph_count == 0 || total > NPU_GRAPHS_MAX)', 'a100')):
    assert source.count(old) == 1, case
    mutant = a.output / (case + '-main.c')
    mutant.write_text(source.replace(old, new))
    status, text, raw = image(case, listed, database, manifest, queue, main=mutant)
    assert status == 2 and 'FIRST_FAULT detail=0000' + detail in text, (case, text)
    print('mutation %s: fault 0x%s' % (case, detail))
print('PASS npu-graphs application on the host model')
