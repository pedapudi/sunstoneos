#!/usr/bin/env python3
"""Refuse stale grant, plan and launch-bundle destinations without redirection."""
import argparse
import importlib.util
import json
from pathlib import Path
import sys
from types import SimpleNamespace
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
    memory = module('artifact_memory', 'apps/memtest/check.py')
    refuses('memory-test', lambda destination: memory.prepare(SimpleNamespace(output=destination)))
    fixture = module('artifact_h264_fixture', 'apps/h264/fixture.py')
    stream = module('artifact_h264_stream', 'apps/h264/stream.py')
    high = module('artifact_h264_high', 'apps/h264/high.py')
    clip = args.output / 'input.h264'
    clip.write_bytes(fixture.construct()[0])
    refuses('h264-stream', lambda destination: stream.prepare_inputs(clip, destination))
    refuses('h264-high', lambda destination: high.prepare_inputs(clip, destination, None, None))
    def fixture_prepare(destination):
        with patch.object(sys, 'argv', ['fixture.py', str(destination), '--ffmpeg', 'unused-ffmpeg']):
            fixture.main()
    refuses('h264-fixture', fixture_prepare)
    returning = module('artifact_storage_read_input', 'tools/storage-read-returning.py')
    returning_input = args.output / 'returning-input.bin'
    returning_input.write_bytes(b'version-one-input')
    try:
        returning.main(['input', '--config', str(args.output / 'unused-config.json'),
                        '--output', str(returning_input)])
    except FileExistsError:
        assert returning_input.read_bytes() == b'version-one-input'
        cases['returning-input'] = {'passed': True}
    else:
        raise AssertionError('Returning read input reused an existing artifact filename.')
    (args.output / 'results.json').write_text(json.dumps(cases, indent=2) + '\n')
    assert all(row['passed'] for row in cases.values()), cases
    print('PASS: eleven artifact producers refuse stale outputs and preserve version one without redirection.')


if __name__ == '__main__':
    main()
