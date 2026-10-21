#!/usr/bin/env python3
"""Build a finite RTC client with the accepted returning platform objects."""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode=True;sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from returning_build import ROOT,digest,platform_arguments,platform_from

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence',type=Path,required=True)
platform_arguments(p)
a=p.parse_args();r=ROOT;e=a.evidence.resolve();b=e/'build'
platform=platform_from(a,e)
platform.compile([r/'apps/rtc-returning/main.c',r/'drivers/rtc/a733_rtc.c'])
initializer=platform.compile([r/'drivers/rtc/a733_rtc_initialize.c'],linked=False,prefix='new-check')
platform.reuse(['02-counter_clock','13-tasks','14-queue'])
platform.link();platform.binary()
# The policy fixture retains mutation entry points and the unchanged calendar
# initializer solely for focused rejection checks. They are not forced into
# the deployable returning image.
platform.link('policy',['-Wl,--undefined='+name for name in ('a733_rtc_open','a733_rtc_arm','a733_rtc_irq','a733_rtc_finish','a733_rtc_initialize','a733_rtc_set_calendar')],initializer)
platform.run()
# Record and copy the exact compiler dependency closure for the two runtime units and check-only initializer.
platform.record(__file__,{'policy_elf_sha256':digest(b/'policy.elf')})
print(f'Built RTC returning image; {len(platform.reused)} platform objects, two runtime units plus one check-only initializer.')
platform.copy_helpers('rtc-returning',('check.py','policy-check.py','README.md'))
