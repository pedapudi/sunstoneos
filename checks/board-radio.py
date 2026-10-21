#!/usr/bin/env python3
"""Check the radio coordinator and changed session admission without protocol replay."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['output', 'freertos', 'tcp', 'headers']:
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--listener-only',action='store_true',help='Only the new listener profile and client sequence')
    p.add_argument('--cc', default='/usr/bin/clang-21')
    p.add_argument('--reuse-digest', type=Path,
                   help='Reuse only an unchanged preserved digest object from a stopped batch')
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    commands = []
    artifacts = []

    def preserve():
        dependencies = {Path(__file__).resolve(), ROOT / 'docs/board-radio.md'}
        dependencies.update(ROOT / path for path in [
            'apps/board/radio.c', 'apps/board/radio.h', 'apps/board/identity.h',
            'checks/board-radio.c', 'checks/board-radio-session.c','checks/board-radio-listener.c','apps/board/radio_profile.c',
            'drivers/net/a733_radio_session.c', 'include/a733_radio_session.h',
            'drivers/net/aic8800_sdio_transport.c', 'include/aic8800_sdio_transport.h',
            'drivers/net/a733_radio_entropy.c', 'include/a733_radio_entropy.h',
            'drivers/net/a733_radio_platform.c', 'include/a733_radio_platform.h',
            'drivers/storage/storage_digest.c'])
        for dep in out.glob('*.d'):
            for name in shlex.split(dep.read_text().replace('\\\n', ' ').split(':', 1)[1]):
                path = Path(name)
                dependencies.add(path.resolve() if path.is_absolute() else (ROOT / path).resolve())
        manifest = {}
        for source in sorted(dependencies):
            identity = digest(source)
            target = out / 'inputs' / identity / source.name
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                shutil.copyfile(source, target)
            manifest[str(source)] = {'sha256': identity, 'snapshot': str(target.relative_to(out))}
        (out / 'inputs.json').write_text(json.dumps(manifest, indent=2) + '\n')
        (out / 'artifacts.json').write_text(json.dumps({str(f.relative_to(out)): digest(f) for f in artifacts}, indent=2) + '\n')
        (out / 'compiler.json').write_text(json.dumps({'path': a.cc, 'sha256': digest(Path(a.cc).resolve())}, indent=2) + '\n')

    def run(name, command):
        commands.append(command)
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                env={**os.environ, 'ASAN_OPTIONS': 'halt_on_error=1'})
        (out / (name + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(name + ': see preserved log')
        return result.stdout

    try:
        common = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                  '-DSUNSTONE_AIC8800_RSN=1', *(['-DSUNSTONE_BOARD_SUPERVISED=1'] if a.listener_only else []), '-DINCLUDE_xSemaphoreGetMutexHolder=1',
                  '-ffunction-sections', '-fdata-sections',
                  '-Inetwork/include', '-Iinclude', '-I' + str(a.freertos / 'include'),
                  '-I' + str(a.freertos / 'portable/GCC/ARM_AARCH64_SRE'),
                  '-I' + str(a.tcp / 'source/include'),
                  '-I' + str(a.tcp / 'source/portable/Compiler/GCC')]
        digest_object = out / 'host-digest.o'
        if a.reuse_digest:
            prior = a.reuse_digest.resolve()
            prior_inputs = json.loads((prior / 'inputs.json').read_text())
            prior_artifacts = json.loads((prior / 'artifacts.json').read_text())
            assert digest(prior / 'host-digest.o') == prior_artifacts['host-digest.o']
            assert digest(Path(a.cc).resolve()) == json.loads((prior / 'compiler.json').read_text())['sha256']
            for name in shlex.split((prior / 'host-digest.d').read_text().replace('\\\n', ' ').split(':', 1)[1]):
                source = Path(name).resolve() if Path(name).is_absolute() else (ROOT / name).resolve()
                assert digest(source) == prior_inputs[str(source)]['sha256']
            shutil.copyfile(prior / 'host-digest.o', digest_object)
            shutil.copyfile(prior / 'host-digest.d', out / 'host-digest.d')
            (out / 'reuse.json').write_text(json.dumps({'digest_object': str(prior / 'host-digest.o'),
                'sha256': digest(digest_object), 'scope': 'Unchanged successful digest compilation only.'}, indent=2) + '\n')
        else:
            run('host-digest', [a.cc, *common, '-fsanitize=address,undefined', '-MMD', '-MF',
                str(out / 'host-digest.d'), '-c', 'drivers/storage/storage_digest.c', '-o', str(digest_object)])
        artifacts.append(digest_object)
        for name in (['board-radio-listener'] if a.listener_only else ['board-radio', 'board-radio-session']):
            artifact = out / name
            obj = out / (name + '.o')
            run(name + '-build', [a.cc, *common, '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-MMD', '-MF', str(out / (name + '.d')),
                '-c', 'checks/' + name + '.c', '-o', str(obj)])
            artifacts.append(obj)
            run(name + '-link', [a.cc, '-fsanitize=address,undefined', '-Wl,--gc-sections',
                str(obj), str(digest_object), '-o', str(artifact)])
            artifacts.append(artifact)
        for index, source in enumerate([] if a.listener_only else ['apps/board/radio.c', 'drivers/net/a733_radio_session.c',
                                        'drivers/net/aic8800_sdio_transport.c',
                                        'drivers/net/a733_radio_entropy.c','drivers/net/a733_radio_platform.c']):
            artifact = out / ('native-' + str(index) + '.o')
            run('native-' + str(index), [a.cc, '--target=aarch64-none-elf', '-mcpu=cortex-a55',
                '-ffreestanding', '-fno-builtin', '-mgeneral-regs-only', *common,
                '-isystem', str(a.headers), '-include', 'stdint.h', '-MMD', '-MF',
                str(out / ('native-' + str(index) + '.d')), '-c', source, '-o', str(artifact)])
            artifacts.append(artifact)
        preserve()
        for name in (['board-radio-listener'] if a.listener_only else ['board-radio', 'board-radio-session']):
            for variant in (range(5) if a.listener_only else (None,)):
                assert 'PASS board radio' in run(name+('-'+str(variant) if variant is not None else ''), [str(out / name)]+([str(variant)] if variant is not None else []))
        (out / 'result.json').write_text(json.dumps({'passed': True,
            'scope': ('Actual new listener profile conversion/client with modeled socket replies; nominal, repeated exchange timeouts and peer/late/address failures, both closes' if a.listener_only else 'Actual coordinator/profile admission/client and session busy admission/retention with modeled lower protocol observations; five actual native source compilations.'),
            'limits': 'No physical SID, CE quality, RF, firmware transfer, WPA2 or IPv4 hardware qualification.'}, indent=2) + '\n')
    finally:
        preserve()
        (out / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')


if __name__ == '__main__':
    main()
