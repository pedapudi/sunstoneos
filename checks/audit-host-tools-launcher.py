#!/usr/bin/env python3
"""Check that every adapted supervised launcher refuses a changed shared anchor.

Each application's build_launcher reads platform/a733-smp-supervised/launch.c.
Here that read returns a mutated copy; generation must stop before compiling.
No compiler, board or USB device runs.
"""
import importlib.util
from pathlib import Path
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SHARED = ROOT / 'platform/a733-smp-supervised/launch.c'
APPS = ('adc', 'bluetooth', 'board', 'crypto', 'jpeg', 'jpeg-encode', 'h264', 'h264-encode', 'hevc', 'monitor',
        'pcie', 'pcie-test', 'pwm', 'rtc', 'spi', 'storage-read', 'thermal', 'uart')
MUTATIONS = {
    'reformatted deadline': ('start+20*A733_SMP_COUNTER_HZ;\n    CONTRACT->expires_at',
                             'start+20 * A733_SMP_COUNTER_HZ;\n    CONTRACT->expires_at'),
    'respelled request magic': ('UINT64_C(0x4153373353554c33)', 'UINT64_C(0X4153373353554C33)'),
    'rewritten session predicate': ('CONTRACT->reserved[0] && !CONTRACT->reserved[1]',
                                    'CONTRACT->reserved[0] && CONTRACT->reserved[1]==0'),
    'duplicated version guard': ('REQUEST[1]==3 &&', 'REQUEST[1]==3 && REQUEST[1]==3 &&'),
}


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def main():
    original = SHARED.read_text()
    board = module('audit_launcher_board', ROOT / 'apps/board/prepare.py')
    # The helper applies edits in order and enforces each expected count.
    assert board.adapt_launcher('a b a', {'a': 'c', 'c b': 'd'}, {'a': 2}) == 'd c'
    for text, changes, counts in [('a a', {'a': 'b'}, {}), ('a', {'z': 'b'}, {}), ('a a a', {'a': 'b'}, {'a': 2})]:
        try:
            board.adapt_launcher(text, changes, counts)
        except ValueError as error:
            assert 'anchor changed' in str(error)
        else:
            raise AssertionError((text, changes))
    assert original.count('20*A733_SMP_COUNTER_HZ') == 3
    read_text = Path.read_text
    results = {}
    with tempfile.TemporaryDirectory(prefix='audit-host-tools-launcher-') as directory:
        for app in APPS:
            prepare = module('audit_launcher_' + app.replace('-', '_'), ROOT / 'apps' / app / 'prepare.py')
            for label, (old, new) in MUTATIONS.items():
                assert original.count(old) == 1, (label, old)
                mutated = original.replace(old, new)

                def read(path, *args, **kwargs):
                    return mutated if Path(path) == SHARED else read_text(path, *args, **kwargs)
                output = Path(directory) / app / label.replace(' ', '-')
                output.mkdir(parents=True)
                with patch.object(Path, 'read_text', read), \
                        patch('subprocess.run', side_effect=AssertionError('Compiler reached after refusal.')):
                    try:
                        prepare.build_launcher(output, 'clang-21', Path('/nonexistent/ld.lld'))
                    except ValueError as error:
                        assert 'Shared supervised launcher anchor changed' in str(error), (app, label, error)
                    else:
                        raise AssertionError(f'{app} accepted a {label}.')
                assert not (output / 'launch.bin').exists()
            results[app] = len(MUTATIONS)
    print(f'PASS: {len(results)} launcher generators refuse {len(MUTATIONS)} shared-anchor changes '
          'before compilation; no compiler or device runs.')


if __name__ == '__main__':
    main()
