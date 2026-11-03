#!/usr/bin/env python3
"""Check the gate's input registry and its refusal of an affected check it cannot supply.

tools/integration-gate.py runs the checks that need a built image, and must not skip them silently. checks/gate-inputs.json lists
the images the gate builds, the checks it runs on them, the checks that need a board, and the affected checks it may leave
unrun, each waived with a reason. Cases:
  registry   every named check exists and is not pure-host; every image a command names is a gate variant or a recipe; each
             recipe has a configure list, a target and an ELF name; no check is registered and waived at once; every waiver has
             a reason; no waiver names a check that no longer exists or that runs without inputs;
  coverage   every non-pure-host check of the index is registered, waived, or needs a board (so the gate has an answer for it);
  refusal    a check that is affected but in no list is unsupplied, naming the check, and a registered one is supplied; a check
             needing USB access is reported as a board check and does not refuse; removing a registry entry makes its check
             unsupplied (mutation);
  standards  every check that executes an integrated OS image builds its emulated machine with harness.machine() (the maximal core,
             which the images' atomics need), and none creates its own AArch64 Unicorn.
Usage: gate-inputs.py --output DIR
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))      # tools/integration-gate.py imports host_evidence
CHECK_INPUTS = ['tools/integration-gate.py', 'checks/gate-inputs.json', 'checks/harness.py']
# Checks that execute an integrated OS image under Unicorn. Each uses the one machine of checks/harness.py.
IMAGE_EXECUTING = ('fault-capture os-gpu-return os-gpu-quiesce os-gpu-untouched os-gpu-unconstructed os-gpu-wait-linked '
                   'os-image-audio os-image-headset os-image-identity os-image-radio os-region-initialization os-audio-replay '
                   'os-audio-replay-linked os-cpu-return-rails os-cpu-return-thermal os-cpu-clock-native os-npu-start '
                   'latency-startup').split()


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
    gate = load('gate_inputs_gate', ROOT / 'tools/integration-gate.py')
    affected = load('gate_inputs_affected', ROOT / 'checks/affected.py')
    index = affected.indexer.generate()
    inputs = gate.load_gate_inputs(ROOT)
    for name, recipe in inputs['images'].items():
        assert recipe['configure'] and recipe['target'] and recipe['elf'].endswith('.elf') and name not in gate.VARIANTS, name
    for name, record in inputs['checks'].items():
        assert name in index and not index[name]['pure_host'], (name, 'registered check is absent or runs without inputs')
        assert record['runs'], name
        for run in record['runs']:
            for kind, image in gate.placeholders(run['args']):
                assert (image in inputs['bundles'] if kind == 'bundle' else image in gate.VARIANTS or image in inputs['images']), (name, image)
            assert any('{output}' in a for a in run['args']), (name, 'a run needs a new output directory')
    assert not set(inputs['checks']) & set(inputs['waived']), 'a check is registered and waived'
    assert not set(inputs['board']) & (set(inputs['checks']) | set(inputs['waived']))
    for name, reason in inputs['waived'].items():
        assert name in index and not index[name]['pure_host'] and ' | ' in reason, (name, 'stale waiver, or no "category | reason"')
    unanswered = []
    for name, entry in sorted(index.items()):
        if entry['pure_host'] or entry.get('library'):
            continue
        needs = affected.needs(entry)
        if name in inputs['checks'] or name in inputs['waived'] or name in inputs['board'] or entry.get('usb') or set(needs) <= gate.BOARD_NEEDS:
            continue
        unanswered.append(name)
    assert not unanswered, ('checks with no registry answer; register or waive them', unanswered[:10])
    # refusal
    probe = {'check': 'os-failure-return.py', 'needs': ['elf', '--out']}
    assert gate.classify_affected([probe], index, inputs)['supplied'] == ['os-failure-return.py']
    stripped = json.loads(json.dumps(inputs))
    del stripped['checks']['os-failure-return.py']
    result = gate.classify_affected([probe], index, stripped)
    assert result['unsupplied'] == [('os-failure-return.py', ['elf', '--out'])] and not result['supplied'], result
    usb = next(n for n, e in sorted(index.items()) if e.get('usb') and not e['pure_host'])
    result = gate.classify_affected([{'check': usb, 'needs': ['USB access']}], index, stripped)
    assert result['board'] == [usb] and not result['unsupplied'], result
    waived = next(iter(inputs['waived']))
    assert gate.classify_affected([{'check': waived, 'needs': ['x']}], index, inputs)['waived'] == [waived]
    # standards
    for name in IMAGE_EXECUTING:
        text = (ROOT / 'checks' / (name + '.py')).read_text()
        assert not re.search(r'\bUc\(UC_ARCH_ARM64, ?UC_MODE_ARM\)', text), (name, 'builds its own AArch64 machine')
        assert 'harness_machine()' in text or 'harness.machine()' in text, (name, 'does not use the shared machine')
    harness_text = (ROOT / 'checks/harness.py').read_text()
    assert 'ctl_set_cpu_model(UC_CPU_ARM64_MAX)' in harness_text
    by_reason = {}
    for reason in inputs['waived'].values():
        by_reason[reason.split(' | ')[0]] = by_reason.get(reason.split(' | ')[0], 0) + 1
    (out / 'result.json').write_text(json.dumps({'status': 'PASS', 'images': len(inputs['images']), 'registered': len(inputs['checks']),
                                                 'waived': len(inputs['waived']), 'waived_by_reason': by_reason}, indent=1) + '\n')
    for reason, count in sorted(by_reason.items(), key=lambda item: -item[1]):
        print(f'  waived {count:4d}  {reason}')
    print(f"PASS gate inputs: {len(inputs['checks'])} checks registered on {len(inputs['images'])} recipes and the gate variants, "
          f"{len(inputs['waived'])} waived with reasons, every other non-pure-host check needs a board; an unlisted affected check is "
          f"unsupplied; {len(IMAGE_EXECUTING)} image-executing checks use the shared machine")


if __name__ == '__main__':
    main()
