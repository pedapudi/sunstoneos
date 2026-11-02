#!/usr/bin/env python3
"""Check linked pool ownership, frozen receipt decoder and native result envelope."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
from harness import save,refused
ROOT=Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps/os/cpu-return-host.py','apps/os/host.py','apps/cpu-npu-concurrency/host.py','apps/cpu-npu-concurrency/vectors.py','apps/cpu-npu-concurrency/manifest.json']
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
p=argparse.ArgumentParser(description=__doc__);p.add_argument('elf',type=Path);p.add_argument('--native-result',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
returns=module('cpu_npu_geometry_return',ROOT/'apps/os/cpu-return-host.py')
receipt=module('cpu_npu_geometry_receipt',ROOT/'apps/cpu-npu-concurrency/host.py')
geometry=returns.geometry(a.elf)
pool=geometry['npu_pool']
# The pool is the one the image links: valid by the host rule (legacy 8 MiB, or sized and ending at 0x47000000) and equal to its symbols.
assert returns.npu_pool_extent_valid(pool['address'],pool['address']+pool['bytes']) and geometry['npu_stop']['bytes']==104
results=[dict(case='exact-symbol-derived-pool-and-stop',passed=True,pool=pool)]
symbols=returns.linked_symbols(a.elf);original_symbols=returns.linked_symbols
for case in ['partial-pool','wrong-end','missing-stop','stop-outside-image','monitor-collision','pool-ends-above-top','pool-too-big']:
 changed=dict(symbols)
 if case=='partial-pool':changed.pop('__os_npu_pool_end')
 if case=='pool-ends-above-top':changed['__os_npu_pool_start']=(0x45c00000,0);changed['__os_npu_pool_end']=(0x47200000,0)
 if case=='pool-too-big':changed['__os_npu_pool_start']=(0x43e00000,0);changed['__os_npu_pool_end']=(0x47000000,0)
 if case=='wrong-end':changed['__os_npu_pool_end']=(0x47a00000,0)
 if case=='missing-stop':changed.pop('os_npu_published_report')
 if case=='stop-outside-image':changed['os_npu_published_report']=(0x47400000,104)
 if case=='monitor-collision':changed['__monitor_bss_end']=(0x47500000,0)
 returns.linked_symbols=lambda path,changed=changed:changed
 try:message=refused(lambda:returns.geometry(a.elf),'')
 finally:returns.linked_symbols=original_symbols
 results.append(dict(case=case,passed=True,refusal=message))
# A pool sized from the declaration: 22 MiB ending at 0x47000000. The geometry is derived from the symbols, the backup extent
# is the whole pool, and an application region inside it is refused.
sized=dict(symbols);sized['__os_npu_pool_start']=(0x45a00000,0);sized['__os_npu_pool_end']=(0x47000000,0)
returns.linked_symbols=lambda path:sized
try:
 grown=returns.geometry(a.elf)
 assert grown['npu_pool']==dict(address=0x45a00000,bytes=22<<20),grown['npu_pool']
 results.append(dict(case='sized-pool-22-MiB',passed=True,pool=grown['npu_pool']))
 original_manifest=returns.host.image_manifest;manifest=original_manifest(a.elf);manifest['manifest']['regions'].append(dict(address=0x45a01000,bytes=4096,kind=1))
 returns.host.image_manifest=lambda image,manifest=manifest:manifest
 try:message=refused(lambda:returns.geometry(a.elf),'overlaps')
 finally:returns.host.image_manifest=original_manifest
 results.append(dict(case='region-inside-sized-pool',passed=True,refusal=message))
finally:returns.linked_symbols=original_symbols
original_manifest=returns.host.image_manifest
for kind in [1,2,3]:
 manifest=original_manifest(a.elf);manifest['manifest']['regions'].append(dict(address=0x47401000,bytes=4096,kind=kind))
 returns.host.image_manifest=lambda image,manifest=manifest:manifest
 try:message=refused(lambda:returns.geometry(a.elf),'overlaps')
 finally:returns.host.image_manifest=original_manifest
 results.append(dict(case=f'overlapping-region-kind-{kind}',passed=True,refusal=message))
wrapper=a.out/'decoder.py';binding=receipt.freeze_decoder(a.elf,wrapper,1)
decoder=module('cpu_npu_frozen_decoder',wrapper)
# Session identities are fixed by the native integration fixture, independent
# of the copied producer record's own identity fields.
payload=returns.host.decode_result(a.native_result.read_bytes(),4660,9,1728)
validated=decoder.decode(payload);assert validated['jobs']==4 and validated['overlap_mask']==15
results.append(dict(case='native-producer-envelope-to-frozen-decode',passed=True,binding=binding))
malformed=bytearray(payload);malformed[0x80+56:0x80+64]=bytes(8)
message=refused(lambda:decoder.decode(malformed),'acknowledgment')
results.append(dict(case='frozen-decoder-rejects-malformed-receipt',passed=True,refusal=message))
for name,raw in [('short',a.native_result.read_bytes()[:-1]),('long',a.native_result.read_bytes()+b'\0')]:
 message=refused(lambda raw=raw:returns.host.decode_result(raw,4660,9,1728),'')
 results.append(dict(case='native-envelope-'+name,passed=True,refusal=message))
save(a.out/'results.json',dict(physical_access=False,image_sha256=hashlib.sha256(a.elf.read_bytes()).hexdigest(),
 native_result_sha256=hashlib.sha256(a.native_result.read_bytes()).hexdigest(),
 sources={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in CHECK_INPUTS},cases=results))
print('PASS',len(results),'CPU/NPU geometry and frozen-decoder cases')
