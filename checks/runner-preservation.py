#!/usr/bin/env python3
"""Check what the board runner preserves: its reach, the scratch attribute and the time estimate.

docs/designs/application-memory-layout.md, step 4. The runner backs up and restores memory over FEL, whose commands
carry 32-bit addresses. This check shows that

- a preservation plan with a WORK and an OUTPUT region above 0x80000000 is accepted and merged, as far as 4 GiB;
- an extent that reaches 4 GiB or lies above it is refused, and the message names the extent and the reach;
- a manifest region marked scratch is packed into the image, left out of the backup list, and refused on an input;
- the estimate is two passes (read, write) of the preserved bytes at the recorded FEL rate;
- a plan whose estimate exceeds the budget the submission names is refused with the estimate and the budget, and a
  plan without a budget, or inside it, is not.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
MIB = 1 << 20


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def refused(call, *words):
    try:
        call()
    except (SystemExit, AssertionError, ValueError) as error:
        for word in words:
            assert word in str(error), 'the refusal does not say %r: %s' % (word, error)
        return
    raise AssertionError('accepted although it should be refused (%s)' % ', '.join(words))


def plan_with(extents):
    return {'image_ram': (0x41000000, 0x1200000), 'input_regions': [],
            'geometry': {'report_address': 0x42000000, 'report_bytes': 0x4000, 'output_address': 0x42004000,
                         'output_bytes': 0x1000, 'monitor': (0x42000000, 0x42200000),
                         'cached_writable': (0x41200000, 0x42000000),
                         'application_regions': [{'address': a, 'bytes': n} for a, n in extents]}}


def main():
    argparse.ArgumentParser(description=__doc__).parse_args()
    layout = load('runner_layout', 'tools/memory_layout.py')
    images = load('runner_images', 'tools/board_images.py')
    host = load('runner_host', 'apps/os/host.py')
    tool = load('runner_manifest', 'tools/sunstone-manifest.py')
    record = layout.record()
    assert record['runner_dram'] == (0x40000000, 0x100000000)

    # An 8 MiB OUTPUT and a 16 MiB WORK at 0x88000000 are preserved and merged with the image's extents.
    plan = plan_with([(0x88000000, 16 * MIB), (0x89000000, 8 * MIB)])
    regions = images.preservation_regions(plan)
    assert (0x88000000, 24 * MIB) in regions, regions
    assert all(a >= 0x40000000 and a + n <= 1 << 32 for a, n in regions)
    top = plan_with([(0xff000000, 16 * MIB)])
    assert (0xff000000, 16 * MIB) in images.preservation_regions(top)
    for extent in ((0xff800000, 16 * MIB), (0x100000000, 2 * MIB), (0x3c0000000, 2 * MIB)):
        refused(lambda: images.preservation_regions(plan_with([extent])), '%#x' % extent[0], '0x100000000', 'scratch')

    # The estimate and the budget.
    plan['preserve_regions'] = regions
    total, seconds = images.preserve_estimate(plan)
    assert total == sum(n for _, n in images.preservation_regions(plan)) and abs(seconds - 2.0 * total / 550000) < 1e-6
    assert images.require_budget(plan, None) == (total, seconds) and images.require_budget(plan, seconds + 1) == (total, seconds)
    refused(lambda: images.require_budget(plan, seconds - 1), '%.0f s' % seconds, 'allows %.0f s' % (seconds - 1), 'scratch')
    assert abs(layout.preserve_seconds(100 * MIB) - 2.0 * 100 * MIB / 550000) < 1e-6

    # The scratch attribute: packed, honoured by the backup list, refused on an input.
    base = {'name': 'scr', 'devices': {'required': [], 'optional': []},
            'cpu': {'cluster': 'a55', 'workers': 0, 'priority': 2, 'stack_bytes': 4096},
            'deadlines': {'return_reserve_us': 1000000, 'progress_window_us': 1000000}, 'output_limit': 64}

    def manifest(regions):
        with tempfile.NamedTemporaryFile('w', suffix='.json') as handle:
            json.dump(dict(base, regions=regions), handle)
            handle.flush()
            return tool.load(handle.name)

    kept = {'name': 'kept', 'kind': 'work', 'access': ['read', 'write'], 'bytes': 8 * MIB}
    scratch = {'name': 'scr', 'kind': 'work', 'access': ['read', 'write', 'scratch'], 'bytes': 16 * MIB}
    placed = manifest([kept, scratch])
    tool.assign(placed)
    unpacked = tool.unpack(tool.pack(placed))[0]
    assert [bool(r['access'] & tool.ACCESS['scratch']) for r in unpacked['regions']] == [False, True]
    host.image_manifest = lambda image: {'manifest': unpacked, 'elf': None}
    host.usb_pool_geometry = lambda elf: None
    by_region = {n: (r['address'], r['bytes']) for n, r in zip(('kept', 'scr'), unpacked['regions'])}
    listed = host.backup_regions('image')
    assert listed == [{'address': by_region['kept'][0], 'bytes': 8 * MIB}], listed
    everything = host.backup_regions('image', scratch=True)
    assert sum(r['bytes'] for r in everything) == 24 * MIB, everything
    refused(lambda: manifest([dict(kept, kind='input', access=['read', 'scratch'])]), 'scratch', 'input')
    print('runner-preservation: passed')


if __name__ == '__main__':
    main()
