#!/usr/bin/env python3
"""Read actual ELF contract metadata and refuse malformed/absent new profiles."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('os_link', ROOT / 'checks/os-image-link.py')
link = importlib.util.module_from_spec(spec); spec.loader.exec_module(link)

def refused(action):
    try: action()
    except AssertionError: return
    raise AssertionError('expected refusal')

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--legacy', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    raw, old = a.image.read_bytes(), a.legacy.read_bytes()
    image = link.Elf(raw)
    actual = link.contract_profile(image)
    assert link.contract_profile(link.Elf(old), legacy=True) is None
    refused(lambda: link.contract_profile(link.Elf(old)))
    record = next(s for s in image.symbols if s['name'] == 'os_contract_profile')
    cases = ['actual-profile', 'legacy-explicit', 'legacy-strict-refusal']
    for name, change in [('size', {'size': 8}), ('local', {'bind': 0}), ('absent', {'shndx': 0})]:
        copy = link.Elf(raw)
        next(s for s in copy.symbols if s['name'] == 'os_contract_profile').update(change)
        refused(lambda: link.contract_profile(copy)); cases.append(name)
    duplicate = link.Elf(raw); duplicate.symbols.append(dict(record))
    refused(lambda: link.contract_profile(duplicate)); cases.append('duplicate')
    for name, offset, value in [('unknown-magic', 0, 0), ('unknown-session', 8, 809)]:
        section = next(s for s in image.sections if s[1] == 1 and s[3] <= record['value'] < s[3] + s[5])
        data = bytearray(raw)
        struct.pack_into('<Q', data, section[4] + record['value'] - section[3] + offset, value)
        refused(lambda: link.contract_profile(link.Elf(data))); cases.append(name)
    result = {'passed': True, 'profile': actual, 'cases': cases,
              'image_sha256': hashlib.sha256(raw).hexdigest(), 'legacy_sha256': hashlib.sha256(old).hexdigest(),
              'sources': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in (Path(__file__), ROOT / 'checks/os-image-link.py', ROOT / 'apps/os/admission.c')}}
    a.output.write_text(json.dumps(result, indent=2) + '\n')
    print('PASS', len(cases), 'linked contract profile cases')

if __name__ == '__main__': main()
