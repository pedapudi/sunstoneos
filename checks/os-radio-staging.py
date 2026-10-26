#!/usr/bin/env python3
"""Focused offline OS radio staging. Every board fact is synthetic, never authority."""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import sys
from harness import CLANG, LLD, Steps, refused

ROOT = Path(__file__).resolve().parents[1]

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value

prep = module('radio_staging_prepare', ROOT/'apps/os/prepare.py')
tool = module('radio_staging_tool', ROOT/'tools/check-a733-os-supervised.py')
sha = lambda b: hashlib.sha256(b).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','link-check','fixture','radio-profile','bluetooth-profile','firmware','native-recipe','output'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--resume',action='store_true')
    p.add_argument('--parent-only',action='store_true',help='Only changed fast-mode parent admission')
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=a.resume)
    steps=Steps(out,'commands.json');facts=out/'MODEL-facts';facts.mkdir(exist_ok=True)
    (out/'invocation.json').write_text(json.dumps([sys.executable,*sys.argv],indent=2)+'\n')
    old=a.fixture/'board';meta=json.loads((old/'prepared.json').read_text())
    oldcache={path:(old/v['name']).read_bytes() for path,v in meta['entry_evidence'].items()}
    original=json.loads((old/'admission.json').read_text());image=a.image.read_bytes();image_hash=sha(image)
    serial=0
    def put(name, value):
        data=value if isinstance(value,(bytes,bytearray)) else json.dumps(value,sort_keys=True).encode()
        path=(facts/(name+'.bin')).resolve();path.write_bytes(data)
        return {'path':str(path),'sha256':sha(data)}
    def clone(value, target_hash=image_hash):
        nonlocal serial
        if isinstance(value,dict) and set(value)=={'path','sha256'}:
            data=oldcache[value['path']]
            try:data=json.dumps(clone(json.loads(data),target_hash),sort_keys=True).encode()
            except (ValueError,UnicodeDecodeError):pass
            serial+=1;return put('baseline-'+str(serial),data)
        if isinstance(value,dict):return {k:clone(v,target_hash) for k,v in value.items()}
        if isinstance(value,list):return [clone(v,target_hash) for v in value]
        return target_hash if value==original['image_sha256'] else value
    record=clone(original);raw=bytearray((old/'session-template.bin').read_bytes()[:808])
    struct.pack_into('<Q',raw,608,808);struct.pack_into('<I',raw,400,1)
    for key,offset in [('initial',632),('electrical',664),('clocks',696),('supervision',728)]:raw[offset:offset+32]=bytes.fromhex(record[key]['sha256'])
    target={k:record[k] for k in ('identity','port','image_sha256','epoch','generation','lease')}
    marker=put('NEVER-DEPLOY',{'physical_access':False,'warning':'Synthetic staging fixture. Never physical authority.'})
    ddr={**target,'schema':'a733-ddr-reservation-v1','exclusive':True,'published_cache_disabled':True,
         'remote_writers_excluded':True,'ranges':[[0x43000000,0x43100000]],'evidence':[marker]}
    record['radio_ddr']=put('radio-ddr',ddr)
    r=bytearray(a.radio_profile.read_bytes());h=bytearray(a.bluetooth_profile.read_bytes()+bytes(208))
    assert len(r)==1872 and len(h)==832
    for i,offset in enumerate((16,24,32,40,48,56,64)):struct.pack_into('<Q',r,offset,70+i)
    struct.pack_into('<II',h,8,3,832)
    struct.pack_into('<Q',raw,312,7);struct.pack_into('<2Q2I',raw,432,64,65,0,0)
    topology=struct.unpack_from('<Q',raw,184)[0]
    platform=bytearray(r[184:336]);struct.pack_into('<4Q',platform,0,60,0,0,0)
    struct.pack_into('<Q4I',platform,32,topology,0,1,1,1);platform[56:80]=raw[488:512]
    r[184:336]=h[176:328]=platform;h[152:176]=raw[432:456]
    struct.pack_into('<2Q',h,16,topology,7);struct.pack_into('<Q',h,352,62)
    struct.pack_into('<Q',r,336,61);r[384:408]=raw[488:512]
    struct.pack_into('<Q',r,1224,63);struct.pack_into('<Q',h,576,63)
    chip=struct.pack('<2I4B',0xc30000,0,3,0,1,1);r[496:508]=h[496:508]=chip
    for off in (192,200,208,344,360,1232,1288,1320):struct.pack_into('<Q',r,off,0)
    for off in (184,192,200,360,368):struct.pack_into('<Q',h,off,0)
    struct.pack_into('<2I',h,624,2,1);h[632:638]=bytes([1,2,3,4,5,6]);h[655]=1
    struct.pack_into('<2IQ',h,656,2,7,17);struct.pack_into('<2Q',h,704,1000,20000)
    struct.pack_into('<I4H2I',h,720,24000,128,0x1fa0,0x800,0x12,1000000,5000000)
    models=json.loads((ROOT/'apps/bluetooth/recognition-models.json').read_text())['models']
    struct.pack_into('<2Q',h,752,*(v['bytes'] for v in models));h[768:832]=b''.join(bytes.fromhex(v['sha256']) for v in models)
    scope={'identity':record['identity'],'port':record['port'],'lifetime':'MODEL-NO-BOARD-LIFETIME'}
    proof={'schema':'a733-radio-sdio-id-v1',**scope}
    fact_objects={}
    for group,keys in prep.radio_assets.FACTS.items():
        f={'schema':'a733-radio-sdio-id-v1-'+group,**scope,'reviewed':True,**dict.fromkeys(keys,True)}
        if group=='topology':f.update(rails={'vbat':'dcdc1','pg':'dcdc8','pm':'dcdc8','vddio':'dcdc8'},routing_evidence='MODEL only')
        if group=='pins':
            f.update({k:[list(platform[o+3*i:o+3*i+3]) for i in range(8)] for k,o in [('expected',80),('desired',104)]})
        if group=='startup':f.update(fabric=list(struct.unpack_from('<3I',platform,56)),fabric_hz=list(struct.unpack_from('<3I',platform,68)),peri0_400mhz_branch_verified=True)
        fact_objects[group]=f;proof[group]=put('radio-'+group,f)
    doc={'schema':prep.radio_assets.SCHEMA,**target,**scope,'reviewed':True,'platform':put('radio-platform',proof),
         'chip_id':list(struct.unpack_from('<4I',r,72)),'sid_authority':7}
    r[88:120]=bytes.fromhex(doc['platform']['sha256'])
    for key,offset,profile in [('radio_ce',120,r),('radio_entropy',152,r),('bluetooth_wiring',56,h),
                              ('bluetooth_sid',88,h),('bluetooth_transport',120,h),('bluetooth_scheduling',672,h)]:
        doc[key]=put(key,{'schema':'MODEL-ONLY',**target,'warning':'Synthetic physical fact only'})
        profile[offset:offset+32]=bytes.fromhex(doc[key]['sha256'])
    doc['radio_sid']=doc['bluetooth_sid']
    names=['fw_adid_8800d80_u02.bin','fw_patch_8800d80_u02.bin','fw_patch_table_8800d80_u02.bin','fmacfw_8800d80_h_u02.bin']
    addresses=[0x43010000,0x43020000,0x43030000,0x43040000];payloads=[]
    for address,name in zip(addresses,names):
        path=(a.firmware/name).resolve();payloads.append({'path':str(path),'sha256':sha(path.read_bytes()),'address':address})
    sizes=[(a.firmware/n).stat().st_size for n in names]
    struct.pack_into('<4Q',r,432,*addresses);struct.pack_into('<4Q',r,464,*sizes)
    struct.pack_into('<4Q',h,512,*addresses);struct.pack_into('<4Q',h,544,*sizes);doc['payloads']=payloads
    doc['radio_profile']=put('radio-profile',r);doc['headset_profile']=put('headset-profile',h)
    raw[320:352]=bytes.fromhex(doc['headset_profile']['sha256'])
    struct.pack_into('<2Q',raw,352,0x43000000,len(r));raw[368:400]=bytes.fromhex(doc['radio_profile']['sha256'])
    record['radio_assets']=put('radio-assets',doc);record['session_template_sha256']=sha(raw)
    template=out/'MODEL-session.bin';template.write_bytes(raw);entry=out/'MODEL-entry.json';entry.write_text(json.dumps(record,indent=2)+'\n')
    manifest=json.loads(a.link_check.read_text());flags=prep.radio_assets.features(manifest,image,prep.os_link.Elf,prep.require)
    assert flags=={'radio':True,'headset':True,'msbc':False}
    selected=[]
    def case(name, action):
        selected.append(name)
        if steps.case(name):return
        try:action()
        except Exception as error:steps.failed(error);raise
        steps.case(name,{'passed':True})
    if a.parent_only:
        case('fast-parent-reviewed',lambda:prep.radio_assets.validate(record,raw,flags,{},prep.board,prep.bluetooth,prep.require))
        def parent(value):
            ff=copy.deepcopy(fact_objects['startup']);ff.pop('peri0_400mhz_branch_verified')
            if value is not None:ff['peri0_400mhz_branch_verified']=value
            pp={**proof,'startup':put('parent-startup-'+str(value),ff)}
            dd={**doc,'platform':put('parent-platform-'+str(value),pp)}
            rr=bytearray(r);rr[88:120]=bytes.fromhex(dd['platform']['sha256'])
            dd['radio_profile']=put('parent-radio-'+str(value),rr);ss=bytearray(raw)
            ss[368:400]=bytes.fromhex(dd['radio_profile']['sha256'])
            entry={**record,'radio_assets':put('parent-assets-'+str(value),dd)}
            refused(lambda:prep.radio_assets.validate(entry,ss,flags,{},prep.board,prep.bluetooth,prep.require),'reviewed PERI0 400 MHz branch')
        for value in (None,False,1):case('fast-parent-refused-'+str(value),lambda value=value:parent(value))
        (out/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'cases':selected},indent=2)+'\n')
        print('Passed four fast-parent admission cases');return
    case('linked-profile-admission',lambda:(prep.image_manifest_check(manifest,image),prep.session_check(raw,False,flags)))
    nominal_cache={}
    case('profile-and-firmware-staging',lambda:prep.radio_assets.validate(record,raw,flags,nominal_cache,prep.board,prep.bluetooth,prep.require))
    # Repopulate in memory after a resume; this performs no test execution.
    plan=prep.radio_assets.validate(record,raw,flags,nominal_cache,prep.board,prep.bluetooth,prep.require)
    assert len(plan)==6 and [v for _,v in plan]==[0x43000000,0x43008000]+addresses
    asset_paths=[a.fixture/'assets'/f'asset-{i}.bin' for i in range(3)]
    bundle=out/'MODEL-NOT-DEPLOYABLE-bundle'
    case('complete-prepare',lambda:prep.prepare(a.image,a.link_check,entry,template,asset_paths,bundle,CLANG,LLD))
    case('frozen-validation',lambda:prep.validate_bundle(bundle))
    def preload():
        frozen=tool.freeze_bundle(bundle,1)
        with tool.combined.launch_view(frozen) as (view,derived):
            assert len(derived['segments'])==len(frozen['segments'])+3
            for path,address in plan:
                item=next(v for v in derived['segments'] if v['address']==address)
                assert (view/item['name']).read_bytes()==nominal_cache[path]
    case('modeled-launcher-preload',preload)
    def mutate(name,edit,reason):
        def action():
            changed=copy.deepcopy(doc);edit(changed)
            ref=put(name,changed);value={**record,'radio_assets':ref}
            refused(lambda:prep.radio_assets.validate(value,raw,flags,{},prep.board,prep.bluetooth,prep.require),reason)
        case(name,action)
    mutate('wrong-lifetime',lambda d:d.update(lease=d['lease']+1),'another reviewed lifetime')
    mutate('missing-profile',lambda d:d.pop('headset_profile'),'profile selection differs')
    mutate('missing-platform',lambda d:d.pop('platform'),'Invalid radio evidence reference')
    mutate('missing-firmware',lambda d:d.update(payloads=[]),'Exactly four')
    mutate('wrong-payload-role',lambda d:d['payloads'].reverse(),'Firmware role address differs')
    mutate('wrong-payload-address',lambda d:d['payloads'][0].update(address=0x44000000),'Firmware role address differs')
    mutate('missing-scheduling',lambda d:d.pop('bluetooth_scheduling'),'Invalid radio evidence reference')
    case('frozen-no-fallback',lambda:refused(lambda:prep.radio_assets.validate(record,raw,flags,{},prep.board,prep.bluetooth,prep.require,frozen=True),'missing from frozen bundle'))
    case('no-radio-compatibility',lambda:prep.radio_assets.validate({},raw,{'radio':False,'headset':False}, {},prep.board,prep.bluetooth,prep.require))
    case('unexpected-radio-assets',lambda:refused(lambda:prep.radio_assets.validate(record,raw,{'radio':False,'headset':False},{},prep.board,prep.bluetooth,prep.require),'without radio/headset'))
    case('missing-ddr-reservation',lambda:refused(lambda:prep.radio_assets.validate({k:v for k,v in record.items() if k!='radio_ddr'},raw,flags,{},prep.board,prep.bluetooth,prep.require),'DDR reservation required'))
    def rebound(name, edit, reason=None):
        def action():
            rr,hh,ss,dd,pp,ff=bytearray(r),bytearray(h),bytearray(raw),copy.deepcopy(doc),copy.deepcopy(proof),copy.deepcopy(fact_objects)
            edit(rr,hh,ss,dd,ff)
            for group,fact in ff.items():pp[group]=put(name+'-'+group,fact)
            dd['platform']=put(name+'-platform',pp);rr[88:120]=bytes.fromhex(dd['platform']['sha256'])
            if dd.get('radio_profile'):
                dd['radio_profile']=put(name+'-radio',rr);ss[368:400]=bytes.fromhex(dd['radio_profile']['sha256'])
            if dd.get('headset_profile'):
                dd['headset_profile']=put(name+'-headset',hh);ss[320:352]=bytes.fromhex(dd['headset_profile']['sha256'])
            else:ss[320:352]=bytes.fromhex(dd['radio_sid']['sha256'])
            value={**record,'radio_assets':put(name+'-assets',dd)}
            mode={'radio':bool(dd.get('radio_profile')),'headset':bool(dd.get('headset_profile')),'msbc':False}
            check=lambda:prep.radio_assets.validate(value,ss,mode,{},prep.board,prep.bluetooth,prep.require)
            if reason:refused(check,reason)
            else:
                prep.session_check(ss,False,mode);assert len(check())==4+int(mode['radio'])+int(mode['headset'])
        case(name,action)
    rebound('missing-pm2-fact',lambda rr,hh,ss,dd,ff:ff['pins'].pop('pm2_reset_lpo_stable'),'Missing reviewed radio pins')
    rebound('unverified-fitted-rails',lambda rr,hh,ss,dd,ff:ff['topology'].update(fitted_board_routes_verified=False),'Missing reviewed radio topology')
    rebound('different-fitted-rails',lambda rr,hh,ss,dd,ff:ff['topology']['rails'].update(pg='bldo5'),'Fitted rail routing absent or different')
    rebound('module-owner-mismatch',lambda rr,hh,ss,dd,ff:struct.pack_into('<Q',hh,176,80),'Shared module profiles disagree')
    rebound('module-chip-mismatch',lambda rr,hh,ss,dd,ff:struct.pack_into('<I',hh,496,0xc70000),'Shared module profiles disagree')
    rebound('thermal-owner-collision',lambda rr,hh,ss,dd,ff:struct.pack_into('<Q',hh,352,15),'Headset owners collide')
    rebound('unsupported-headset-codec',lambda rr,hh,ss,dd,ff:struct.pack_into('<I',hh,624,3),'Headset mode exceeds linked codec')
    def overlap(rr,hh,ss,dd,ff):
        struct.pack_into('<Q',rr,440,addresses[0]);struct.pack_into('<Q',hh,520,addresses[0]);dd['payloads'][1]['address']=addresses[0]
    rebound('firmware-overlap',overlap,'Overlapping profile firmware spans')
    def malformed_pm(rr,hh,ss,dd,ff):
        struct.pack_into('<I',ss,452,1);hh[152:176]=ss[432:456]
    rebound('zero-mask-is-not-a-latch-value',malformed_pm,'Invalid normal PM ownership record')
    def complete_pm(rr,hh,ss,dd,ff):
        struct.pack_into('<2I',ss,448,63,5);hh[152:176]=ss[432:456]
    rebound('full-mask-needs-history',complete_pm,'Complete PM latch provenance absent')
    def proven_pm(rr,hh,ss,dd,ff):
        complete_pm(rr,hh,ss,dd,ff)
        ff['pins'].update(complete_pm_latch_authority=True,pm_latch=5,latch_origin='verified-writer-history',latch_evidence='MODEL only')
    rebound('full-mask-with-history',proven_pm)
    def alone(rr,hh,ss,dd,ff):
        dd.pop('radio_profile');struct.pack_into('<I',ss,400,0);ss[352:400]=bytes(48);ss[432:456]=bytes(24)
    rebound('headset-only-assets',alone)
    rebound('radio-only-assets',lambda rr,hh,ss,dd,ff:dd.pop('headset_profile'))
    def corrupt_blob():
        dd=copy.deepcopy(doc);data=bytearray(Path(dd['payloads'][0]['path']).read_bytes());data[0]^=1
        ref=put('corrupt-pinned-blob',data);dd['payloads'][0]={**ref,'address':addresses[0]}
        value={**record,'radio_assets':put('corrupt-blob-assets',dd)}
        refused(lambda:prep.radio_assets.validate(value,raw,flags,{},prep.board,prep.bluetooth,prep.require),'Firmware role differs from native catalogue')
    case('firmware-catalogue-not-caller-hash',corrupt_blob)
    def unavailable_sources():
        original_read=Path.read_bytes
        def restricted(path):
            assert str(path) not in nominal_cache,'live artifact fallback'
            return original_read(path)
        Path.read_bytes=restricted
        try:prep.validate_bundle(bundle)
        finally:Path.read_bytes=original_read
    case('frozen-originals-unreadable',unavailable_sources)
    def tampered_plan():
        dest=out/'MODEL-resealed-wrong-address';shutil.copytree(bundle,dest)
        metadata=json.loads((dest/'board/prepared.json').read_text());metadata['segments'][-1]['address']+=64
        (dest/'board/prepared.json').write_text(json.dumps(metadata))
        outer=json.loads((dest/'prepared.json').read_text());outer['board_prepared_sha256']=sha((dest/'board/prepared.json').read_bytes())
        (dest/'prepared.json').write_text(json.dumps(outer))
        refused(lambda:prep.validate_bundle(dest),'Unsealed or changed upload')
    case('resealed-upload-address-tamper',tampered_plan)
    case('radio-single-recognition-bound',lambda:refused(lambda:tool.freeze_bundle(bundle,2),'Repeated capture requires keyboard'))
    def standalone():
        data=a.bluetooth_profile.read_bytes();session=bytearray(raw);struct.pack_into('<I',session,400,2)
        session[320:352]=hashlib.sha256(data).digest();prep.bluetooth.profile_check(data,session)
        struct.pack_into('<I',session,400,1)
        refused(lambda:prep.bluetooth.profile_check(data,session),'Bluetooth profile/session identity differs')
    case('standalone-bluetooth-default-preserved',standalone)
    def plain():
        admission=clone(original,original['image_sha256']);session=bytearray((old/'session-template.bin').read_bytes())
        for key,offset in [('initial',632),('electrical',664),('clocks',696),('supervision',728)]:session[offset:offset+32]=bytes.fromhex(admission[key]['sha256'])
        admission['session_template_sha256']=sha(session)
        prep.prepare(old/'image.elf',old/'image-input-manifest.json',put('plain-entry',admission)['path'],
                     put('plain-session',session)['path'],asset_paths,out/'MODEL-plain-compatible',CLANG,LLD)
    case('plain-os-prepare-compatible',plain)
    for flag in ('radio','headset'):
        case('linked-'+flag+'-cannot-be-hidden',lambda flag=flag:refused(
            lambda:prep.image_manifest_check({**manifest,flag:False},image),'Linked '+flag+' capability differs'))
    native_command=json.loads(a.native_recipe.read_text())[0]
    native_command=[str(CLANG),*native_command[1:-2]]
    native_command[native_command.index('checks/bluetooth-pm-profile.c')]='checks/os-radio-staging-profile.c'
    native_command += ['-DSUNSTONE_BLUETOOTH_HEADSET=1','-DSUNSTONE_BLUETOOTH_RECOGNITION=1',
                       '-I'+str(a.image.parent/'os-image/bluetooth'),'-o',str(out/'native-profile')]
    steps.command('native-profile-build',native_command)
    case('native-staged-profile-admission',lambda:steps.command('native-profile-run',[out/'native-profile',doc['headset_profile']['path']]))
    result={'passed':True,'physical_access':False,'image_sha256':image_hash,'cases':selected,'reuse':'Native radio/headset image and unchanged runtime evidence retained. Synthetic facts are never board authority.'}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
