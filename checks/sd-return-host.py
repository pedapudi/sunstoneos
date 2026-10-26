#!/usr/bin/env python3
"""Bind the new return decoder to an actual linked producer log, without USB."""
import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'checks'))
import harness


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=False)
    tool = load('sd_return_host', ROOT / 'tools/storage-read-returning.py')
    raw = (a.native / 'nominal.input.bin').read_bytes(); log = (a.native / 'nominal.log').read_text()
    value = tool.decode_log(log, raw, 0); assert value['complete'] and value['words']['SD_RESTORED'] == 1
    refusals = []
    for name, text, reason in (
        ('missing receipt', log.replace('SD_RESTORED=0x0000000000000001\n', ''), 'missing SD restoration receipt'),
        ('duplicate receipt', log+'SD_RESTORED=1\n', 'duplicate result word'),
        ('false receipt', log.replace('SD_RESTORED=0x0000000000000001', 'SD_RESTORED=0'), 'missing SD restoration receipt'),
        ('unknown version', log.replace('SD_RETURN_VERSION=0x0000000000000001', 'SD_RETURN_VERSION=2'), 'unsupported return version')):
        refusals.append(dict(name=name, rejection=harness.refused(lambda: tool.decode_log(text, raw, 0), reason)))
    assert not tool.decode_log(log, raw, 1)['complete']
    refusals.append(dict(name='missing status', rejection=harness.refused(
        lambda: tool.decode_log(log, raw), 'missing successful return status')))
    # Earlier frozen capture format remains decodable, without granting it the
    # restoration receipt of this new image.
    legacy = '\n'.join(x for x in log.splitlines() if not x.startswith(('SD_RETURN_VERSION=', 'SD_RESTORED=')))+'\n'
    assert tool.decode_log(legacy, raw, 0)['complete']
    outcomes = json.loads((ROOT / 'apps/storage-read-returning/controlled.json').read_text())['outcomes']
    assert {o['name'] for o in outcomes} == {'sd-input', 'sd-providers', 'sd-return-admission'}
    assert all(o['status'] == 1 for o in outcomes)
    # Exercise actual argparse/main default with explicitly modeled host
    # lifetime authority; this fixture does not sign a physical approval.
    config = {'pmic_pins': [[2,1,1]]*2, 'expected_pins': [[0,0,0]]*7,
              'desired_pins': [[2,1,1]]*6+[[0,1,1]]}
    cfg = a.output / 'config.json'; cfg.write_text(json.dumps(config))
    tool.lifetime = lambda *args: None
    selected = []
    tool.approved = lambda *args: selected.append(args[-1])
    tool.fabric_read = lambda *args: (([0,1<<31,1<<31], [24000000]*3), [0,1<<31,1<<31])
    tool.digest = lambda path: 'model'
    argv = ['input']
    for name in ('approval','request','ddr','survey','fabric-read'): argv += ['--'+name, str(a.output / name)]
    argv += ['--config', str(cfg), '--output', str(a.output / 'default.input.bin')]
    with contextlib.redirect_stdout(io.StringIO()): assert tool.main(argv) == 0
    assert tool.parse_input((a.output / 'default.input.bin').read_bytes())['profile'] == 2 and selected == [tool.BUNDLES[2]]
    (a.output / 'result.json').write_text(json.dumps(dict(result='pass', physical=False,
        native=str(a.native), refusals=refusals, default_profile=2, controlled_before_tasks_only=True), indent=2)+'\n')


if __name__ == '__main__': main()
