#!/usr/bin/env python3
"""Exercise the actual legacy HID mailbox under sequential and concurrent reads."""
import argparse
from pathlib import Path
import host_rtos

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    defines = ['SUNSTONE_A733_OS_USB_CLASSES=1']
    includes = [ROOT / 'apps/os', ROOT / 'apps/board']
    def build(name, client=None, quiet=False):
        defs = defines + (['KEYBOARD_CLIENT_SOURCE="%s"' % client] if client else [])
        return host_rtos.build(out, ['checks/usb-keyboard-client.c'], name, defines=defs, includes=includes,
                               flags=['-pthread'] + (['-w'] if quiet else []))
    exe = build('keyboard')
    result = host_rtos.run(exe)
    (out / 'run.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0, ('run', result.stdout, result.stderr)
    # Negative control: the owner publishing over a claimed report must fail the check.
    mutant = host_rtos.mutated(out, 'apps/os/client.c', 'if(state>1||(state==1&&!keyboard_pending.invalidate)||',
                               'if((state==1&&!keyboard_pending.invalidate)||', 'publish-over-reader.c')
    control = host_rtos.run(build('keyboard-control', mutant, True))
    assert control.returncode != 0, 'control passed: publication over a claimed report'
    print((out / 'run.log').read_text().strip())


if __name__ == '__main__':
    main()
