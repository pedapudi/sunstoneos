#!/usr/bin/env python3
"""Check the host ABI against runtime.h and the actual native model report."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('qualification_host', HERE / 'host.py')
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--native-report', type=Path)
    parser.add_argument('--epoch', type=lambda x: int(x, 0))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = args.output / 'abi.c'
    source.write_text('''#include "runtime.h"
#include <stdio.h>
int main(void) {
 printf("%llu %zu %zu %zu %zu %zu %zu %zu\\n", (unsigned long long)QUAL_MAGIC,
 sizeof(struct qualification_report), offsetof(struct qualification_report,cpu),
 sizeof(struct qualification_core), offsetof(struct qualification_core,off_observed),
 offsetof(struct qualification_core,drained_epoch),
 offsetof(struct qualification_core,idle_completed), offsetof(struct qualification_core,idle_wakes));
 return 0;
}
''')
    executable = args.output / 'abi'
    subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(HERE), str(source), '-o', str(executable)], check=True)
    actual = [int(x) for x in subprocess.check_output([str(executable)], text=True).split()]
    host.require(actual == [host.MAGIC, host.REPORT_BYTES, host.CPU_OFFSET, host.CPU_BYTES, 768, 104, 112, 120], 'Host layout disagrees with actual runtime.h.')
    g = host.manifest_check(args.image, json.loads(args.manifest.read_text()))
    passed = ['native-header-layout', 'linked-report-stacks-and-fault-reservations']

    def refused(name, function):
        try:
            function()
        except (ValueError, KeyError):
            passed.append(name)
        else:
            raise AssertionError('Accepted ' + name)

    # Exercise the actual preparation entry, before compilation or file output.
    admission = args.output / 'missing-admission.json'
    admission.write_text('{}\n')
    refused('missing-physical-admission', lambda: host.prepare(args.image, args.manifest, admission,
            args.output / 'must-not-exist', 'unused', 'unused', 'unused'))
    host.require(not (args.output / 'must-not-exist').exists(), 'Refused admission created a bundle.')
    refused('empty-report', lambda: host.decode_report(b'', 1))
    if args.native_report is not None:
        host.require(args.epoch is not None, 'Native report epoch required.')
        raw = args.native_report.read_bytes()
        host.decode_report(raw, args.epoch)
        passed.append('actual-native-model-report')
        refused('wrong-expected-epoch', lambda: host.decode_report(raw, args.epoch + 1))
        mutations = [('header-magic', 0, 0), ('old-report-version', 8, 2), ('incomplete-state', 40, 3), ('global-error', 48, 1),
                     ('cpu0-ticks', host.CPU_OFFSET + 96, 31)]
        for core in range(8):
            offset = host.CPU_OFFSET + core * host.CPU_BYTES
            if core:
                mutations.extend([(f'cpu{core}-missing-timer', offset + 96, 0),
                                  (f'cpu{core}-duplicate-timer', offset + 96, 2)])
            for name, field, value in [('epoch', 0, 0), ('captured', 8, 0), ('cleaned', 16, 0),
                                       ('restored', 24, 0), ('drained', 104, 0), ('error', 48, 1), ('rounds', 64, 31),
                                       ('idle-completed', 112, 0), ('idle-wakes', 120, 0), ('background', 72, 0), ('sgis', 80, 0), ('failure', 88, 1)]:
                mutations.append((f'cpu{core}-{name}', offset + field, value))
            for name, field in [('request', 32), ('acknowledged', 40), ('off-observed', 768)]:
                mutations.append((f'cpu{core}-{name}', offset + field, 0 if core else args.epoch))
        for name, offset, value in mutations:
            changed = bytearray(raw)
            struct.pack_into('<Q', changed, offset, value)
            refused(name, lambda changed=changed: host.decode_report(changed, args.epoch))
        # The two idle-phase reasons are named distinctly.
        for code, text in ((8, 'idle/wake proof missing'), (9, 'drained barrier timed out')):
            changed = bytearray(raw)
            struct.pack_into('<Q', changed, 48, code)
            struct.pack_into('<Q', changed, host.CPU_OFFSET + 3 * host.CPU_BYTES + 48, code)
            try:
                host.decode_report(changed, args.epoch)
            except ValueError as error:
                host.require(f'reason {code} ({text}' in str(error), f'Reason {code} decoded as: {error}')
            else:
                raise AssertionError(f'Reason {code} accepted')
            passed.append(f'reason-{code}-decoded')
        # A report of another layout or session is refused before its error word is decoded.
        for name, offset, value in [('magic', 0, 0), ('version', 8, 2), ('size', 16, host.REPORT_BYTES - 8),
                                    ('epoch', 24, args.epoch + 1)]:
            changed = bytearray(raw)
            struct.pack_into('<Q', changed, offset, value)
            struct.pack_into('<Q', changed, 48, 9)
            try:
                host.decode_report(changed, args.epoch)
            except ValueError as error:
                host.require(str(error) == 'No native report of this session.', f'Foreign {name} decoded as: {error}')
            else:
                raise AssertionError(f'Foreign {name} accepted')
            passed.append(f'foreign-{name}-identity-before-reason')
        refused('truncated-report', lambda: host.decode_report(raw[:-1], args.epoch))
        refused('oversized-report', lambda: host.decode_report(raw + b'\0', args.epoch))
    host.save_json(args.output / 'result.json', {'passed': passed, 'geometry': g,
        'image_sha256': host.digest(args.image), 'native_report_sha256': host.digest(args.native_report) if args.native_report else None,
        'source_hashes': {str(p): host.digest(p) for p in [HERE / 'host.py', HERE / 'host-check.py',
            HERE / 'runtime.h', Path(host.supervised.__file__)]},
        'scope': 'offline ABI and report validation; native report inputs model hardware and do not grant physical authority'})
    print(f'{len(passed)} focused host checks passed')


if __name__ == '__main__':
    main()
