#!/usr/bin/env python3
"""Build a finite NPU supply client with the accepted returning platform objects."""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode=True;sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from returning_build import ROOT,platform_arguments,platform_from

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence',type=Path,required=True)
platform_arguments(p)
a=p.parse_args();r=ROOT;e=a.evidence.resolve()
platform=platform_from(a,e)
platform.compile([r/'apps/npu-returning/main.c'])
platform.reuse([f'{i:02d}-{name}' for i,name in enumerate(['unused','shared_locks','counter_clock','a733_clock_leases','a733_rail_leases','a733_pmic','a733_pmic_native','a733_pmic_consumer','a733_pins','a733_pin_tables','a733_pins_native','a733_gpu_resources','a733_gpu_resources_native','tasks','queue']) if i])
platform.link();platform.binary();platform.run()
# Record the exact compiler dependency closure for the finite client.
platform.record(__file__)
print(f'Built NPU supply returning image; {len(platform.reused)} platform objects, one NPU supply client.')
platform.copy_helpers('npu-returning')
