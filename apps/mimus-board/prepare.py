#!/usr/bin/env python3
"""Prepare a fixed application using the reviewed board launcher and DDR entry.

No hardware access. Fresh board admission is required; historical preparation
records cannot authorize this image. The wrapper retains the board's own bundle
unchanged and adds only the three exact input uploads plus their DDR evidence.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
board=module('mimus_board_preparation',ROOT/'apps/board/prepare.py')
host=module('mimus_board_host',ROOT/'apps/mimus-board/host.py')
board.PROFILES['sunstone-a733-mimus-board']='board-supervised-v2'
require=host.require
RANGES=[[0x44000000,0x45000000],[0x45000000,0x45100000],[0x46000000,0x47000000]]
def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def input_admission(record,image,read):
    value=json.loads(read(record['mimus_ddr']))
    require(value.get('schema')=='a733-ddr-reservation-v1' and value.get('identity')==record['identity'] and
        value.get('port')==record['port'] and value.get('image_sha256')==image and value.get('exclusive') is True and
        value.get('published_cache_disabled') is True and value.get('remote_writers_excluded') is True and
        value.get('ranges')==RANGES and value.get('evidence'),'Fixed input/arena DDR admission differs.')
    for item in value['evidence']:read(item)
def prepare(image,manifest,admission,template,assets,output,clang,lld):
    image,manifest,admission,output=map(Path,(image,manifest,admission,output));data=json.loads(manifest.read_text())
    require(data.get('target')=='sunstone-a733-mimus-board' and data.get('application')=='recorded-mimus-with-read-only-usb-storage-v2' and data.get('usb2_combined') is True and data.get('keyboard_trigger')=='boot-neutral-enter-v1',
        'Mimus board image manifest required.')
    require(data.get('models')==json.loads((ROOT/'apps/bluetooth/recognition-models.json').read_text()) and
        data.get('recorded_input')==json.loads((ROOT/'apps/mimus-board/inputs.json').read_text()),'Compiled input declarations differ.')
    record=json.loads(admission.read_text());cache={}
    def evidence(item):
        require(set(item)=={'path','sha256'} and Path(item['path']).is_absolute(),'Invalid evidence reference.')
        blob=Path(item['path']).read_bytes();require(hashlib.sha256(blob).hexdigest()==item['sha256'],'Evidence changed.')
        cache[item['path']]=blob;return blob
    input_admission(record,digest(image),evidence)
    output.mkdir(parents=True,exist_ok=False)
    base=board.prepare(image,manifest,admission,template,output/'board',clang,lld)
    require(base['workload'] in (0,4),'Only storage or keyboard/storage workload admitted.')
    fixed=host.freeze_assets(assets,output/'assets')
    refs={}
    for i,(name,blob) in enumerate(cache.items()):
        target=f'input-evidence-{i}.bin';(output/target).write_bytes(blob)
        refs[name]={'name':target,'sha256':hashlib.sha256(blob).hexdigest()}
    value={'schema':'mimus-board-supervised-bundle-v1','physical_access':False,
        'board_prepared_sha256':digest(output/'board/prepared.json'),'assets_sha256':digest(output/'assets/assets.json'),
        'input_evidence':refs,'uploads':[{'directory':'board',**item} for item in base['segments']]+[{'directory':'assets','name':item['name'],'address':item['address']} for item in fixed['assets']]}
    (output/'prepared.json').write_text(json.dumps(value,indent=2)+'\n');return validate_bundle(output)
def validate_bundle(output,*,capture_identity=False):
    output=Path(output);value=json.loads((output/'prepared.json').read_text())
    require(value.get('schema')=='mimus-board-supervised-bundle-v1' and value.get('physical_access') is False,'Bundle schema differs.')
    require(digest(output/'board/prepared.json')==value['board_prepared_sha256'] and digest(output/'assets/assets.json')==value['assets_sha256'],'Bundle identity differs.')
    base=board.validate_bundle(output/'board');assets=host.validate_assets(output/'assets')
    require(base['workload'] in (0,4),'Only storage or keyboard/storage workload admitted.')
    def evidence(item):
        retained=value['input_evidence'][item['path']];require(retained['sha256']==item['sha256'] and Path(retained['name']).name==retained['name'],'Evidence identity differs.')
        blob=(output/retained['name']).read_bytes();require(hashlib.sha256(blob).hexdigest()==item['sha256'],'Evidence changed.');return blob
    input_admission(json.loads((output/'board/admission.json').read_text()),base['image_sha256'],evidence)
    require(value['uploads']==[{'directory':'board',**item} for item in base['segments']]+[{'directory':'assets','name':item['name'],'address':item['address']} for item in assets['assets']], 'Upload plan differs.')
    return {key:base[key] for key in ('epoch','generation','workload')} if capture_identity else value
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','session-template','base','overlay','wav','output','clang','lld'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();prepare(a.image,a.manifest,a.admission,a.session_template,[a.base,a.overlay,a.wav],a.output,a.clang,a.lld)
