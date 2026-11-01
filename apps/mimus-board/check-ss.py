#!/usr/bin/env python3
"""Run the native mimus adapter, mailbox and keyboard gate with modeled devices."""
import argparse
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'checks'))
from harness import CLANG, SANITIZE, Steps

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--resume', action='store_true')
a = p.parse_args()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=a.resume)
(out / 'FreeRTOS.h').write_text('''#include <stdint.h>
#include <assert.h>
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef uintptr_t StackType_t;
typedef void *TaskHandle_t;
typedef struct { uintptr_t words[32]; } StaticTask_t;
typedef void (*TaskFunction_t)(void *);
#define configSTACK_DEPTH_TYPE uint32_t
#define configASSERT(x) assert(x)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
''')
(out / 'task.h').write_text('''#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void vTaskDelay(TickType_t);
void vTaskSuspend(TaskHandle_t);
TaskHandle_t xTaskCreateStaticAffinitySet(TaskFunction_t, const char *,
    configSTACK_DEPTH_TYPE, void *, UBaseType_t, StackType_t *, StaticTask_t *, UBaseType_t);
#ifdef __cplusplus
}
#endif
''')
common = ['-O1', '-g', '-Wall', '-Wextra', '-Werror', *SANITIZE,
          '-DSUNSTONE_A733_OS=1', '-I' + str(out), '-I' + str(ROOT / 'include'),
          '-I' + str(ROOT / 'apps/mimus-board')]
steps = Steps(out, 'commands.json')
# LeakSanitizer cannot inspect threads under the sandbox's process tracing.
os.environ.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
def run(command, name):
    steps.command(name, command, timeout=10)

run([CLANG, '-std=c11', *common, '-c', ROOT / 'apps/mimus-board/keyboard.c',
     '-o', out / 'keyboard.o'], 'compile-keyboard')
run([CLANG, '-x', 'c++', '-std=c++20', '-pthread', *common,
     ROOT / 'apps/mimus-board/recognition-ss.cc', ROOT / 'apps/mimus-board/ss-host.cc',
     ROOT / 'apps/mimus-board/check-ss.cc', '-x', 'none', out / 'keyboard.o',
     '-lstdc++', '-o', out / 'adapter'], 'compile-adapter')
results = {}
for case in ('staged', 'capture', 'headset', 'keyboard', 'no-overlap'):
    run([out / 'adapter', case], case)
    results[case] = True
(out / 'results.json').write_text(json.dumps(dict(passed=True, cases=results,
    physical_access=False, inference='modeled; adapter and keyboard gate execute'), indent=2) + '\n')
print(json.dumps(results))
