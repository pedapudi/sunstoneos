#!/usr/bin/env python3
"""Check a built native HEVC image against host preparation's ELF rules.

Runs apps/hevc/prepare.py's image reader on sunstone-a733-hevc.elf: entry
point, load segments inside the image and extended-memory reservations,
linker-owned pool and contract symbols, BSS below the session page, and the
unique immutable geometry and build-ID markers. The manifest must name the
same target, execution profile and final ELF digest. Preparation itself still
refuses until physical allocation and page-table evidence exists.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--image',type=Path,required=True,help='sunstone-a733-hevc.elf')
    p.add_argument('--manifest',type=Path,required=True,help='hevc-application/manifest.json')
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    spec=importlib.util.spec_from_file_location('hevc_image_preparation',ROOT/'apps/hevc/prepare.py')
    prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
    data=a.image.read_bytes();digest=hashlib.sha256(data).hexdigest()
    manifest=json.loads(a.manifest.read_text())
    assert manifest.get('target')=='sunstone-a733-hevc' and manifest.get('execution_profile')=='hevc-supervised-v1'
    assert manifest.get('final_elf')==digest
    parts,address,identity=prepare._image_parts(data)
    assert identity.hex()==manifest['build_id']
    result=dict(passed=True,physical_access=False,image_sha256=digest,build_id=identity.hex(),build_id_address=hex(address),
        segments=[dict(address=hex(x['address']),memory_bytes=x['memory_bytes'],file_bytes=len(x['data'])) for x in parts],
        scope='Static ELF and manifest rules of apps/hevc/prepare.py; no preparation bundle, launcher or execution.')
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS HEVC image geometry, reservations and build identity match host preparation')


if __name__=='__main__':main()
