#!/usr/bin/env python3
"""Build one returning CPU operating-point bundle from the current platform."""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from returning_build import ROOT, PROVIDER_UNITS, ReturningPlatform, copy_closure

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--bundle', type=int, choices=(20, 21, 22, 23, 24, 30, 31, 32, 33), default=33)
p.add_argument('--platform-build', type=Path, required=True)
p.add_argument('--evidence', type=Path, required=True)
p.add_argument('--cpu-return-dcdc3-800', action='store_true',
               help='Bundle33 variant: DCDC3 already at 800 mV; lower only DCDC5 after boot-clock admission')
a = p.parse_args()
if a.cpu_return_dcdc3_800 and a.bundle != 33:
    p.error('--cpu-return-dcdc3-800 requires --bundle 33')
platform = ReturningPlatform(a.evidence.resolve(), build=a.platform_build.resolve())
units = ['apps/board-bundles-power/power.c', 'apps/board-bundles-power/trace.c',
         'apps/board-bundles-power/cpu-opp-step.c', 'drivers/power/a733_power_domains.c',
         'drivers/power/a733_power_domains_native.c', 'arch/aarch64/device_memory.c',
         'drivers/clock/a733_cpu_clock.c', 'drivers/thermal/a733_thermal.c',
         'drivers/thermal/a733_thermal_native.c', 'drivers/adc/a733_adc_native.c', 'platform/a733/cpu_pmu.c']
platform.compile([ROOT / u for u in units], [f'-DOPP_BUNDLE={a.bundle}',
    *(['-DOS_CPU_RETURN_DCDC3_800=1'] if a.cpu_return_dcdc3_800 else [])])
platform.reuse(list(PROVIDER_UNITS))
platform.link()
platform.binary()
platform.run()
provider_inputs = copy_closure(platform.evidence, sorted(platform.objects_dir.glob('provider-*.obj.d')),
                              directory='provider-inputs')
platform.record(__file__, {'bundle': a.bundle, 'provider_compiler_inputs': provider_inputs,
                          'execution_variant': 'os-cpu-return-dcdc3-800-v1' if a.cpu_return_dcdc3_800 else 'legacy',
                          'physical_access': False})
print(f'Built CPU bundle {a.bundle}; physical execution requires separate admission and review.')
