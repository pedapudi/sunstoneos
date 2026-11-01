#!/usr/bin/env python3
"""Check negotiated x1 speeds through all three production PCIe decoders.

Inputs are copied frames from linked-image models. Rehashing a changed link
field isolates admission of link status from payload-integrity admission.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from harness import refused


CHECK_INPUTS = ('apps/pcie/observe.py', 'apps/pcie-test/observe.py', 'apps/pcie-nvme/observe.py')

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT)
    for name in ('pcie', 'pcie-test', 'pcie-nvme'):
        p.add_argument('--' + name, type=Path, nargs=2, required=True, metavar=('FRAME', 'SESSION'))
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=False)
    results = {}
    for name, message in (
            ('pcie', 'PCIe identity/link/command differs'),
            ('pcie-test', 'Endpoint identity/configuration differs'),
            ('pcie-nvme', 'NVMe endpoint identity or read target differs')):
        path = a.source / 'apps' / name / 'observe.py'
        spec = importlib.util.spec_from_file_location(name.replace('-', '_') + '_decode', path)
        decoder = importlib.util.module_from_spec(spec); spec.loader.exec_module(decoder)
        frame_path, session_path = getattr(a, name.replace('-', '_'))
        frame = frame_path.read_bytes(); session = session_path.read_bytes()
        assert decoder.decode(frame, session)['status'] == 1
        cases = {}
        for link in (0x11, 0x12, 0x13, 0x10, 0x14, 0x21, 0x213):
            changed = bytearray(frame); struct.pack_into('<H', changed, 128 + 22, link)
            changed[64:96] = hashlib.sha256(changed[128:]).digest()
            if link in (0x11, 0x12, 0x13):
                assert decoder.decode(changed, session)['status'] == 1
            else:
                refused(lambda: decoder.decode(changed, session), message)
            cases[hex(link)] = 'accepted' if link in (0x11, 0x12, 0x13) else 'refused'
        results[name] = dict(cases=cases, decoder_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                             frame_sha256=hashlib.sha256(frame).hexdigest(),
                             session_sha256=hashlib.sha256(session).hexdigest())
    (a.output / 'results.json').write_text(json.dumps(dict(passed=True, physical_access=False, decoders=results), indent=2) + '\n')
    print('PASS x1 Gen1/Gen2/Gen3 admission and invalid speed/width refusals in all three PCIe decoders')


if __name__ == '__main__':
    main()
