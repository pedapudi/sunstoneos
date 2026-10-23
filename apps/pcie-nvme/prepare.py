#!/usr/bin/env python3
"""Offline preparation of one supervised read-only PCIe NVMe session."""
import hashlib
import importlib.util
import json
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
board=module('pcie_nvme_board_prepare',ROOT/'apps/board/prepare.py')
base=board.base
require=base.require
SCHEMA='a733-pcie-nvme-supervised-bundle-v1'
CONTRACT_MAGIC=0x41533733504e4331
REQUEST_MAGIC=0x41533733504e4c31
SESSION_BYTES=848
# Facts the reviewed NVMe record must assert for the admitted controller.
NVME_FACTS=('bar_mmio_authorized','controller_disabled_at_handoff','nvm_command_set','namespace_read_authorized',
 'queue_dma_authorized','trusted_identity_inbound_accepted','normal_shutdown_authorized')

FACTS={
 'initial':['board_v12_fpc_topology_verified','cold_trusted_endpoint_with_perst_asserted','all_comb1_usb3_clients_excluded','remote_secure_writers_excluded','irq168_175_184_185_owned_masked','endpoint_config_read_authorized','no_inherited_endpoint_dma'],
 'electrical':['external_endpoint_power_stable','perst_pd22_active_low','pd23_not_driven','pd_bank_bldo1_route_verified','pd_bank_bldo1_1v8_stable','dcdc2_comb1_core_route_verified','dcdc2_core_0v8_stable','comb1_analog_supplies_stable','device_mappings_security_verified'],
 'clocks':['shared_serdes100mhz_ready','pck7_on_effective','autonomous_pcie_power_disabled','stable_peri0_ref_osc_fabric','no_global_retune_reset_writers'],
 'supervision':['whole_board_power_removal','permanent_retention','no_reuse','one_attempt']}

def session_check(raw):
    require(len(raw)==SESSION_BYTES,'PCIe NVMe session must be 848 bytes.')
    require(board.session_check(raw[:592])==2,'PCIe requires USB0/provider selection2.')
    require(struct.unpack_from('<3Q',raw,592)==(0x41533733504e4131,1,SESSION_BYTES),'Invalid PCIe NVMe session.')
    owner,authority,timeout,client=struct.unpack_from('<4Q',raw,616)
    require(owner and authority and owner not in struct.unpack_from('<2Q',raw,456) and
        1500000<=timeout<=10000000 and client<16,'Invalid PCIe ownership or startup bound.')
    require(all(any(raw[o:o+32]) for o in (648,680,712,744)),'Missing PCIe physical evidence.')
    for i in range(4):
        function,pull,drive=struct.unpack_from('<3B',raw,776+3*i)
        require((function==1 if i==2 else function in (0,15)) and pull<=2 and drive<=3,'Invalid inherited PD20–23 configuration.')
    vendor,device=struct.unpack_from('<2H',raw,788)
    require(0<vendor<65535 and 0<device<65535,'Actual admitted endpoint identity required.')
    timeout,lba=struct.unpack_from('<2Q',raw,792);function,namespace=struct.unpack_from('<2I',raw,808)
    require(1000<=timeout<=5000000 and function<=7 and 0<namespace<0xffffffff and any(raw[816:848]),'Missing bounded NVMe read authority.')

def request_bytes(address,identity,raw,epoch,generation,lease):
    request,contract=board.request_bytes(address,identity,epoch,generation,lease,raw)
    request=struct.pack('<Q',REQUEST_MAGIC)+request[8:]
    contract=bytearray(contract);struct.pack_into('<Q',contract,184,SESSION_BYTES)
    return request,bytes(contract)

