#!/usr/bin/env python3
"""Check CPU reference framing, neutral endpoints, Bayer phase and mismatch reporting."""
import argparse
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location('isp0_cpu_reference', Path(__file__).with_name('isp0-pixel-reference.py'))
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    cases = {}
    frames = {}
    for name, pattern in (('black', b'\0\0'), ('white', b'\xff\x0f'),
                          ('red', None), ('blue', None)):
        if pattern is None:
            raw = bytearray(reference.PLANE * 2)
            for y in range(480):
                for x in range(640):
                    if (y % 2, x % 2) == ((1, 1) if name == 'red' else (0, 0)):
                        raw[(y * 640 + x) * 2:(y * 640 + x) * 2 + 2] = b'\xff\x0f'
        else:
            raw = pattern * reference.PLANE
        nv16, report = reference.reference(raw, args.output / name, 'limited')
        assert reference.compare(nv16, nv16, 0)['passed']
        frames[name] = nv16
        if name in ('black', 'white'):
            assert set(nv16[:reference.PLANE]) == {16 if name == 'black' else 235}
            assert set(nv16[reference.PLANE:]) == {128}
        else:
            expected = (81, 90, 240) if name == 'red' else (41, 240, 110)
            assert set(nv16[:reference.PLANE]) == {expected[0]}
            assert set(nv16[reference.PLANE::2]) == {expected[1]}
            assert set(nv16[reference.PLANE + 1::2]) == {expected[2]}
        cases[name] = dict(passed=True, reference_sha256=report['reference_sha256'])
    assert not reference.compare(frames['red'], frames['blue'], 0)['passed']
    changed = bytearray(frames['black'])
    changed[17] += 1
    changed[reference.PLANE + 3] += 2
    comparison = reference.compare(frames['black'], changed, 0)
    assert not comparison['passed'] and comparison['planes']['Y']['mismatched'] == 1
    assert comparison['planes']['U']['mismatched'] == 0 and comparison['planes']['V']['mismatched'] == 1
    for raw in (bytes(4), b'\0\x10' * reference.PLANE):
        try:
            reference.reference(raw, args.output / 'invalid', 'limited')
        except ValueError:
            pass
        else:
            raise AssertionError('Malformed Bayer input accepted')
    report = dict(status='pass', cases=cases, mismatch_report=comparison,
                  physical_access=False, numerical_qualified=False)
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS independent CPU Bayer/NV16 reference and mismatch reporting')


if __name__ == '__main__':
    main()
