#!/usr/bin/env python3
"""Exercise the native USB execution gate with modeled task admission."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from harness import CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    native = ROOT / 'drivers/usb/a733_usb2_native.c'
    source = native.read_text()
    functions = []
    for name in ('current', 'a733_usb2_native_share', 'a733_usb2_native_enter',
                 'a733_usb2_native_client', 'a733_usb2_native_leave'):
        match = re.search(r'(?:static )?(?:int|void) '+name+r'\([^)]*\)\s*\{', source)
        assert match, name
        end = source.index('{', match.start()) + 1
        depth = 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        functions.append(source[match.start():end])
    (out / 'execution.inc').write_text('\n'.join(functions)+'\n')
    command = [str(CLANG), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               *SANITIZE, '-I'+str(ROOT/'include'), '-I'+str(out),
               str(ROOT/'checks/usb-host-execution.c'), '-o', str(out/'execution')]
    result = subprocess.run(command, capture_output=True, text=True, env=environment())
    (out/'compile.log').write_text(result.stdout+result.stderr)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([out/'execution'], capture_output=True, text=True, env=environment())
    (out/'run.log').write_text(result.stdout+result.stderr)
    assert result.returncode == 0, result.stderr
    record = dict(result='PASS', hardware_access=False, command=command,
                  inputs={str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in [native, ROOT/'include/a733_usb2_native.h',
                                       ROOT/'checks/usb-host-execution.c', Path(__file__)]},
                  scope='Actual native gate functions and structure; task identity, privilege admission and notifications modeled. No syscall, scheduler or physical execution.')
    (out/'result.json').write_text(json.dumps(record, indent=2)+'\n')
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
