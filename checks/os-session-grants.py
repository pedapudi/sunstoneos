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
CHECK_INPUTS = ('apps/os/grants.c', 'apps/os/os.h', 'apps/os/os-services.h', 'apps/os/host.py', 'tools/sunstone-manifest.py')
# The grants, records and supervised ids come from the declaration (apps/os/os-services.h): the harness prints the grant
# bits and service ids as C compiles them, and the host builder's records (apps/os/host.py) are read from the same rows.
GATE_RUNS = [['--build', '{build:all}', '--output', '{output}']]
HARNESS = r'''
#include "os.h"
#include <stdio.h>
const struct os_session *os_session_validated(void){return NULL;}
int main(void)
{
    for(unsigned m=1;m<=6;m+=m==2?1:m==3?3:1){   /* main cores 1, 2 and 6 */
        if(m!=1&&m!=2&&m!=6)continue;
        for(unsigned w=0;w<=8;++w){uint32_t mask=0;int ok=os_worker_cores(m,w,&mask);printf("W %u %u %d %u\n",m,w,ok,mask);}
    }
#define GRANT(NAME,bit,with) printf("G %s %u %d\n",#NAME,(unsigned)OS_GRANT_##NAME,(int)OS_LINKED_##NAME);all|=OS_GRANT_##NAME;
    unsigned all=0;OS_GRANT_ROWS(GRANT)
#define SERVICE(NAME,name,window,grant,with) with(printf("S %s %u %u %s\n",#NAME,(unsigned)OS_SERVICE_##NAME,(unsigned)(grant),name);)
    OS_SUPERVISED_ROWS(SERVICE)
    printf("N %u\n",(unsigned)OS_SERVICES);
    for(unsigned g=0;g<=all;++g)if(!(g&~all))
        printf("%u %zu %u %s\n",g,os_session_wire_bytes(g),os_composition_refused(g),
               os_composition_refused(g)?os_composition_reason(os_composition_refused(g)):"");
    return 0;
}
'''
# The admitted session is validated once: after the first success, os_session_admitted and os_granted are loads.
CACHE_HARNESS = r'''
#include "os.h"
#include <stdio.h>
static struct os_session session;
static unsigned validations,sealed;
const struct os_session *os_session_validated(void){++validations;return sealed?&session:NULL;}
int main(void)
{
    session.granted=OS_GRANT_AUDIO|OS_GRANT_RADIO;
    /* Before admission nothing is cached and nothing is granted. */
    if(os_granted(OS_GRANT_AUDIO)||os_session_admitted()||validations!=2)return 1;
    sealed=1;
    if(!os_granted(OS_GRANT_AUDIO)||validations!=3)return 2;
    for(unsigned i=0;i<100000;++i){
        if(os_session_admitted()!=&session||!os_granted(OS_GRANT_RADIO)||!os_granted(OS_GRANT_AUDIO|OS_GRANT_RADIO)||
           os_granted(OS_GRANT_NPU)||os_granted(0)||os_granted(OS_GRANT_AUDIO|OS_GRANT_NPU))return 3;
    }
    printf("%u\n",validations);
    return validations==3?0:4;
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
    placement = {}
    grants, supervised, count = {}, [], 0
    for line in subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout.splitlines():
        if line.startswith('W '):
            _, main, workers, ok, mask = line.split()
            placement[(int(main), int(workers))] = (int(ok), int(mask))
            continue
        if line.startswith('G '):
            _, name, bit, linked = line.split()
            grants[name] = int(bit)
            continue
        if line.startswith('S '):
            _, name, number, needs, label = line.split(' ', 4)
            supervised.append((name, int(number), int(needs), label.strip('"')))
            continue
        if line.startswith('N '):
            count = int(line.split()[1])
            continue
        granted, size, refused, reason = (line.split(' ', 3) + [''])[:4]
        rows[int(granted)] = (int(size), int(refused), reason)

    (args.output / 'cache.c').write_text(CACHE_HARNESS)
    cache_exe = args.output / 'cache'
    done = subprocess.run([CLANG, '-std=gnu11', '-O1', '-w', '-no-pie', *defines, *includes, '-I' + str(ROOT / 'apps/os'),
                           str(ROOT / 'apps/os/grants.c'), str(args.output / 'cache.c'), '-o', str(cache_exe)],
                          capture_output=True, text=True, env=environment())
    assert done.returncode == 0, done.stderr[-1500:]
    done = subprocess.run([str(cache_exe)], capture_output=True, text=True)
    assert done.returncode == 0, ('the admitted session was validated again after the first success', done.returncode, done.stdout)
    # The full validation is called from nowhere else: no hot path (the thermal step, the application and USB waits, the
    # service supervisor) can reach os_session_valid or the decode again.
    callers = sorted(str(p.relative_to(ROOT)) for p in [*(ROOT / 'apps').rglob('*.c'), *(ROOT / 'drivers').rglob('*.c')]
                     if re.search(r'\bos_session_validated\(|\bos_session_valid\(|\bos_session_decode', p.read_text()))
    assert set(callers) <= {'apps/os/admission.c', 'apps/os/session.c', 'apps/os/start.c', 'apps/os/grants.c'}, callers
    # Worker placement: within the main task's cluster first, the interrupt cores 0 and 1 last, and an A55 manifest never
    # on the A76 cores; more workers than the order holds are refused.
    bits = lambda *c: sum(1 << x for x in c)
    expect = {(2, 0): (1, 0), (2, 1): (1, bits(3)), (2, 3): (1, bits(3, 4, 5)), (2, 4): (1, bits(3, 4, 5, 0)),
              (2, 5): (1, bits(3, 4, 5, 0, 1)), (2, 6): (0, 0),
              (6, 1): (1, bits(7)), (6, 5): (1, bits(7, 2, 3, 4, 5)), (6, 7): (1, bits(7, 2, 3, 4, 5, 0, 1)), (6, 8): (0, 0),
              (1, 4): (1, bits(2, 3, 4, 5)), (1, 5): (1, bits(2, 3, 4, 5, 0)), (1, 6): (0, 0)}
    for key, (ok, mask) in expect.items():
        assert placement[key] == (ok, mask), (key, placement[key], (ok, mask))
    for (main, workers), (ok, mask) in placement.items():
        if main != 6 and ok:
            assert not mask & bits(6, 7), (main, workers)
        if ok and workers <= 3 and main == 2:
            assert not mask & bits(0, 1), (main, workers)
    import importlib.util
    spec = importlib.util.spec_from_file_location('session_host', ROOT / 'apps/os/host.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    for granted, (size, refused, reason) in rows.items():
        # The size the host builder writes into the thermal record: the prefix and every service record that is present.
        expected = host.PREFIX_BYTES + host.SERVICE_BYTES[host.THERMAL] + sum(
            r['bytes'] for r in host.RECORDS.values() if granted & grants[r['grant']])
        assert size == expected, (granted, size, expected)
        assert (refused != 0) == bool(reason), (granted, refused, reason)
    # A single service is never refused; every refused set holds at least two, and a superset of a refused set is refused.
    for granted, (_, refused, _) in rows.items():
        if bin(granted).count('1') < 2:
            assert not refused, granted
        if refused:
            assert all(rows[g][1] for g in rows if g & granted == granted), granted
    names = lambda g: '+'.join(n for n, b in grants.items() if g & b)
    table = sorted({names(g): rows[g][2] for g in rows if rows[g][1] and not any(rows[h][1] for h in rows if h != g and h & g == h)}.items())
    # The grant bits are distinct, one per row, and every record names a grant. The supervised ids follow the application
    # (5) in row order, every row's grant is a set of declared grants, and only the headset's rows depend on a build option:
    # every other service has its id in every image.
    assert len(grants) == len(set(grants.values())) and all(b and not b & (b - 1) for b in grants.values()), grants
    assert all(r['grant'] in grants for r in host.RECORDS.values()), host.RECORDS
    every = sum(grants.values())
    assert [n for _, n, _, _ in supervised] == list(range(6, 6 + len(supervised))) and count == 6 + len(supervised), (supervised, count)
    assert all(not needs & ~every for _, _, needs, _ in supervised), supervised
    declared = host.manifest_tool.os_service_rows('OS_SUPERVISED_ROWS')
    assert {row[0] for row in declared if row[4] != 'OS_ALWAYS'} == {'BLUETOOTH', 'HEADSET'}, declared
    assert [s[0] for s in supervised] == [row[0] for row in declared if row[4] == 'OS_ALWAYS' or row[0] in {s[0] for s in supervised}]
    (args.output / 'result.json').write_text(json.dumps({'passed': True, 'subsets': len(rows), 'refused_minimal_sets': dict(table)}, indent=2) + '\n')
    print('PASS session grants: %d subsets, wire sizes match the host builder, %d minimal refused compositions' % (len(rows), len(table)))


if __name__ == '__main__':
    main()
