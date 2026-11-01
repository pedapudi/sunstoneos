#!/usr/bin/env python3
"""Verify the three finite image artifacts, watchdog markers and run instructions."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
PROFILES = {'startup-only': ('npu-startup-return-v1', 0),
            'single-inference': ('npu-single-inference-return-v1', 1),
            'runtime': ('npu-runtime-return-v1', 2)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--images', type=Path, required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('npu_image_host', ROOT / 'apps/npu-interrupt/host.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    rows = []
    for name, (profile, jobs) in PROFILES.items():
        directory = args.images / name
        data = (directory / 'image.elf').read_bytes()
        manifest = json.loads((directory / 'manifest.json').read_bytes())
        digest = hashlib.sha256(data).hexdigest()
        assert manifest['execution_profile'] == profile and manifest['final_elf'] == digest
        assert host.finite.watchdog_marker(data) == {'mode': 0xb1, 'session_seconds': 20}
        host.finite.image_profile(data, manifest)
        readme = (directory / 'README.md').read_text()
        binary_hash = hashlib.sha256((directory / 'image.bin').read_bytes()).hexdigest()
        for expected in (digest, binary_hash, 'run-ddr.py', 'dev-run.py npu', '0x0601',
                         '0x06ff', 'failure', f'inference_jobs={jobs}'):
            assert expected in readme, (name, expected)
        ddr_command = next(line for line in readme.splitlines() if 'tools/run-ddr.py --' in line)
        assert ' --fel ' in ddr_command, 'DRAM initialization requires its explicit FEL executable'
        rows.append(dict(name=name, elf_sha256=digest, binary_sha256=binary_hash))
    (args.images / 'image-check.json').write_text(json.dumps(dict(passed=True,
        images=rows, physical_access=False), indent=2) + '\n')
    print('PASS three finite NPU images: exact manifests, watchdog markers, hashes and run instructions')


if __name__ == '__main__':
    main()
