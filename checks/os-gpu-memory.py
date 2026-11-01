#!/usr/bin/env python3
"""Check native CPU output permissions derived from the linked GPU manifest.

Runs the actual EL3 table builder with modeled system registers and cache
maintenance. Manifest GPU outputs must be CPU read-only, and CPU inputs and
work areas must remain writable. A relocated output verifies the table builder
uses the manifest's address instead of a fixed application address.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
from harness import Image

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('cpu_tables',ROOT/'checks/cpu-table-walks.py')
tables=importlib.util.module_from_spec(spec);spec.loader.exec_module(tables)
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--image',type=Path,nargs='+',required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();results=[]
for path in a.image:
    image=Image(path,['ss_application','ss_manifest','ss_region'])
    machine=tables.prepared(image)
    manifest=machine.read(image.address('ss_application')+image.layouts['ss_application']['manifest'])
    count=int.from_bytes(machine.uc.mem_read(manifest+image.layouts['ss_manifest']['region_count'],1),'little')
    base=manifest+image.layouts['ss_manifest']['regions'];regions=[]
    for i in range(count):
        address=base+i*56
        region=dict(address=machine.read(address),bytes=machine.read(address+8),
                    access=int.from_bytes(machine.uc.mem_read(address+20,4),'little'),record=address)
        regions.append(region)
    root=image.address('a733_cached_el1_l2')
    def access(m,address):return (m.read(root+8*((address-0x40000000)>>21))>>6)&3
    for region in regions:
        expected=3 if region['access']&8 and not region['access']&2 else 1
        for address in range(region['address']&~0x1fffff,(region['address']+region['bytes']+0x1fffff)&~0x1fffff,0x200000):
            assert access(machine,address)==expected,(str(path),hex(address),expected,access(machine,address))
    output=next(r for r in regions if r['access']&8 and not r['access']&2)
    relocated=tables.Machine(image)
    relocated.write(output['record'],0x47200000)
    assert relocated.call('a733_cached_memory_prepare')==1
    for address in range(0x47200000,(0x47200000+output['bytes']+0x1fffff)&~0x1fffff,0x200000):
        assert access(relocated,address)==3
    assert access(relocated,output['address'])==1
    results.append(dict(image=str(path),image_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        regions=regions,relocated_output=0x47200000,passed=True))
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(json.dumps(dict(physical_access=False,cases=results,scope=__doc__),indent=2)+'\n')
print('PASS native GPU manifest CPU mappings:',len(results),'images')
