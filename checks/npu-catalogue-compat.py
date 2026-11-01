#!/usr/bin/env python3
"""Verify historical NPU artifact maps with unchanged file identity checks."""
import argparse
import copy
import importlib.util
import hashlib
import json
from pathlib import Path
import subprocess
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
    parser.add_argument('--configure', action='store_true', help='configure the NPU board image using a copy of the supplied map')
    parser.add_argument('--configure-script', type=Path,
                        default=Path.home() / '.cache/sunstoneos/bin/cfg.sh')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    catalogue = module('catalogue', 'tools/npu-session-catalogue.py')
    embed = module('embed', 'apps/os/npu-catalogue.py')
    sessions = module('sessions', 'checks/npu-session.py')
    board = module('board', 'apps/npu-board/catalogue.py')
    original = args.artifacts.read_bytes()
    copied_map = args.output / 'historical-map.json'
    copied_map.write_bytes(original)
    old = json.loads(original)
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
    board.generate(copied_map, args.output / 'old-board.c')
    board.generate(new_path, args.output / 'new-board.c')
    assert (args.output / 'old-board.c').read_bytes() == (args.output / 'new-board.c').read_bytes()
    for label, path in [('old', copied_map), ('new', new_path)]:
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
            consumers = [('embed', lambda: embed.generate(path, directory / 'bad.c', 4)),
                         ('session', lambda: sessions.fixtures(path, [name], directory))]
            if section == 'model':
                consumers.append(('board', lambda: board.generate(path, directory / 'bad-board.c')))
            for consumer, call in consumers:
                try:
                    call()
                except (ValueError, AssertionError, SystemExit) as error:
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
    command = None
    if args.configure:
        build = args.output / 'npu-board-build'
        command = [str(args.configure_script), str(ROOT), str(build), '-DMIMUS_DIR=',
                   '-DSUNSTONE_A733_OS=ON', '-DSUNSTONE_APPLICATION=',
                   '-DSUNSTONE_A733_NPU_BOARD=ON',
                   '-DSUNSTONE_NPU_BOARD_CATALOGUE=' + str(copied_map.resolve())]
        with (args.output / 'configure.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        assert (build / 'npu-board-application/models.c').read_bytes() == (args.output / 'old-board.c').read_bytes()
    assert args.artifacts.read_bytes() == original, 'source artifact map changed'
    result = dict(result='pass', refused=refused, configure_command=command,
                  artifacts_sha256=hashlib.sha256(original).hexdigest())
    (args.output / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    print('PASS NPU artifact map compatibility: identical old/new output; changed artifacts refused')
    if args.configure:
        print('PASS NPU board configure: copied cached map; source map unchanged')


if __name__ == '__main__':
    main()
