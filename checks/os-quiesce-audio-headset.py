#!/usr/bin/env python3
"""Check audio and headset return steps with native owners and modeled replies."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--build', '{build:headset}', '--output', '{output}']]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def function(text, name):
    match = re.search(r'\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', text)
    assert match, name
    begin = text.index('{', match.start())
    depth = 0
    for i in range(begin, len(text)):
        depth += text[i] == '{'
        depth -= text[i] == '}'
        if not depth:
            return text[begin:i + 1]
    raise AssertionError(name)


def source_problem(text, owner):
    start, callback = {'audio': ('os_audio_stream_open', 'audio_quiesce_step'),
                       'headset': ('os_headset_start', 'headset_quiesce_step')}[owner]
    if not re.search(r'os_quiesce_register\(&' + owner + r'_quiesce\)', function(text, start)):
        return 'missing registration'
    if re.search(r'\b(platform_exit|board_fail|fail_close|a733_smp_exit_privileged)\s*\(', function(text, callback)):
        return 'stop in the quiesce callback'
    if 'os_quiesce_fault' not in function(text, 'fail'):
        return 'failure closes admission'
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--owner', choices=('audio', 'headset'), action='append')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    temporary = args.output / 'tmp'
    temporary.mkdir(exist_ok=True)
    os.environ['TMPDIR'] = str(temporary.resolve())
    commands = json.loads((args.build / 'compile_commands.json').read_text())
    model = load('audio_model', ROOT / 'checks/os-image-audio.py')
    lld = Path.home() / '.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'
    owners = args.owner or ('audio', 'headset')
    records = {}
    for owner in owners:
        text = (ROOT / ('apps/os/' + owner + '.c')).read_text()
        assert source_problem(text, owner) is None
        no_registration = text.replace('os_quiesce_register(&' + owner + '_quiesce)', '1')
        assert source_problem(no_registration, owner) == 'missing registration'
        callback = function(text, owner + '_quiesce_step')
        stopped = text.replace(callback, callback.replace('{', '{ platform_exit(6);', 1), 1)
        assert source_problem(stopped, owner) == 'stop in the quiesce callback'
        unit = next(u for u in commands if u['file'].endswith('/apps/os/' + owner + '.c'))
        compiled_root = str(Path(unit['file']).resolve().parents[2])
        flags = [flag.replace(compiled_root, str(ROOT)) for flag in shlex.split(unit['command'])]
        flags = flags[:flags.index('-o')]
        sources = (('checks/os-image-audio.c', 'apps/os/capture.c', 'drivers/audio/a733_i2s.c') if owner == 'audio' else
                   ('checks/os-image-headset.c', 'apps/bluetooth/pcm.c', 'drivers/storage/storage_digest.c'))
        command = flags + [str(ROOT / p) for p in (*sources, 'runtime/memory.c', 'runtime/memmove.c')] + [
            '-nostdlib', '--ld-path=' + str(lld), '-Wl,-Ttext=0x41000000', '-Wl,-e,os_check_run', '-Wl,--gc-sections']
        if owner == 'audio':
            command += ['-Wl,--wrap=os_audio_stream_open']
            runs = [(mode, case, frames) for mode, case in enumerate(model.CASES)
                    for frames in (model.LENGTHS if mode == 0 else [16000])]
        else:
            headset = load('headset_model', ROOT / 'checks/os-image-headset.py')
            runs = [(mode, case, 16000) for mode, case in headset.RUNS]
        segments, symbols = model.image(command, args.output, owner + '-return')
        cases = {}
        for mode, case, frames in runs:
            result, _ = model.run(segments, symbols, mode, frames)
            cases[case + '-' + str(frames)] = result
            assert result == 1, (owner, mode, frames, result)
        records[owner] = {'cases': cases, 'mutants_rejected': 2,
                          'source_sha256': hashlib.sha256(text.encode()).hexdigest(), 'command': command}
    record = {'passed': True, 'physical_access': False, 'owners': records}
    (args.output / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS return owners: ' + ', '.join(owners) + '; delayed completion, retained faults, capture regressions; '
          + str(2 * len(owners)) + ' mutants rejected')


if __name__ == '__main__':
    main()
