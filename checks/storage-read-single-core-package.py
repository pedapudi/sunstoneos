#!/usr/bin/env python3
"""Check truthful single-core producer packaging through the existing host path."""
import argparse, hashlib, importlib.util, json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result
def run(image,manifest,fixture,out,clang,lld):
    out.mkdir(parents=True,exist_ok=False)
    host=module('single_core_storage_host',ROOT/'tools/check-a733-storage-read-supervised.py')
    prep=host.preparation;body=image.read_bytes();record=json.loads(manifest.read_text())
    prep.image_manifest_check(record,body)
    refused=[]
    for name,changes in [('old-profile',{'execution_profile':'storage-read-supervised-v2'}),
                          ('old-target',{'target':'sunstone-a733-storage-read','execution_profile':'storage-read-supervised-v2'}),
                          ('wrong-image',{'final_elf':'00'*32})]:
        try:prep.image_manifest_check({**record,**changes},body)
        except ValueError:refused.append(name)
        else:raise AssertionError(name)
    bundle=out/'MODEL-NOT-PHYSICAL-bundle'
    prep.prepare(image,manifest,fixture/'admission.json',fixture/'session-template.bin',bundle,clang,lld)
    frozen=host.freeze_bundle(bundle)
    assert frozen['_frozen']['image.elf']==body
    actual=json.loads(frozen['_frozen']['image-input-manifest.json'])
    assert actual['target']=='sunstone-a733-storage-read-single-core'
    assert actual['execution_profile']=='storage-read-single-core-supervised-v2'
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,
      image_sha256=hashlib.sha256(body).hexdigest(),refused=refused,
      prepared_sha256=frozen['_prepared_sha256'],launcher_sha256=frozen['files']['launch.bin'],
      scope='Actual single-core manifest identity, full original authority/session preparation, launcher compilation, bundle validation and host freeze. Modeled authority never authorizes physical use; native read/USB completion behavior is covered separately.'),indent=2)+'\n')
    print('PASS single-core storage producer and host package')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','fixture','output','clang','lld'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();run(a.image,a.manifest,a.fixture,a.output,a.clang,a.lld)
