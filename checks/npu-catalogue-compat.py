#!/usr/bin/env python3
"""Verify historical NPU artifact maps with unchanged file identity checks."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    return tool


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    catalogue = module('catalogue', 'tools/npu-session-catalogue.py')
    embed = module('embed', 'apps/os/npu-catalogue.py')
    sessions = module('sessions', 'checks/npu-session.py')
    old = json.loads(args.artifacts.read_text())
    # Application example: this historical key identifies the pinned linear layer.
    legacy = 'mimus-linear-q'
    name = 'linear-i16-per-row'
    assert legacy in old and name not in old, 'supply the historical artifact map'
    selected = embed.models()[4]
    assert selected['name'] == name
    assert catalogue.artifact_files(old, name) is old[legacy]
    new = copy.deepcopy(old)
    new[name] = new.pop(legacy)
    new_path = args.output / 'current-map.json'
    new_path.write_text(json.dumps(new))
    embed.generate(args.artifacts, args.output / 'old.c', 4)
    embed.generate(new_path, args.output / 'new.c', 4)
    assert (args.output / 'old.c').read_bytes() == (args.output / 'new.c').read_bytes()
    for label, path in [('old', args.artifacts), ('new', new_path)]:
        directory = args.output / label
        directory.mkdir()
        sessions.fixtures(path, sessions.SESSION_NAMES['reserved'], directory)
    assert (args.output / 'old/npu_session_fixtures.h').read_bytes() == (args.output / 'new/npu_session_fixtures.h').read_bytes()
    refused = []
    for section in ('model', 'inputs', 'references'):
        source = old[legacy][section] if section == 'model' else old[legacy][section][0]
        data = bytearray(Path(source).read_bytes())
        data[0] ^= 1
        bad = args.output / (section + '-changed.bin')
        bad.write_bytes(data)
        changed = copy.deepcopy(old)
        if section == 'model':
            changed[legacy][section] = str(bad)
        else:
            changed[legacy][section][0] = str(bad)
        # A present current entry takes precedence over a valid historical one.
        current = copy.deepcopy(old)
        current[name] = changed[legacy]
        for label, paths in [('historical', changed), ('precedence', current)]:
            stem = section + '-' + label
            path = args.output / (stem + '.json')
            path.write_text(json.dumps(paths))
            directory = args.output / stem
            directory.mkdir()
            for consumer, call in [('embed', lambda: embed.generate(path, directory / 'bad.c', 4)),
                                   ('session', lambda: sessions.fixtures(path, [name], directory))]:
                try:
                    call()
                except (ValueError, AssertionError) as error:
                    assert 'artifact differs' in str(error) or 'does not match the catalogue' in str(error)
                    refused.append(stem + '-' + consumer)
                else:
                    raise AssertionError('changed artifact accepted: ' + stem + '-' + consumer)
    try:
        catalogue.artifact_files(old, 'unknown-model')
    except KeyError:
        pass
    else:
        raise AssertionError('unknown model accepted')
    (args.output / 'result.json').write_text(json.dumps(dict(result='pass', refused=refused), indent=1) + '\n')
    print('PASS NPU artifact map compatibility: identical old/new output; changed model, input and reference refused by both consumers')


if __name__ == '__main__':
    main()
