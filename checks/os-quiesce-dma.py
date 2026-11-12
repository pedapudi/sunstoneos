#!/usr/bin/env python3
"""Check the DMA return adapter against idle retirement and retained ownership.

The real callback runs with modeled service and bootstrap replies. This check
establishes adapter control flow, not hardware drain. dma-retirement.py runs the
worker's idle retirement and abandonment cases with the real controller.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/dma.c', 'apps/os/quiesce.h', 'apps/os/quiesce.c',
                'include/dma_service.h', 'drivers/dma/dma_service.c', 'include/sunstone_crumb.h']


def function(text, name):
    match = re.search(r'^[^\n]*\b' + re.escape(name) + r'\([^;]*?\)\s*\{', text, re.M)
    assert match, name
    begin = match.start()
    depth, at = 1, match.end()
    while depth:
        depth += text[at] == '{'
        depth -= text[at] == '}'
        at += 1
    return text[begin:at]


def registered(text):
    start = function(text, 'os_dma_start')
    return ('os_quiesce_register(&dma_quiesce)' in start and
            start.index('os_quiesce_register') < start.index('a733_dma_pool_session_init'))


PRELUDE = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "quiesce.h"
#undef SUNSTONE_CRUMB_HERE
#define SUNSTONE_CRUMB_HERE() ((const void *)(uintptr_t)0x1234)
enum { A733_DMA_POOL_EMPTY, A733_DMA_POOL_WAITING, A733_DMA_POOL_READY, A733_DMA_POOL_FAILED };
struct service { atomic_uint quiesce_fault; };
struct pool { atomic_uint state; struct service service; };
static struct { struct pool pool; atomic_uint ready; unsigned mask_fault; } dma;
static int closing, failing, service_reply, bootstrap_reply;
static unsigned service_calls, bootstrap_calls, panic_calls, suspended, exits;
static jmp_buf ending;
int os_cpu_return_requested(void) { return closing; }
int os_cpu_return_failing(void) { return failing; }
static int dma_service_quiesce(struct service *s)
{ assert(s==&dma.pool.service); ++service_calls; return service_reply; }
static int a733_dma_pool_session_retire(struct pool *p)
{ assert(p==&dma.pool); ++bootstrap_calls; return bootstrap_reply; }
static void vTaskSuspend(void *p) { assert(!p); ++suspended; longjmp(ending,1); }
static _Noreturn void platform_panic(const char *name,unsigned line)
{ (void)name;(void)line;++panic_calls;longjmp(ending,1); }
static _Noreturn void platform_exit(unsigned detail)
{ (void)detail;++exits;exit(90); }
'''
TEST = r'''
static void reset(void)
{
    atomic_store(&dma.ready,0);atomic_store(&dma.pool.state,A733_DMA_POOL_READY);
    atomic_store(&dma_quiesce.result,OS_QUIESCE_PENDING);
    atomic_store(&dma_quiesce.fault_state,0);atomic_store(&dma_quiesce.requested,1);
    dma.mask_fault=0;atomic_store(&dma.pool.service.quiesce_fault,0);service_reply=bootstrap_reply=0;
    service_calls=bootstrap_calls=panic_calls=suspended=exits=0;closing=failing=1;
}
int main(void)
{
    assert(os_quiesce_register(&dma_quiesce));
    reset();atomic_store(&dma.ready,1);assert(dma_quiesce_step(&dma_quiesce)==OS_QUIESCE_DONE&&!service_calls);
    reset();atomic_store(&dma.pool.state,A733_DMA_POOL_WAITING);
    assert(dma_quiesce_step(&dma_quiesce)==OS_QUIESCE_PENDING&&!service_calls);
    reset();assert(dma_quiesce_step(&dma_quiesce)==OS_QUIESCE_PENDING&&service_calls==1&&!bootstrap_calls);
    reset();service_reply=1;assert(dma_quiesce_step(&dma_quiesce)==OS_QUIESCE_PENDING&&bootstrap_calls==1);
    reset();service_reply=bootstrap_reply=1;
    assert(dma_quiesce_step(&dma_quiesce)==OS_QUIESCE_DONE&&atomic_load(&dma.ready));
    reset();service_reply=-1;
    assert(dma_quiesce_step(&dma_quiesce)==OS_QUIESCE_FAULT&&!bootstrap_calls);
    assert((dma_quiesce.site&0xfff)==0x811&&dma_quiesce.value==A733_DMA_POOL_READY);
    reset();service_reply=1;bootstrap_reply=-1;dma.mask_fault=3;
    assert(dma_quiesce_step(&dma_quiesce)==OS_QUIESCE_FAULT);
    assert(dma_quiesce.value==(A733_DMA_POOL_READY|3u<<24));
    reset();atomic_store(&dma.pool.state,A733_DMA_POOL_FAILED);
    assert(dma_quiesce_step(&dma_quiesce)==OS_QUIESCE_FAULT&&!service_calls);
    reset();if(!setjmp(ending))fatal(NULL);
    assert(suspended==1&&!panic_calls&&!exits&&(dma_quiesce.site&0xfff)==0x812);
    reset();closing=failing=0;atomic_store(&dma_quiesce.requested,0);
    if(!setjmp(ending))fatal(NULL);
    assert(panic_calls==1&&!suspended&&!exits);
    puts("PASS DMA return adapter: idle completion, pending proof, retained fault and fatal parking");
}
'''


def build(text, out, name):
    directory = out / name
    directory.mkdir(parents=True, exist_ok=True)
    descriptor = re.search(r'static struct os_quiesce dma_quiesce=\{.*?\};', text, re.S).group(0)
    source = directory / 'check.c'
    source.write_text(PRELUDE + function(text, 'dma_quiesce_step') + '\n' + descriptor + '\n' +
                      function(text, 'fatal') + '\n' + TEST)
    binary = directory / 'check'
    command = ['clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-function',
               '-fsanitize=address,undefined', '-I' + str(ROOT / 'apps/os'), '-I' + str(ROOT / 'include'),
               str(source), str(ROOT / 'apps/os/quiesce.c'), '-o', str(binary)]
    compile_result = subprocess.run(command, capture_output=True, text=True)
    (directory / 'compile.json').write_text(json.dumps({'command': command, 'returncode': compile_result.returncode,
        'stdout': compile_result.stdout, 'stderr': compile_result.stderr}, indent=2) + '\n')
    if compile_result.returncode:
        raise RuntimeError('DMA fixture compilation failed: ' + str(source) + '\n' +
                           compile_result.stdout + compile_result.stderr)
    return subprocess.run([str(binary)], capture_output=True, text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    text = (ROOT / 'apps/os/dma.c').read_text()
    assert registered(text), 'DMA starts without registering its return step'
    real = build(text, args.output, 'real')
    assert real.returncode == 0, real.stdout + real.stderr
    print(real.stdout.strip())
    assert not registered(text.replace('os_quiesce_register(&dma_quiesce)', '1')), 'missing registration mutation survived'
    body = function(text, 'dma_quiesce_step')
    mutant = body.replace('{', '{ platform_exit(8);', 1)
    result = build(text.replace(body, mutant), args.output, 'mutant-platform-exit')
    assert result.returncode != 0, 'platform_exit mutation survived'
    print('PASS DMA mutations: removed registration and a stop inside the return callback are rejected')


if __name__ == '__main__':
    main()
