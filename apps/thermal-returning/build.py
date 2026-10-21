#!/usr/bin/env python3
"""Build a finite calibrated thermal client with the accepted returning platform objects."""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode=True;sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from returning_build import ROOT,platform_arguments,platform_from

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--device',choices=('thermal','adc'),default='thermal')
p.add_argument('--evidence',type=Path,required=True)
platform_arguments(p)
a=p.parse_args();r=ROOT;e=a.evidence.resolve()
platform=platform_from(a,e)
units=[r/'apps/thermal-returning/main.c',r/'drivers/thermal/a733_thermal.c',r/'drivers/thermal/a733_thermal_native.c',r/'drivers/adc/a733_adc_native.c'] if a.device=='thermal' else [r/'apps/thermal-returning/adc.c',r/'drivers/adc/a733_adc.c',r/'drivers/adc/a733_adc_native.c']
platform.compile(units)
names=['01-shared_locks','02-counter_clock','03-a733_clock_leases','13-tasks','14-queue']
if a.device=='adc':names += [f'{i:02}-{name}' for i,name in enumerate(['a733_rail_leases','a733_pmic','a733_pmic_native','a733_pmic_consumer','a733_pins','a733_pin_tables','a733_pins_native','a733_gpu_resources','a733_gpu_resources_native'],4)]
platform.reuse(names)
platform.link();platform.binary();platform.run()
# Record and copy the exact compiler dependency closure for the runtime units.
platform.record(__file__,{'device':a.device})
print(f'Built {a.device} returning image; {len(platform.reused)} platform objects, {len(units)} runtime units.')
platform.copy_helpers('thermal-returning')
