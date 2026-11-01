#!/usr/bin/env python3
"""Check cancellation admission and older finite decoders with real ELF inputs.

This parses images and immutable markers only. It opens no device and does not
execute the image or construct a launch authority record.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

CHECK_INPUTS = ('tools/board_images.py', 'apps/npu-finite/host.py',
                'apps/npu-interrupt/host.py', 'apps/os/host.py')
ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('active-elf', 'active-manifest', 'legacy-elf', 'legacy-manifest', 'output'):
        p.add_argument('--' + name, type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(ROOT/'tools'))
    import board_images as images
    host = images.load('cancel_profile_records', ROOT/'apps/os/host.py')
    finite = images.load('cancel_profile_finite', ROOT/'apps/npu-interrupt/host.py').finite
    active = json.loads(a.active_manifest.read_text())
    legacy = json.loads(a.legacy_manifest.read_text())
    rows = []

    def check(name, image, decoder, manifest, expected=None):
        try:
            result = images.finite_npu_info(image, host, decoder, manifest)
        except ValueError as error:
            assert expected is not None and str(error) == expected, (name, str(error))
        else:
            assert expected is None, name
            rows.append(dict(name=name, execution_profile=result['execution_profile'], passed=True))
            return result
        rows.append(dict(name=name, refusal=expected, passed=True))

    facts = check('active-controller-recovery', a.active_elf, finite, active)
    assert facts['execution_profile'] == finite.ACTIVE_CANCEL
    assert facts['geometry']['active_cancel'] and facts['geometry']['startup_only']
    assert not facts['geometry'].get('power_cycle')
    check('ordinary-runtime', a.legacy_elf, finite, legacy)

    class FrozenDecoder:
        def __init__(self, replacement=None):
            self.replacement = replacement
        def __getattr__(self, name):
            if name == 'ACTIVE_CANCEL':
                if self.replacement is None:
                    raise AttributeError(name)
                return self.replacement
            return getattr(finite, name)

    check('older-decoder-runtime', a.legacy_elf, FrozenDecoder(), legacy)
    check('older-decoder-cannot-claim-cancellation', a.active_elf, FrozenDecoder(), active,
          'Unsupported finite NPU profile.')
    check('unknown-extension-name', a.active_elf, FrozenDecoder('npu-other-extension'), active,
          'Unsupported finite NPU profile.')

    marker = struct.pack('<6Q', 0x4e505543414e3031, 32768, 262160, 524288, 0x189000, 0xe010a000)
    data = bytearray(a.active_elf.read_bytes())
    assert data.count(marker) == 1
    offset = data.index(marker)
    struct.pack_into('<Q', data, offset+8, 32767)
    changed = a.output/'wrong-wait-count.elf'
    changed.write_bytes(data)
    check('wrong-owned-command-marker', changed, finite, active,
          'Active cancellation implementation or fixed command layout differs.')
    inputs = [a.active_elf, a.active_manifest, a.legacy_elf, a.legacy_manifest]
    result = dict(physical_access=False, cases=rows,
                  inputs={str(p): images.sha(p) for p in inputs},
                  sources={name: images.sha(ROOT/name) for name in CHECK_INPUTS})
    (a.output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print('PASS', len(rows), 'finite cancellation and legacy profile cases')


if __name__ == '__main__':
    main()
