#!/usr/bin/env python3
"""Run the native delay loop against a fractional counter and a stopped clock.

Compile the production C function with a host counter callback. Replace only
the AArch64 yield instruction with a host compiler barrier; its control flow
and sample bound remain unchanged. This does not qualify hardware timing.
"""
import argparse
from pathlib import Path
import subprocess
import harness


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--source", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = args.source or root / "platform/a733/npu_native.c"
    text = source.read_text()
    start = text.index("static void delay(void *cookie, unsigned us)")
    end = text.index("\nstatic int publish", start)
    function = text[start:end]
    instruction = '__asm__ volatile("yield" ::: "memory");'
    assert function.count(instruction) == 1
    function = function.replace(instruction, '__asm__ volatile("" ::: "memory");')
    prelude = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
struct a733_npu_native { unsigned unused; };
static uint64_t counter, calls;
static unsigned stopped;
static jmp_buf failed;
static uint64_t microseconds(struct a733_npu_native *n) {
    (void)n;++calls;
    uint64_t value=counter/24;
    if(!stopped)++counter;
    return value;
}
static _Noreturn void fatal(struct a733_npu_native *n) {(void)n;longjmp(failed,1);}
'''
    cases = r'''
int main(void) {
    struct a733_npu_native n={0};
    const unsigned intervals[]={5,20,40,50,100,1000};
    for(unsigned i=0;i<sizeof(intervals)/sizeof(*intervals);++i) {
        for(unsigned phase=0;phase<24;++phase) {
            counter=phase;calls=0;
            assert(!setjmp(failed));
            delay(&n,intervals[i]);
            assert(counter-phase>=(uint64_t)intervals[i]*24);
            assert(calls<=1048577);
        }
    }
    calls=0;stopped=1;
    if(!setjmp(failed)){delay(&n,50);assert(0);}
    assert(calls==1048577);
    calls=0;
    if(!setjmp(failed)){delay(&n,1001);assert(0);}
    assert(calls==0);
    puts("PASS NPU native delay: six intervals at all fractional phases; stopped clock and excessive delay preserve fatal bounds");
    return 0;
}
'''
    args.output.mkdir(parents=True, exist_ok=False)
    unit = args.output / "native-delay.c"
    unit.write_text(prelude + function + cases)
    exe = args.output / "native-delay"
    subprocess.run([str(harness.CLANG), "-std=c11", "-O1", "-Wall", "-Wextra",
                    "-Werror", *harness.SANITIZE, str(unit), "-o", str(exe)],
                   check=True, env=harness.environment())
    subprocess.run([str(exe)], check=True, env=harness.environment())


if __name__ == "__main__":
    main()
