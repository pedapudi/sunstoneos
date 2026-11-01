#!/usr/bin/env python3
"""Exercise the actual legacy HID mailbox under sequential and concurrent reads."""
import argparse
from pathlib import Path
import re
import subprocess
from harness import CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'(?:static void|int) ' + name + r'\([^)]*\)\s*\{', source)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--thread-sanitizer', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = (ROOT / 'apps/os/client.c').read_text()
    begin = source.index('/* ready: empty, published, reader copying, owner publishing. */')
    end = source.index('int ss_storage_read(', begin)
    storage = re.search(r'^static struct \{[^\n]+\} storage;$', source, re.M)
    assert storage, 'Actual storage mailbox declaration missing'
    mailbox = storage.group(0) + '\n' + source[begin:end]
    assert 'keyboard_publish();' in function(source, 'serve'), 'Owner must retry pending publication'
    (out / 'usb-keyboard-client.inc').write_text(mailbox + function(source, 'ss_storage_read') + '\n' +
        function(source, 'ss_key_report') + '\n' + function(source, 'usb_input') + '\n' +
        function(source, 'serve') + '\n')
    flags = ['-fsanitize=thread', '-fno-sanitize-recover=all'] if args.thread_sanitizer else SANITIZE
    command = [str(CLANG), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread', *flags,
               '-DSUNSTONE_A733_OS_USB_CLASSES=1', '-I' + str(ROOT / 'include'), '-I' + str(out),
               str(ROOT / 'checks/usb-keyboard-client.c'), '-o', str(out / 'keyboard')]
    for label, cmd in [('compile', command), ('run', [str(out / 'keyboard')])]:
        result = subprocess.run(cmd, text=True, capture_output=True, env=environment())
        (out / (label + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, (label, result.stdout, result.stderr)
    print((out / 'run.log').read_text().strip())


if __name__ == '__main__':
    main()
