#!/usr/bin/env python3
"""Refuse stale grant, plan and launch-bundle destinations without redirection."""
import argparse
import importlib.util
import json
from pathlib import Path
from unittest.mock import patch

import harness

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True)
    cases = {}
    storage = module('artifact_storage_update', 'tools/storage-update.py')
    slots = module('artifact_boot_slots', 'tools/boot-slots.py')

    def refuses(name, action):
        destination = args.output / name
        destination.mkdir(mode=0o700)
        old = destination / 'grant.bin'
        old.write_bytes(b'version one: granted card range and image')
        before = {p.name: p.read_bytes() for p in destination.iterdir()}
        try:
            action(destination)
        except FileExistsError:
            assert {p.name: p.read_bytes() for p in destination.iterdir()} == before
            cases[name] = {'passed': True}
        except Exception as error:
            cases[name] = {'passed': False, 'failure': str(error)}
        else:
            cases[name] = {'passed': False, 'failure': 'Existing artifact destination was accepted.'}

    def grant(destination):
        with storage.ProtectedDirectory(destination, create=True):
            raise AssertionError('A second grant can be prepared below the stale grant.')

    refuses('storage-grant', grant)
    refuses('slot-plan', slots.new_directory)
    for name in ('smp', 'device-launch', 'smp-observe', 'smp-supervised'):
        host = module('artifact_' + name.replace('-', '_'), 'tools/check-a733-' + name + '.py')
        def prepare(destination, host=host, name=name):
            arguments = [args.output / 'version-two.elf', None]
            if name == 'smp-supervised':
                arguments.append(None)
            arguments += [destination, 'clang-21', harness.LLD]
            if name == 'device-launch':
                arguments.append(None)
            with patch.object(host, 'image_parts', return_value=([], 0x41000000, bytes([2]) * 32)), \
                    patch.object(host.subprocess, 'run', side_effect=AssertionError('Compiler reached after stale output.')):
                host.prepare(*arguments)
        refuses(name, prepare)
    (args.output / 'results.json').write_text(json.dumps(cases, indent=2) + '\n')
    assert all(row['passed'] for row in cases.values()), cases
    print('PASS: six artifact producers refuse stale outputs and preserve version one without children.')


if __name__ == '__main__':
    main()
