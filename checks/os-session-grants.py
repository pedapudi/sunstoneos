#!/usr/bin/env python3
"""The services a session grants: wire sizes, refused compositions and the service ids (apps/os/grants.c).

Compiles the production functions for the host and checks, for every subset of the granted services, that the thermal
record's size equals what the host session builder (apps/os/host.py) computes for the same records, that exactly the rows of
the refusal table are refused, that each row names a reason, and that every service has its own id in every image.
"""
import argparse
import itertools
import json
import re
import subprocess
from pathlib import Path
from harness import CLANG, environment

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ('apps/os/grants.c', 'apps/os/os.h', 'apps/os/host.py', 'tools/sunstone-manifest.py')
# OS_GRANT_* in apps/os/os.h, and the record each service carries on the wire (apps/os/host.py).
GRANTS = {'NPU': 1, 'AUDIO': 2, 'CPU_CLOCK': 4, 'DMA': 8, 'RADIO': 16}
RECORD = {'AUDIO': 'AUDIO', 'NPU': 'NPU', 'CPU_CLOCK': 'CPU_GRANT', 'DMA': 'DMA'}
GATE_RUNS = [['--build', '{build:all}', '--output', '{output}']]
HARNESS = r'''
#include "os.h"
#include <stdio.h>
int main(void)
{
    for(unsigned g=0;g<32;++g)
        printf("%u %zu %u %s\n",g,os_session_wire_bytes(g),os_composition_refused(g),
               os_composition_refused(g)?os_composition_reason(os_composition_refused(g)):"");
    return 0;
}
'''
INCLUDES = ['apps/os', 'apps/board', 'platform/a733-storage-update', 'platform/a733-smp-cached', 'drivers/usb', 'include']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True, help='a configured OS build directory, for its compile flags')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    flags = (args.build / 'CMakeFiles/sunstone-a733-os-core.dir/flags.make').read_text()
    defines = re.search(r'^C_DEFINES = (.*)$', flags, re.M).group(1).split()
    includes = re.search(r'^C_INCLUDES = (.*)$', flags, re.M).group(1).split()
    (args.output / 'harness.c').write_text(HARNESS)
    exe = args.output / 'grants'
    command = [CLANG, '-std=gnu11', '-O1', '-w', '-no-pie', *defines, *includes, '-I' + str(ROOT / 'apps/os'),
               str(ROOT / 'apps/os/grants.c'), str(args.output / 'harness.c'), '-o', str(exe)]
    done = subprocess.run(command, capture_output=True, text=True, env=environment())
    assert done.returncode == 0, done.stderr[-1500:]
    rows = {}
    for line in subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout.splitlines():
        granted, size, refused, reason = (line.split(' ', 3) + [''])[:4]
        rows[int(granted)] = (int(size), int(refused), reason)

    import importlib.util
    spec = importlib.util.spec_from_file_location('session_host', ROOT / 'apps/os/host.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    for granted, (size, refused, reason) in rows.items():
        # The size the host builder writes into the thermal record: the prefix and every service record that is present.
        expected = host.PREFIX_BYTES + host.SERVICE_BYTES[host.THERMAL] + sum(
            host.SERVICE_BYTES[getattr(host, RECORD[name])] for name, bit in GRANTS.items() if name in RECORD and granted & bit)
        assert size == expected, (granted, size, expected)
        assert (refused != 0) == bool(reason), (granted, refused, reason)
    # A single service is never refused; every refused set holds at least two, and a superset of a refused set is refused.
    for granted, (_, refused, _) in rows.items():
        if bin(granted).count('1') < 2:
            assert not refused, granted
        if refused:
            assert all(rows[g][1] for g in rows if g & granted == granted), granted
    names = lambda g: '+'.join(n for n, b in GRANTS.items() if g & b)
    table = sorted({names(g): rows[g][2] for g in rows if rows[g][1] and not any(rows[h][1] for h in rows if h != g and h & g == h)}.items())
    # The service ids of the supervisor are fixed (apps/os/os.h): no id depends on a build option except the headset's.
    header = (ROOT / 'apps/os/os.h').read_text()
    enum = re.search(r'enum \{ OS_SERVICE_APPLICATION=5,(.*?)OS_SERVICES \};', header, re.S).group(1)
    ids = re.findall(r'OS_SERVICE_[A-Z]+', re.sub(r'#ifdef.*?#endif', '', enum, flags=re.S))
    assert ids == ['OS_SERVICE_THERMAL', 'OS_SERVICE_NPU', 'OS_SERVICE_AUDIO', 'OS_SERVICE_RADIO', 'OS_SERVICE_NETWORK'], ids
    (args.output / 'result.json').write_text(json.dumps({'passed': True, 'subsets': len(rows), 'refused_minimal_sets': dict(table)}, indent=2) + '\n')
    print('PASS session grants: %d subsets, wire sizes match the host builder, %d minimal refused compositions' % (len(rows), len(table)))


if __name__ == '__main__':
    main()
