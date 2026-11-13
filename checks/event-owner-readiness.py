#!/usr/bin/env python3
"""Check that readiness changes wake a blocked application without periodic hints."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/cpu.c', 'apps/os/thermal.c', 'include/a733_thermal.h']
GATE_RUNS = [['--output', '{output}']]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    cpu = (ROOT / 'apps/os/cpu.c').read_text()
    thermal = (ROOT / 'apps/os/thermal.c').read_text()
    ready = function(cpu, 'static void publish_ready(void)')
    load = function(thermal, 'static void publish_load(enum a733_thermal_level level)')
    classify = function(thermal, 'int os_thermal_throttled(void)')
    assert cpu.count('publish_ready();') == 2
    unit = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include "a733_thermal.h"
static struct { atomic_uint ready; } owner;
static struct { struct a733_thermal_policy policy; } thermal;
static _Atomic uint64_t load_state = A733_THERMAL_THROTTLE;
static uint64_t tick, until;
static unsigned hints, seen, advance_on_fresh, refresh_on_clock;
static uint64_t a733_smp_counter(void){
    if(refresh_on_clock){refresh_on_clock=0;atomic_store(&load_state,(until<<2)|A733_THERMAL_NORMAL);}
    return tick*24;
}
static uint64_t fresh_until(void){if(advance_on_fresh){advance_on_fresh=0;++tick;}return until;}
unsigned a733_thermal_policy_seen(const struct a733_thermal_policy *p){(void)p;return seen;}
static void os_application_wake_main(void){++hints;}
'''
    exercise = r'''
int main(void){
    publish_ready(); assert(hints==1 && atomic_load(&owner.ready)==1);
    publish_ready(); assert(hints==1);
    tick=100; until=1000; seen=0;
    publish_load(A733_THERMAL_NORMAL); assert(hints==2);
    until=1100; publish_load(A733_THERMAL_NORMAL); assert(hints==2);
    seen=31; publish_load(A733_THERMAL_NORMAL); assert(hints==3);
    until=1200; publish_load(A733_THERMAL_NORMAL); assert(hints==3);
    tick=1300; until=2300; publish_load(A733_THERMAL_NORMAL); assert(hints==4);
    publish_load(A733_THERMAL_THROTTLE); assert(hints==5);
    until=2400; publish_load(A733_THERMAL_THROTTLE); assert(hints==5);
    publish_load(A733_THERMAL_CRITICAL); assert(hints==6);
    atomic_store(&load_state,(UINT64_C(1000)<<2)|A733_THERMAL_NORMAL);
    tick=1000;until=2000;advance_on_fresh=1;
    publish_load(A733_THERMAL_NORMAL);assert(tick==1001 && hints==7);
    atomic_store(&load_state,(UINT64_C(1000)<<2)|A733_THERMAL_NORMAL);
    tick=1001;until=2000;refresh_on_clock=1;
    assert(!os_thermal_throttled());
    atomic_store(&load_state,(UINT64_C(1000)<<2)|A733_THERMAL_NORMAL);
    assert(os_thermal_throttled());tick=1000;assert(!os_thermal_throttled());
    return 0;
}
'''
    early_clock = load.replace('    uint64_t now=a733_smp_counter()/24;\n', '')
    early_clock = early_clock.replace('    uint64_t next=', '    uint64_t now=a733_smp_counter()/24;\n    uint64_t next=')
    stale_reader = '''int os_thermal_throttled(void){
        uint64_t state=atomic_load_explicit(&load_state,memory_order_acquire);
        return (state&3)!=A733_THERMAL_NORMAL||a733_smp_counter()/24>(state>>2);
    }'''
    for name, cpu_code, load_code, reader_code, expected in (
        ('readiness', ready, load, classify, True),
        ('missing-wake', ready.replace('os_application_wake_main();', '(void)0;'), load, classify, False),
        ('freshness-poll', ready, load[:-1]+'    os_application_wake_main();\n}', classify, False),
        ('expiry-before-publish', ready, early_clock, classify, False),
        ('stale-reader', ready, load, stale_reader, False),
    ):
        body = unit + cpu_code + '\n' + load_code + '\n' + reader_code + exercise
        source = args.output / (name + '.c')
        source.write_text(body)
        executable = args.output / name
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-I', str(ROOT / 'include'),
                        str(source), '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        (args.output / (name + '.log')).write_text(result.stdout + result.stderr)
        assert (result.returncode == 0) == expected, (name, result.returncode)
    print('PASS readiness: CPU ready, sensor freshness, expiry interleavings and policy changes; four mutations rejected')


if __name__ == '__main__':
    main()
