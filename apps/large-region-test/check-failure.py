#!/usr/bin/env python3
"""Check that storage failure injection follows a complete, validated region load."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', required=True, type=Path)
parser.add_argument('--cc', default='/usr/bin/clang-21')
args = parser.parse_args()
args.build.mkdir(parents=True, exist_ok=True)
app = ROOT / 'apps/large-region-test'
normal = json.loads((app / 'manifest-return.json').read_text())
manifest = json.loads((app / 'manifest-failure-read.json').read_text())
expected = json.loads(json.dumps(normal))
expected['deadlines']['run_us'] = 40_000_000
assert manifest == expected, 'The diagnostic changes only the declared run time.'
spec = importlib.util.spec_from_file_location('region_test_file', ROOT / 'tools/region_test_file.py')
stream = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stream)
region = manifest['large_regions'][0]
assert region['source_bytes'] == stream.SIZE == 536_870_912
assert region['sha256'] == stream.EXPECTED_SHA256
assert region['file'] == '/sunstone-test/sunstone-region-test.bin'
generated = args.build / 'generated'
subprocess.run([sys.executable, '-B', str(ROOT / 'tools/sunstone-manifest.py'), 'emit',
                '--manifest', str(app / 'manifest-failure-read.json'), '--output', str(generated)], check=True)
(generated / 'FreeRTOS.h').write_text('#include <stdint.h>\ntypedef uint64_t TickType_t;\n')
(generated / 'task.h').write_text('void vTaskDelay(TickType_t ticks);\n')
fixture = r'''
#include "large_region-app.h"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
static jmp_buf end;
static unsigned mode, waits, statuses, bases, clocks, delays, terminal, value;
static char loaded;
int ss_clock_ready(void) { return mode != 2 || clocks++ != 0; }
int ss_live(void) { return mode == 11 ? -1 : 1; }
void ss_progress(void) {}
void vTaskDelay(uint64_t ticks) { assert(ticks == 1); ++delays; }
int ss_large_wait(unsigned index, uint64_t timeout)
{
 assert(index == LARGE_REGION_LARGE_WEIGHTS && timeout == 1000000); ++waits;
 if (mode == 1 && waits < 3) return 0;
 return mode == 3 ? -1 : mode == 11 ? 0 : 1;
}
int ss_large_status(unsigned index, struct ss_large_status *out)
{
 assert(index == LARGE_REGION_LARGE_WEIGHTS && waits); ++statuses;
 *out = (struct ss_large_status){.state = SS_LARGE_READY, .loaded = 536870912, .total = 536870912};
 if (mode == 5) out->state = SS_LARGE_LOADING;
 if (mode == 6) --out->loaded;
 if (mode == 7) --out->total;
 if (mode == 3 || mode == 8) out->error = 9;
 return mode == 4 ? -1 : 0;
}
void *ss_large_base(unsigned index, size_t *bytes)
{
 assert(index == LARGE_REGION_LARGE_WEIGHTS && statuses == 1); ++bases;
 *bytes = mode == 10 ? 536870912 : 1073741824;
 return mode == 9 ? 0 : &loaded;
}
_Noreturn void ss_fail(uint32_t detail) { terminal = detail; longjmp(end, 1); }
_Noreturn void ss_fail_value(uint32_t detail, uint32_t v)
{ terminal = detail; value = v; longjmp(end, 1); }
int main(int argc, char **argv)
{
 assert(argc == 2); mode = strtoul(argv[1], 0, 10);
 struct ss_grant grant = {0};
 if (!setjmp(end)) large_region_main(&grant);
 unsigned expected = mode <= 2 ? 0x780 : mode == 3 || mode == 11 ? 0x781 : mode <= 8 ? 0x782 : 0x783;
 assert(terminal == expected && statuses == 1);
 assert(waits == (mode == 1 ? 3 : 1) && delays == (mode == 2));
 if (mode <= 2) assert(value == 536870912 && bases == 1);
 else assert(value == (mode == 3 ? 9 : 0) && bases == (mode == 9 || mode == 10));
 return 0;
}
'''
(args.build / 'fixture.c').write_text(fixture)
binary = args.build / 'failure'
flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'include'), '-I' + str(generated)]
subprocess.run([args.cc, *flags, '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                str(args.build / 'fixture.c'), str(app / 'failure.c'), '-o', str(binary)], check=True)
for case in range(12):
    subprocess.run([str(binary), str(case)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
result = {'cases': 12, 'manifest_matches_recorded_stream': True, 'physical_access': False,
          'scope': 'Application control flow with modeled loader status; physical card data and retirement remain untested.'}
(args.build / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
