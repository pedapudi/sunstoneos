#!/usr/bin/env python3
"""Check failure injection after the empty-port owner checkpoint predicate."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--cc', default='/usr/bin/clang-21')
args = parser.parse_args()
args.build.mkdir(parents=True, exist_ok=True)
app = ROOT / 'apps/usb-empty-fail'
manifest = json.loads((app / 'manifest.json').read_text())
assert manifest['devices'] == {'required': ['usb-keyboard'], 'optional': []}
assert manifest['regions'] == [] and manifest['cpu']['clock_grants']
generated = args.build / 'generated'
subprocess.run([sys.executable, '-B', str(ROOT / 'tools/sunstone-manifest.py'), 'emit',
                '--manifest', str(app / 'manifest.json'), '--output', str(generated)], check=True)
(generated / 'FreeRTOS.h').write_text('#include <stdint.h>\ntypedef uint64_t TickType_t;\n')
(generated / 'task.h').write_text('void vTaskDelay(TickType_t);\n')
fixture = r'''
#include "usb_empty_fail-app.h"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
static jmp_buf end;
static unsigned mode, polls, progress, delays, terminal, value;
int os_usb_failure_ready(void) { ++polls; return mode == 0 || (mode == 1 && polls == 4); }
void ss_progress(void) { ++progress; }
void vTaskDelay(uint64_t ticks)
{ assert(ticks == 1 && progress == ++delays); if (mode == 5 && delays == 8) longjmp(end, 1); }
int ss_live(void) { assert(!"The empty port never becomes attached."); return 0; }
_Noreturn void ss_fail(uint32_t detail) { terminal = detail; longjmp(end, 1); }
_Noreturn void ss_fail_value(uint32_t detail, uint32_t v)
{ terminal = detail; value = v; longjmp(end, 1); }
int main(int argc, char **argv)
{
 assert(argc == 2); mode = strtoul(argv[1], 0, 10);
 struct ss_grant grant = {.devices = SS_DEVICE_USB_KEYBOARD};
 if (mode == 2) grant.devices = 0;
 if (mode == 3) grant.devices |= SS_DEVICE_USB_STORAGE;
 if (mode == 4) grant.region_count = 1;
 if (!setjmp(end)) usb_empty_fail_main(&grant);
 if (mode <= 1) assert(terminal == 0x790 && value == 3 && polls == (mode ? 4 : 1));
 else if (mode == 5) assert(!terminal && !value && polls == 8 && delays == 8);
 else assert(terminal == 0x791 && !value && !polls && !delays);
 assert(delays == progress); return 0;
}
'''
(args.build / 'fixture.c').write_text(fixture)
binary = args.build / 'fixture'
subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-fno-sanitize-recover=all', '-I' + str(ROOT / 'include'), '-I' + str(generated),
                str(args.build / 'fixture.c'), str(app / 'main.c'), '-o', str(binary)], check=True)
for case in range(6):
    subprocess.run([str(binary), str(case)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
result = {'cases': 6, 'physical_access': False,
          'scope': 'Application waits for the owner predicate and never requires an attached-device live state.'}
(args.build / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
