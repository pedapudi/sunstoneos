#!/usr/bin/env python3
"""Strict acceptance for the read-only output cache diagnostic's frozen capture."""
import hashlib
import json
from pathlib import Path
import re
EXPECTED = 'COMPLETE GPU output cache permission: EL1 RO, zero tags, CIVAC 4MiB, bytes preserved'
OWNED_RAM = (0x41800000, 0x400000)
def validate_bundle(bundle):
    bundle = Path(bundle)
    record = json.loads((bundle / 'build.json').read_text())
    if record.get('schema') != 'gpu-cache-permission-v1' or record.get('expect') != EXPECTED:
        raise ValueError('permission image profile')
    for name, key in [('image.elf', 'image_sha256'), ('probe.bin', 'binary_sha256')]:
        if hashlib.sha256((bundle / name).read_bytes()).hexdigest() != record.get(key):
            raise ValueError('permission image identity')
    span = record.get('logical_output', {})
    if record.get('extra_owned_ram') != [] or (span.get('address'), span.get('bytes')) != OWNED_RAM:
        raise ValueError('permission output ownership')
    for name, expected in record.get('host_files', {}).items():
        if hashlib.sha256((bundle / name).read_bytes()).hexdigest() != expected:
            raise ValueError('permission frozen host identity')
    return record

def validate_result(result, log):
    if tuple(result.get(k) for k in ('magic', 'stage', 'status')) != (0x534b4552, 5, 0):
        raise ValueError('permission terminal status')
    lines = log.splitlines()
    if any(line.startswith('FAIL ') for line in lines) or lines.count(EXPECTED) != 1:
        raise ValueError('permission completion report')
    timing = [line for line in lines if line.startswith('GPU output cache ticks=')]
    if len(timing) != 1 or not re.fullmatch(r'GPU output cache ticks=0x[0-9a-f]{16}', timing[0]):
        raise ValueError('permission timing report')
    return {'accepted': True, 'maintained_bytes': OWNED_RAM[1], 'cache_ticks': int(timing[0].split('=')[1], 16),
            'scope': 'EL1 read-only cache maintenance and initialization; no GPU DMA or recognition proof'}

def main(argv=None):
    import argparse
    import importlib.util
    import sys
    argv = sys.argv[1:] if argv is None else argv
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--mode', default='kernel')
    parser.add_argument('--expect', default=EXPECTED)
    args, _ = parser.parse_known_args(argv)
    bundle = Path(__file__).resolve().parents[2]
    record = validate_bundle(bundle)
    if hashlib.sha256(args.image.read_bytes()).hexdigest() != record['binary_sha256']:
        raise ValueError('permission runtime image identity')
    if args.mode != 'kernel' or args.expect != EXPECTED or any(
            value.startswith('--') and any(option.startswith(value.split('=')[0]) for option in
                ('--controlled', '--ram-input', '--ram-input-present')) for value in argv):
        raise ValueError('permission host mode')
    path = Path(__file__).with_name('returning-kernel.py')
    spec = importlib.util.spec_from_file_location('permission_returning_kernel', path)
    runner = importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)
    return runner.main(list(argv) + ['--expect', EXPECTED], validate_result=validate_result)
if __name__ == '__main__': main()
