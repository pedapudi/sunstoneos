#!/usr/bin/env python3
"""Plan a resident card bundle using ordinary files; no physical writer exists here.

The policy schema is sunstone-card-boot-policy-v1, scope resident-cold-boot.
It binds image_sha256, stage2_sha256, manifest_sha256, canonical_session_sha256,
workers, identity, target {port,sysfs}, four sid_words, sid_authority and an
evidence_class of board-records or model-fixture. Evidence references are
{path,sha256} relative to the policy file: qualification, recovery, wiring,
memory and thermal_initial/electrical/clocks/supervision. An active CPU grant
also requires cpu_b1 and cpu {clusters,max_mhz,vendor_mv,bin_column,changes_per_boot}.
The current CPU perf and reserved bytes are zero and remain in the canonical
hash. The policy digest belongs to the bundle only. Model evidence requires
--model-fixture; every bundle records physical_qualified:false.

Each --asset PATH@LOAD is prepacked by its caller. At most two assets may
cover the linked manifest's authenticated input slices. Other asset bytes
are zero. Outputs include monitor packaging, a generation-bound slot plan
and storage-update.py session candidates. Applying them to hardware requires
a separately reviewed writer, range backups and coordinator approval.
Only complete bundles are published at --output. Failed staging remains in
a private hidden sibling. A helper plan's output_directory is an informational
creation path; artifact names resolve beside the plan after relocation.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('card_boot_producer', ROOT / 'apps/os/card.py')
card = importlib.util.module_from_spec(spec)
spec.loader.exec_module(card)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('image', 'link-check', 'session', 'policy', 'stage2', 'stage2-elf', 'boot0',
                 'allocation', 'backup', 'card-image', 'card', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--asset', action='append', default=[], metavar='PATH@LOAD')
    parser.add_argument('--packer', type=Path, help='Optional pinned vendor packer cross-check.')
    parser.add_argument('--model-fixture', action='store_true')
    args = parser.parse_args(argv)
    try:
        result = card.prepare(args)
    except (ValueError, OSError, KeyError, TypeError, AssertionError, struct.error) as error:
        parser.exit(1, 'card-boot: %s\n' % error)
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