def build_launcher(out,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    (out/'base-supervised-launch.c').write_text(text)
    text=board.adapt_launcher(text,{'0x4153373353554c33':'0x41533733504e4c31','REQUEST[1]==3':'REQUEST[1]==2','CONTRACT->version==3':'CONTRACT->version==2',
        'CONTRACT->reserved[0] && !CONTRACT->reserved[1]':'CONTRACT->reserved[0]==A733_BOARD_SESSION_ADDRESS && CONTRACT->reserved[1]==sizeof(struct pcie_session)',
        '20*A733_SMP_COUNTER_HZ':'60*A733_SMP_COUNTER_HZ'})
    (out/'pcie-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/pcie-nvme/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('pcie_session.h',ROOT/'apps/pcie-nvme/pcie_session.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands += [[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',out/'launch.elf',out/'launch.bin']]
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'PCIe NVMe launcher build failed; inspect retained log.')
    base.save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False):
    # Existing DDR/platform/manual-recovery checks stay exact and byte-snapshotted.
    require(record.get('schema')=='a733-pcie-supervised-entry-v1','Wrong PCIE admission schema.')
    common={**record,'schema':'a733-board-supervised-entry-v2'}
    cache=board.validate_admission(common,image_hash,template_hash,cache,frozen)
    for key,offset in [('initial',648),('electrical',680),('clocks',712),('supervision',744)]:
        item=record[key];path=Path(item['path']);require(path.is_absolute(),'Evidence path must be absolute.')
        if str(path) not in cache:
            require(not frozen,'Missing frozen PCIe evidence.');cache[str(path)]=path.read_bytes()
        data=cache[str(path)];require(hashlib.sha256(data).digest()==raw[offset:offset+32] and hashlib.sha256(data).hexdigest()==item['sha256'],'PCIe evidence differs.')
        fact=json.loads(data)
        require(fact.get('schema')=='a733-pcie-'+key+'-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('reviewed') is True,'PCIe authority is not bound to image/target.')
        require(all(fact.get(k) is True for k in FACTS[key]),'Required PCIe physical fact absent.')
        if key=='initial':require((fact.get('vendor'),fact.get('device'))==struct.unpack_from('<2H',raw,788),'Endpoint identity authority differs.')
        if key=='electrical':
            routes=fact.get('comb1_analog_supply_routes')
            require(isinstance(routes,list) and routes,'Fitted COMB1 analog routes must be identified independently.')
            seen=set()
            for route in routes:
                require(isinstance(route,dict) and all(isinstance(route.get(k),str) and route[k].strip() for k in ('net','source')) and
                        type(route.get('voltage_uv')) is int and route['voltage_uv']>0 and
                        all(route.get(k) is True for k in ('route_verified','stable','lifetime_writers_excluded')),
                        'COMB1 analog route lacks actual source/stability/writer authority.')
                require(route['net'] not in seen,'Duplicate COMB1 analog net.');seen.add(route['net'])

    item=record['nvme'];path=Path(item['path']);require(path.is_absolute(),'NVMe record path must be absolute.')
    if str(path) not in cache:
        require(not frozen,'Missing frozen NVMe record.');cache[str(path)]=path.read_bytes()
    blob=cache[str(path)];require(hashlib.sha256(blob).hexdigest()==item['sha256'] and hashlib.sha256(blob).digest()==raw[816:848],'NVMe record differs.')
    proof=json.loads(blob)
    vendor,device=struct.unpack_from('<2H',raw,788);lba=struct.unpack_from('<Q',raw,800)[0];function,namespace=struct.unpack_from('<2I',raw,808)
    require(proof.get('schema')=='a733-pcie-nvme-read-v1' and proof.get('identity')==record['identity'] and proof.get('port')==record['port'] and
        proof.get('image_sha256')==image_hash and proof.get('reviewed') is True and
        (proof.get('vendor'),proof.get('device'),proof.get('function'),proof.get('namespace_id'),proof.get('lba'))==(vendor,device,function,namespace,lba),
        'NVMe read authority differs.')
    require(all(proof.get(k) is True for k in NVME_FACTS),'NVMe controller or read authority is incomplete.')
    controller=proof.get('controller',{})
    require(all(isinstance(controller.get(k),str) and controller[k].strip() for k in ('model','serial','firmware')),'Actual NVMe controller identity required.')
    require(proof.get('evidence'),'NVMe controller evidence required.')
    for ref in proof['evidence']:
        require(set(ref)=={'path','sha256'} and Path(ref['path']).is_absolute(),'Invalid NVMe evidence reference.')
        name=ref['path']
        if name not in cache:
            require(not frozen,'Missing frozen NVMe evidence.');cache[name]=Path(name).read_bytes()
        require(hashlib.sha256(cache[name]).hexdigest()==ref['sha256'],'NVMe evidence changed.')
    return cache

def prepare(image,manifest,admission,template,output,clang,lld):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();th=hashlib.sha256(raw).hexdigest()
    mf=json.loads(snapshots['image-input-manifest.json'])
    require(mf.get('target')=='sunstone-a733-pcie-nvme' and mf.get('execution_profile')=='pcie-nvme-supervised-v1' and mf.get('final_elf')==ih,'Tested PCIE image manifest required.')
    record=json.loads(snapshots['admission.json']);cache=validate_admission(record,ih,th,raw)
    epoch,generation,lease=(secrets.randbits(64) or 1 for _ in range(3))
    struct.pack_into('<3Q',raw,24,epoch,generation,lease)
    out=Path(output);out.mkdir(parents=True,exist_ok=False)
    for name,data in snapshots.items():(out/name).write_bytes(data)
    segments,address,identity=base.image_parts(out/'image.elf')
    # Runtime authenticates linked build identity, not the ELF file hash.
    raw[48:80]=identity
    build_launcher(out,clang,lld)
    uploads=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';(out/name).write_bytes(segment['data']);uploads.append({'name':name,'address':segment['address']})
    (out/'session.bin').write_bytes(raw);uploads.append({'name':'session.bin','address':0x41f00000})
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,epoch,generation,lease)):(out/name).write_bytes(data)
    for name,count in [('result-zero.bin',16384),('report-zero.bin',128)]:(out/name).write_bytes(bytes(count))
    evidence={}
    for i,(path,data) in enumerate(cache.items()):
        name=f'evidence-{i}.bin';(out/name).write_bytes(data);evidence[path]={'name':name,'sha256':hashlib.sha256(data).hexdigest()}
    p=dict(schema=SCHEMA,physical_access=False,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,reservation=record['reservation'],session='session.bin',profile=None,manifest='image-input-manifest.json',image='image.elf',segments=uploads,entry_evidence=evidence,backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text());require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong PCIE bundle.')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed PCIE authority or transport input.')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'PCIE bundle input changed.')
    require(base.digest(out/'image.elf')==p['image_sha256'],'PCIE image changed.')
    manifest=json.loads((out/'image-input-manifest.json').read_text())
    require(manifest.get('target')=='sunstone-a733-pcie-nvme' and manifest.get('execution_profile')=='pcie-nvme-supervised-v1' and manifest.get('final_elf')==p['image_sha256'],'PCIE image manifest differs.')
    sizes={name:(out/name).stat().st_size for name in p['files']}
    require(p.get('backup_sizes')=={name:n for name,a,n in base.REGIONS} and 0<sizes.get('launch.bin',0)<=0xc00,'PCIE protected SRAM geometry changed.')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128),'PCIE initial report backing changed.')
    raw=(out/'session.bin').read_bytes();session_check(raw)
    template=(out/'session-template.bin').read_bytes();cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    validate_admission(json.loads((out/'admission.json').read_text()),p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True)
    segments,address,identity=base.image_parts(out/'image.elf');require(identity.hex()==p['build_id'],'PCIE build identity changed.')
    expected_raw=bytearray(template);struct.pack_into('<3Q',expected_raw,24,p['epoch'],p['generation'],p['lease']);expected_raw[48:80]=identity
    require(raw==expected_raw,'PCIE session differs from admitted derivation.')
    expected=[]
    for i,segment in enumerate(segments):
        if segment['data']:
            name=f'segment-{i}.bin';require((out/name).read_bytes()==segment['data'],'PCIE ELF bytes changed.');expected.append({'name':name,'address':segment['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(item['name'] in sizes for item in expected),'PCIE upload spans changed or unsealed.')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'])):require((out/name).read_bytes()==data,'PCIE launch contract changed.')
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();prepare(a.image,a.manifest,a.admission,a.template,a.output,a.clang,a.lld)
