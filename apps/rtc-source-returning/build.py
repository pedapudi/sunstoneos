#!/usr/bin/env python3
"""Build source-only or alarm RTC operation with frozen returning objects."""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode=True;sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from returning_build import ROOT,platform_arguments,platform_from

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--operation',choices=('source','alarm'),default='source');p.add_argument('--evidence',type=Path,required=True)
platform_arguments(p)
a=p.parse_args();r=ROOT;e=a.evidence.resolve();app=r/f'apps/rtc-{a.operation}-returning'
platform=platform_from(a,e)
platform.compile([app/'main.c',app/'monitor.c',r/'drivers/rtc/a733_rtc.c',r/('drivers/rtc/a733_rtc_initialize.c' if a.operation=='source' else 'drivers/gpu/gicv3_spi.c')],['-iquote',str(app)])
platform.reuse(['02-counter_clock','13-tasks','14-queue'])
wraps=['monitor_init']+(['monitor_exception','vApplicationFPUSafeIRQHandler'] if a.operation=='alarm' else [])
platform.link(options=[f'-Wl,--wrap={x}' for x in wraps]);platform.binary();platform.run()
platform.record(__file__,{'operation':a.operation,'wraps':wraps})
platform.copy_helpers(f'rtc-{a.operation}-returning',())
print(f'Built {a.operation} returning RTC image; four target units, {len(platform.reused)} platform objects.')
