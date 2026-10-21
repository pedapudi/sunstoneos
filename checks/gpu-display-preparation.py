#!/usr/bin/env python3
"""Check offline GPU/display preparation with explicit modeled physical authority."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
from unittest.mock import patch
if not __debug__:raise SystemExit('checks require assertions; unset PYTHONOPTIMIZE and do not pass -O')
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
p=module('display_preparation_check',ROOT/'apps/gpu-display/prepare.py')
def refused(call):
    try:call()
    except (ValueError,RuntimeError,KeyError,struct.error) as error:return str(error)
    raise AssertionError('Invalid preparation admitted')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('image','root-session','provider-session','manifest','producer-inputs','mesa-archive','firmware','firmware-license','output','clang','lld','objcopy'):ap.add_argument('--'+name,type=Path,required=True)
    ap.add_argument('--case',action='append',choices=('refusal','bundle','linked-input','provenance','usb'))
    a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True);image=a.image.read_bytes();parts,address,build=p.image_data(image)
    result_path=a.output/'result.json';results=json.loads(result_path.read_text()) if result_path.exists() else {'physical_access':False,'image_sha256':p.sha(image),'cases':{}}
    assert results['image_sha256']==p.sha(image),'Refuse stale image evidence'
    selected=a.case or ['refusal','bundle','usb']
    def passed(name,value):results['cases'][name]=value;p.save_json(result_path,results);print('PASS',name,flush=True)
    raw=a.root_session.read_bytes();data=raw[p.INPUT_OFFSET:];p.validate_inputs(data)
    hdmi=bytearray(a.provider_session.read_bytes()[24:568]);hdmi[48:80]=build;profile=p.session_profile(hdmi)
    if 'refusal' in selected and 'refusal' not in results['cases']:
        (a.output/'hdmi.bin').write_bytes(hdmi);(a.output/'inputs.bin').write_bytes(data)
        p.save_json(a.output/'synthetic-admission.json',{'schema':p.ADMISSION,'physical_access':False,'reviewed':True})
        refused(lambda:p.prepare(a.image,a.manifest,a.output/'hdmi.bin',a.output/'inputs.bin',a.output/'synthetic-admission.json',a.output/'must-not-create',None,None,None))
        assert not (a.output/'must-not-create').exists()
        legacy=bytearray(raw);struct.pack_into('<Q',legacy,8,1);refused(lambda:p.request_bytes(address,build,bytes(legacy),*struct.unpack_from('<3Q',raw,48)))
        for offset in (328,336+11*4,392):
            bad=bytearray(hdmi);bad[offset]=0;refused(lambda:p.session_profile(bad))
        bad=bytearray(data);bad[48:80]=bytes(32);refused(lambda:p.validate_inputs(bytes(bad)))
        request,contract=p.request_bytes(address,build,raw,*struct.unpack_from('<3Q',raw,48))
        assert len(request)==128 and len(contract)==4096 and struct.unpack_from('<3Q',contract)==(0,2,288)
        assert contract[224:256]==bytes.fromhex(p.sha(raw)) and contract[256:288]==raw[104:136]
        assert struct.unpack_from('<6Q',contract,176)==(1,struct.unpack_from('<Q',raw,696)[0],0x48000000,0x78000000,0x78000000,0x79000000)
        passed('refusal',{'passed':True,'synthetic_production_refused_before_output':True,'invalid_provider_handoff_refused':True,'input_digest_refused':True,'exact_contract_layout':True,'scope':'Actual ELF/DWARF and producer input geometry, host refusal and contract serialization. No launcher or native image execution.'})
    if 'bundle' in selected and 'bundle' not in results['cases']:
        fixtures=a.output/'MODELED-inputs';fixtures.mkdir(exist_ok=True)
        def ref(path):return {'path':str(path.resolve()),'sha256':p.digest(path)}
        def put(name,record):
            path=fixtures/name;p.save_json(path,record);return ref(path)
        common={**profile,'identity':'MODELED-NOT-A-BOARD','port':'MODELED-NOT-A-PORT','image_sha256':p.sha(image),
            'memory_authority':struct.unpack_from('<Q',raw,696)[0],
            'usb_owner':struct.unpack_from('<Q',raw,704)[0],'usb_handoff':struct.unpack_from('<Q',raw,712)[0],
            'usb_cold_reference':2,'usb_supply_uv':list(p.USB_SUPPLIES),'combined_lifetime':True,'reviewed':True,'physical_access':False}
        outer={**common,'schema':p.ADMISSION,'execution_profile':p.PROFILE,'session_seconds':20,'inputs_sha256':p.sha(data)}
        proof=put('MODELED-proof.json',{'physical_access':False,'scope':'Parser fixture; no physical authority'})
        outer['ddr']=put('MODELED-ddr.json',{**common,'schema':'a733-ddr-reservation-v1','ranges':p.RANGES,'exclusive':True,'published_cache_disabled':True,'remote_writers_excluded':True,'evidence':[proof]})
        for name,conditions in p.CONDITIONS.items():
            fact={**common,'schema':'a733-gpu-display-'+name.replace('_','-')+'-v1','conditions':{k:proof for k in conditions}}
            if name=='analog':fact['request_sha256']=p.sha(hdmi[232:304])
            if name=='power':fact['pmic_pins_sha256']=p.sha(hdmi[304:440])
            if name.startswith('usb_'):fact.update(session_seconds=20)
            if name=='usb_initial':fact.update(cold_reference=2,irq_ids=[188,189,190])
            if name=='usb_electrical':fact.update(supply_uv=list(p.USB_SUPPLIES),fabric_sha256=p.sha(hdmi[512:536]),pmic_pins_sha256=p.sha(hdmi[304:440]),dedicated_sink=True)
            if name=='usb_supervision':fact['one_attempt']=True
            if name in ('memory','gpu_memory'):fact.update(ranges=p.RANGES,display_parent_bytes=4194304)
            if name=='memory':fact.update(display_master=8,gpu_system_iommu_master=None,display_bank_bytes=3739648,framebuffer_snapshot_sha256=p.sha(hdmi[504:544]))
            outer[name]=put('MODELED-'+name+'.json',fact)
            if name in ('analog','power','supervision','isolation','memory'):
                at={'analog':112,'power':144,'supervision':176,'isolation':440,'memory':472}[name];hdmi[at:at+32]=bytes.fromhex(outer[name]['sha256'])
        outer['firmware']=ref(a.firmware);outer['firmware_license']=ref(a.firmware_license)
        producer=next(v for v in json.loads(a.producer_inputs.read_text()) if v['path'].endswith('/checks/pvr-texture-producer.py'))
        program={'schema':'a733-gpu-display-trusted-programs-v1','reviewed':True,'profile':2,'image_sha256':p.sha(image),
            'source_sha256':p.digest(ROOT/'drivers/gpu/pvr_render_texture.c'),'producer_sha256':producer['sha256'],
            'mesa_revision':'86c7176d2dd0065555521bf8c61f208bf53c75d6','source':ref(ROOT/'drivers/gpu/pvr_render_texture.c'),
            'producer':producer,'compiler_source':ref(a.mesa_archive),'producer_inputs':ref(a.producer_inputs),
            'shaders':{name:ref(a.producer_inputs.parent/(name+'.bin')) for name in ('vertex','fragment')}}
        outer['programs']=put('MODELED-programs.json',program)
        entry=put('MODELED-admission.json',outer);entry_raw=Path(entry['path']).read_bytes();hdmi[80:112]=bytes.fromhex(p.sha(entry_raw))
        hdmi_path=fixtures/'hdmi.bin';hdmi_path.write_bytes(hdmi);input_path=fixtures/'inputs.bin';input_path.write_bytes(data)
        refused(lambda:p.validate_admission(entry_raw,bytes(hdmi),data,build,p.sha(image)))
        with patch.object(p,'physical_record',lambda record:None):
            prepared=p.prepare(a.image,a.manifest,hdmi_path,input_path,entry['path'],a.output/'MODELED-NOT-ADMITTED-bundle',a.clang,a.lld,a.objcopy)
            closure={name:prepared['_frozen'][v['name']] for name,v in prepared['entry_evidence'].items()}
            p.validate_admission(entry_raw,bytes(hdmi),data,build,p.sha(image),closure,True,image_raw=image)
            bad=bytearray(hdmi);bad[440]^=1;refused(lambda:p.validate_admission(entry_raw,bytes(bad),data,build,p.sha(image),closure,True,image_raw=image))
            bad_outer={**outer,'session_seconds':60};bad_entry=json.dumps(bad_outer).encode();bad=bytearray(hdmi);bad[80:112]=bytes.fromhex(p.sha(bad_entry))
            refused(lambda:p.validate_admission(bad_entry,bytes(bad),data,build,p.sha(image),closure,True,image_raw=image))
            bad_cache=dict(closure);bad_cache[outer['memory']['path']]+=b'x';refused(lambda:p.validate_admission(entry_raw,bytes(hdmi),data,build,p.sha(image),bad_cache,True,image_raw=image))
            metadata=a.output/'MODELED-NOT-ADMITTED-bundle/prepared.json';before=metadata.read_bytes();m=json.loads(before);m['payloads'][0]['address']+=64;p.save_json(metadata,m)
            refused(lambda:p.validate_bundle(metadata.parent));metadata.write_bytes(before)
        # A parser bundle that passed under the explicit fixture predicate must
        # still fail normal production revalidation with that predicate restored.
        refused(lambda:p.validate_bundle(a.output/'MODELED-NOT-ADMITTED-bundle'))
        passed('bundle',{'passed':True,'session_bytes':prepared['session_bytes'],'frozen_proofs':len(closure),'launcher_bytes':len(prepared['_frozen']['launch.bin']),
            'physical_predicate':'Only physical_record explicitly substituted for modeled positive fixture; all new binding/provenance/freeze checks execute. Ordinary production rejects the same bundle.',
            'cases':['prepare and frozen revalidation','HDMI isolation digest','wrong lifetime','proof tamper','upload tamper','production fixture refusal'],
            'scope':'Actual offline producer and ARM SRAM wrapper build; no SRAM execution, native rebuild, queue, USB or physical operation.'})
    if 'linked-input' in selected and 'linked-input' not in results['cases']:
        out=a.output/'linked-input';out.mkdir(exist_ok=False)
        source=out/'check.c';source.write_text('#include "input.h"\n#include <stdio.h>\n#include <stdlib.h>\nint main(int argc,char **argv){if(argc!=2)return 2;FILE *f=fopen(argv[1],"rb");if(!f)return 2;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);if(n<144||n>1048576)return 2;void *p=calloc(1,(size_t)n);if(!p||fread(p,1,(size_t)n,f)!=(size_t)n)return 2;fclose(f);int r=gpu_display_inputs_valid(p);free(p);return r?0:1;}\n')
        command=[str(a.clang),'-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-I'+str(ROOT/'include'),'-I'+str(ROOT/'apps/gpu-display'),str(source),str(ROOT/'apps/gpu-display/input.c'),str(ROOT/'drivers/gpu/pvr_render_texture.c'),str(ROOT/'drivers/storage/storage_digest.c'),'-Wl,--gc-sections','-o',str(out/'check')]
        p.save_json(out/'command.json',command);r=subprocess.run(command,capture_output=True,text=True);(out/'build.log').write_text(r.stdout+r.stderr);assert r.returncode==0
        path=out/'inputs.bin';path.write_bytes(data);r=subprocess.run([str(out/'check'),str(path)],capture_output=True,text=True);(out/'valid.log').write_text(r.stdout+r.stderr);assert r.returncode==0 and not r.stderr
        bad=bytearray(data);offset=struct.unpack_from('<I',bad,16+20)[0];struct.pack_into('<I',bad,offset,0x7fc00000)
        # Rehash the changed payload so rejection reaches the native vertex
        # contract rather than relying only on the envelope's digest check.
        size=struct.unpack_from('<I',bad,16+24)[0];bad[48:80]=bytes.fromhex(p.sha(bad[offset:offset+size]));path=out/'nan.bin';path.write_bytes(bad)
        refused(lambda:p.validate_inputs(bytes(bad)));r=subprocess.run([str(out/'check'),str(path)],capture_output=True,text=True);(out/'nan.log').write_text(r.stdout+r.stderr);assert r.returncode==1 and not r.stderr
        passed('linked-input',{'passed':True,'sanitizers':['address','undefined'],'scope':'Actual linked app input validator, existing GPU geometry/vertex validator and digest. Real existing host-packed two-frame data accepted; rehashed NaN vertex refused. No architecture or device code executes.'})
    if 'provenance' in selected and 'provenance' not in results['cases']:
        folder=a.output/'provenance';folder.mkdir(exist_ok=False)
        outer=json.loads((a.output/'MODELED-inputs/MODELED-admission.json').read_text())
        hdmi=bytearray((a.output/'MODELED-inputs/hdmi.bin').read_bytes())
        original=json.loads(Path(outer['programs']['path']).read_text())
        closure=json.loads(a.producer_inputs.read_text())
        for name,mutate,expected in (
            ('missing-C',lambda program:program.update(producer_inputs={'path':str((folder/'missing-C-inputs.json').resolve()),'sha256':p.digest(folder/'missing-C-inputs.json')}),'omits or ambiguously names'),
            ('shader-mismatch',lambda program:program['shaders'].update(vertex={'path':str((folder/'wrong-vertex.bin').resolve()),'sha256':p.digest(folder/'wrong-vertex.bin')}),'Resident shader differs')):
            if name=='missing-C':p.save_json(folder/'missing-C-inputs.json',[v for v in closure if not v['path'].endswith('/checks/pvr-texture-producer.c')])
            else:
                changed=bytearray(Path(original['shaders']['vertex']['path']).read_bytes());changed[0]^=1;(folder/'wrong-vertex.bin').write_bytes(changed)
            program=json.loads(json.dumps(original));mutate(program);path=folder/(name+'-program.json');p.save_json(path,program)
            record={**outer,'programs':{'path':str(path.resolve()),'sha256':p.digest(path)}};entry=json.dumps(record).encode();hdmi[80:112]=bytes.fromhex(p.sha(entry))
            with patch.object(p,'physical_record',lambda record:None):
                reason=refused(lambda:p.validate_admission(entry,bytes(hdmi),data,build,p.sha(image),image_raw=image))
            assert expected in reason,reason
        passed('provenance',{'passed':True,'cases':['missing shader-producing C input','retained shader differs from immutable uploaded native bytes'],
            'scope':'Actual combined provenance validator with only physical-record predicate substituted. Complete record digests rebound so refusals reach provenance checks; no compiler, launcher or native execution.'})
    if 'usb' in selected and 'usb' not in results['cases']:
        folder=a.output/'usb-refusals';folder.mkdir(exist_ok=False)
        outer=json.loads((a.output/'MODELED-inputs/MODELED-admission.json').read_text())
        hdmi=bytearray((a.output/'MODELED-inputs/hdmi.bin').read_bytes())
        cases=[]
        for name,changes,fact_name,fact_changes in (
            ('owner-collision',{'usb_owner':9},None,None),
            ('missing-handoff',{'usb_handoff':0},None,None),
            ('wrong-ROM-mode',{'usb_cold_reference':1},None,None),
            ('wrong-rails',None,'usb_electrical',{'supply_uv':[3300000,3300000,1200000,900001]}),
            ('wrong-fabric',None,'usb_electrical',{'fabric_sha256':'00'*32}),
            ('wrong-pins',None,'usb_electrical',{'pmic_pins_sha256':'00'*32}),
            ('partial-IRQ-set',None,'usb_initial',{'irq_ids':[188,189]}),
            ('shorter-lifetime',None,'usb_supervision',{'session_seconds':19})):
            record=json.loads(json.dumps(outer))
            if changes:record.update(changes)
            if fact_name:
                fact=json.loads(Path(record[fact_name]['path']).read_text());fact.update(fact_changes)
                path=folder/(name+'.json');p.save_json(path,fact);record[fact_name]={'path':str(path.resolve()),'sha256':p.digest(path)}
            entry=json.dumps(record).encode();hdmi[80:112]=bytes.fromhex(p.sha(entry))
            with patch.object(p,'physical_record',lambda record:None):
                reason=refused(lambda:p.validate_admission(entry,bytes(hdmi),data,build,p.sha(image),image_raw=image))
            assert reason;cases.append(name)
        metadata=json.loads((a.output/'MODELED-NOT-ADMITTED-bundle/prepared.json').read_text())
        assert all(k in metadata for k in ('epoch','generation','lease','usb_owner','usb_handoff','segments'))
        assert metadata['segments'][-1]=={'name':'session.bin','address':0x41f00000}
        assert not any(v['name'] in ('request.bin','contract.bin','launch.bin') for v in metadata['segments'])
        session=(a.output/'MODELED-NOT-ADMITTED-bundle/session.bin').read_bytes()
        assert struct.unpack_from('<3Q',session)==(p.SESSION_MAGIC,2,len(session))
        assert len(session[704:840])==136 and struct.unpack_from('<2I4I',session,816)==(2,0,*p.USB_SUPPLIES)
        passed('usb',{'passed':True,'cases':cases,'derived_authority_bytes':136,'input_offset':840,
            'scope':'New USB authority derivation and typed fact/refusal checks, exact sealed-supervisor metadata. Only physical-record predicate substituted; no native, launcher, queue or USB execution.'})

if __name__=='__main__':main()
