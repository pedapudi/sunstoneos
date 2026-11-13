#!/usr/bin/env python3
"""Check the short read manifest, native result layout and SD result decoder."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
from harness import environment

ROOT=Path(__file__).resolve().parents[1]
CHECK_INPUTS=['apps/large-region-test/main.c','apps/large-region-test/host.py',
              'apps/large-region-test/manifest-return.json','apps/large-region-test/manifest-failure-read.json',
              'include/sunstone_storage.h','checks/event-sd-app.c','checks/event-sd-app.py']


def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    app=ROOT/'apps/large-region-test'
    source=(app/'main.c').read_text()
    extraction=load('sd_app_extract',ROOT/'checks/os-quiesce-storage.py')
    start=source.index('struct large_region_test_result {');end=source.index('};',start)+2
    model=source[start:end]+'\n'+extraction.function(source,'large_region_main')
    (args.output/'event-sd-app-production.h').write_text(model)
    generated=args.output/'generated'
    subprocess.run([sys.executable,'-B',str(ROOT/'tools/sunstone-manifest.py'),'emit','--manifest',
                    str(app/'manifest-failure-read.json'),'--output',str(generated)],check=True,env=environment())
    normal=json.loads((app/'manifest-return.json').read_text());short=json.loads((app/'manifest-failure-read.json').read_text())
    normal['deadlines']['run_us']=40000000;assert short==normal
    executable=args.output/'app'
    subprocess.run(['/usr/bin/clang-21','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all','-DSUNSTONE_LARGE_REGION_SD_COUNTERS=1',
                    '-I'+str(ROOT/'include'),'-I'+str(generated),'-I'+str(args.output),
                    str(ROOT/'checks/event-sd-app.c'),'-o',str(executable)],check=True,env=environment())
    decoder=load('sd_app_decoder',app/'host.py')
    for case in range(9):
        payload=args.output/f'case-{case}.bin'
        run=subprocess.run([str(executable),str(case),str(payload)],capture_output=True,text=True,env=environment())
        (args.output/f'case-{case}.log').write_text(run.stdout+run.stderr);assert not run.returncode,run.stdout+run.stderr
        if case>=5:continue
        try:decoded=decoder.decode(payload.read_bytes())
        except ValueError:assert case==4;continue
        failures=decoder.failures(decoded)
        assert bool(failures)==(case in (2,3)),(case,failures)
        if case in (0,1):assert decoded['retired_idle_wakes_per_second']==0
    (args.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=9,
        scope='Application control flow, native result layout and decoder; device and memory effects are modeled.'),indent=2)+'\n')
    print('PASS nine short-read application/decoder cases; storage and memory effects are modeled')


if __name__=='__main__':main()
