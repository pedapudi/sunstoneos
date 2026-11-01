#!/usr/bin/env python3
"""Build a read-only PMIC observer; physical launch needs a qualified wrapper."""
import argparse
from pathlib import Path
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from returning_build import ROOT, platform_arguments, platform_from

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--evidence', type=Path, required=True)
platform_arguments(parser)
args = parser.parse_args()
out = args.evidence.resolve()
if out.exists():
    parser.error('use a fresh evidence directory')
platform = platform_from(args, out)
platform.compile([ROOT / 'apps/power-supply-observe/main.c'])
platform.reuse(['01-shared_locks', '02-counter_clock', '03-a733_clock_leases',
                '04-a733_rail_leases', '05-a733_pmic', '06-a733_pmic_native',
                '07-a733_pmic_consumer', '08-a733_pins', '09-a733_pin_tables',
                '10-a733_pins_native', '11-a733_gpu_resources',
                '12-a733_gpu_resources_native', '13-tasks', '14-queue'])
platform.link()
platform.binary()
platform.run()
platform.record(__file__, {'kind': 'read-only-pmic-observer',
                         'launch_ready': False,
                         'blocker': 'qualified host-armed watchdog/breadcrumb wrapper required'})
print('Built read-only supply observer; physical launch remains gated.')
