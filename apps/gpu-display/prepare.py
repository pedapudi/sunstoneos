#!/usr/bin/env python3
"""Prepare a bounded GPU-to-HDMI RAM bundle offline; no device or queue access."""
import argparse
import hashlib
import importlib.util
import io
import json
import re
from pathlib import Path
import shutil
import struct
import subprocess
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
gpu=module('gpu_display_source_contracts',ROOT/'apps/gpu-render/prepare.py')
packer=module('gpu_display_input_packer',ROOT/'apps/gpu-display/host.py')
require,save_json=gpu.require,gpu.save_json
def sha(raw):return hashlib.sha256(raw).hexdigest()
def digest(path):return sha(Path(path).read_bytes())
SCHEMA='a733-gpu-display-supervised-bundle-v1'
ADMISSION='a733-gpu-display-supervised-admission-v1'
PROFILE='gpu-display-supervised-v1'
SESSION_MAGIC=0x4153373347444131
CONTRACT_MAGIC=0x4153373347444331
REQUEST_MAGIC=0x4153373347444c31
GEOMETRY=(SESSION_MAGIC,1,704,288,0x41f00000,1048576,20,544,568,144,1280,720)
RANGES=[[0x41000000,0x42200000],[0x48000000,0x78000000],[0x78000000,0x79000000]]
CONDITIONS={
 'analog':{'qualified-complete-analog-image','charge-pump-and-reserved-test-provenance','cold-reset-inactive-vo1','exclusive-resistor-word-writers'},
 'power':{'authenticated-pmic-pin-inheritance','hdmi-rails-and-operating-point-qualified','stable-shared-clock-power-lifetime'},
 'supervision':{'manual-whole-board-power-removal','retain-all-resources-until-power-removal','stop-after-first-failure','one-attempt'},
 'isolation':{'all-system-iommu-masters-excluded','prior-transactions-and-walks-drained','remote-secure-writers-excluded','de-tv0-iommu-irqs-owned-masked'},
 'memory':{'full-extended-dram-physically-reserved','no-physical-aliases','display-parent-retained','master8-readonly-display-bank','gpu-private-mmu-confinement','cache-publication-and-acquisition'},
 'gpu_initial':gpu.GPU_CONDITIONS,
 'gpu_memory':{'full-extended-dram-physically-reserved','firmware-and-graphics-address-paths-confined','no-physical-aliases','cache-publication-and-acquisition'},
 'gpu_electrical':{'gpu-pll-400mhz-parent','gpu-dcdc4-800mv-settled','shared-ccu-pck-pmic-ownership','gpu-hdmi-concurrent-lease-exclusion'},
 'gpu_supervision':{'manual-whole-board-power-removal','retain-all-resources-until-power-removal','stop-after-first-failure','combined-gpu-hdmi-lifetime'}}

def image_data(raw):
    elf=ELFFile(io.BytesIO(raw));require(elf.elfclass==64 and elf.little_endian and elf['e_machine']=='EM_AARCH64' and elf['e_type']=='ET_EXEC' and elf['e_entry']==0x41000000,'Native GPU/display ELF required.')
    parts=[]
    for p in elf.iter_segments():
        if p['p_type']!='PT_LOAD':continue
        a,m,n,o,flags=(int(p[k]) for k in ('p_vaddr','p_memsz','p_filesz','p_offset','p_flags'))
        require(a==p['p_paddr'] and m>0 and 0<=n<=m and 0<=o<=o+n<=len(raw) and flags&4 and not flags&~7 and flags&3!=3,'Invalid image segment.')
        require(any(lo<=a<a+m<=hi for lo,hi in RANGES),'Segment outside exact reserved ranges.')
        require(not flags&1 or a+m<=0x41200000,'Executable segment exceeds immutable image prefix.')
        require(not n or a+n<=0x41f00000,'Initialized segment overlaps session or monitor storage.')
        if a<0x42000000 and 0x41f00000<a+m:
            require((a,m,n)==(0x41f00000,1048576,0),'Session reservation must be exact and uninitialized.')
        require(all(a+m<=v['address'] or v['address']+v['memory_bytes']<=a for v in parts),'Overlapping image allocations.')
        parts.append({'address':a,'memory_bytes':m,'flags':flags,'data':raw[o:o+n]})
    symbols={s.name:s for s in elf.get_section_by_name('.symtab').iter_symbols()}
    def read(name,size):
        require(name in symbols and symbols[name]['st_size']==size,'Missing immutable symbol: '+name)
        at=int(symbols[name]['st_value'])
        for p in parts:
            if p['address']<=at and at+size<=p['address']+len(p['data']):
                require(not p['flags']&2,'Writable immutable symbol: '+name)
                return at,p['data'][at-p['address']:at-p['address']+size]
        raise ValueError('Unbacked immutable symbol: '+name)
    require(struct.unpack('<12Q',read('gpu_display_geometry',96)[1])==GEOMETRY,'Unsupported native geometry.')
    for name,address in {'__device_pool_start':0x48000000,'__device_pool_end':0x78000000,'__uncached_pool_start':0x78000000,'__uncached_pool_end':0x79000000,'__a733_smp_result':0x42000000,'__a733_smp_contract':0x42004000,'__a733_smp_contract_end':0x42005000}.items():
        require(name in symbols and symbols[name]['st_value']==address,'Linker reservation differs: '+name)
    require(elf.has_dwarf_info(),'Native DWARF ABI information required.')
    wanted={'gpu_display_session':(848,{'hdmi':24,'gpu_initial':568,'gpu_memory':600,'gpu_electrical':632,'gpu_supervision':664,'memory_authority':696,'inputs':704}),
        'hdmi_session':(544,{'image':48,'evidence':80,'analog':112,'power':144,'supervision':176,'supervisor':208,'owner':216,'pck_client':224,'request':232,'pmic':304,'pins':336,'framebuffer':440}),
        'hdmi_framebuffer_session':(104,{'isolation':0,'memory':32,'authority':64}),
        'gpu_display_inputs':(144,{'frame':16})};found=set()
    for cu in elf.get_dwarf_info().iter_CUs():
        for die in cu.iter_DIEs():
            name=die.attributes.get('DW_AT_name');size=die.attributes.get('DW_AT_byte_size')
            if die.tag!='DW_TAG_structure_type' or not name or not size or name.value.decode() not in wanted:continue
            name=name.value.decode();members={c.attributes['DW_AT_name'].value.decode():c.attributes['DW_AT_data_member_location'].value for c in die.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes and 'DW_AT_data_member_location' in c.attributes}
            expected,offsets=wanted[name];require(size.value==expected and all(members.get(k)==v for k,v in offsets.items()),'Native ABI drift: '+name);found.add(name)
    require(found==set(wanted),'Incomplete native ABI declarations.')
    address,build=read('a733_smp_build_id',32);require(0x41000000<=address<=0x411fffe0 and any(build),'Invalid native build identity.')
    start,end=(symbols[k]['st_value'] for k in ('gpu_firmware','gpu_firmware_end'))
    firmware=next((p['data'][start-p['address']:end-p['address']] for p in parts if p['address']<=start<end<=p['address']+len(p['data'])),None)
    require(firmware is not None and sha(firmware)==json.loads((ROOT/'sources.lock.json').read_text())['gpu-firmware']['sha256'],'Embedded opaque firmware differs from pin.')
    return parts,address,build

def validate_inputs(raw):
    require(len(raw)>=144 and struct.unpack_from('<Q2I',raw)==(packer.MAGIC,1,len(raw)) and len(raw)<=1048576-704,'Invalid packed application extent.')
    frames=[];next_offset=144
    for index in range(2):
        row=struct.unpack_from('<8I32s',raw,16+index*64);shape=row[:5];offset,size,reserved,expected=row[5:]
        require(not reserved and offset==next_offset and 0<size<=len(raw)-offset,'Input offset/reserved/extent differs.')
        payload=raw[offset:offset+size];require(hashlib.sha256(payload).digest()==expected,'Input digest differs.')
        width,height,stride,vertices,indices=shape
        require(1<=width<=1920 and 1<=height<=1080 and stride==((width+1)&~1) and
            1<=vertices<=65536 and 3<=indices<=65535 and indices%3==0 and
            size==vertices*24+indices*2+stride*height*4,'Input geometry bounds.')
        # Match profile2's bit-level finite/range rules, including signed zero.
        for v in range(vertices):
            w=struct.unpack_from('<6I',payload,v*24)
            require(all((w[k]&0x7fffffff)<=0x3f800000 for k in (0,1)) and not w[2]&0x7fffffff and w[3]==0x3f800000 and
                all((w[k]&0x7fffffff)<=0x3f800000 and (not w[k]&0x80000000 or not w[k]&0x7fffffff) for k in (4,5)),'Noncanonical profile2 vertex.')
        require(all(struct.unpack_from('<H',payload,vertices*24+i*2)[0]<vertices for i in range(indices)),'Index exceeds vertex extent.')
        frames.append((shape,payload));next_offset+=size
    require(next_offset==len(raw) and packer.inputs(frames,limit=1048576-704)==raw,'Noncanonical packed texture input.')
    return frames

def session_profile(hdmi):
    require(len(hdmi)==544 and struct.unpack_from('<3Q',hdmi)==(0x4153373348444131,1,544),'Exact extended-profile HDMI session required.')
    epoch,generation,lease=struct.unpack_from('<3Q',hdmi,24);supervisor,owner=struct.unpack_from('<2Q',hdmi,208)
    client,reserved=struct.unpack_from('<2I',hdmi,224);pmic,identity,deadline=struct.unpack_from('<3Q',hdmi,304)
    analog_id,analog_generation=struct.unpack_from('<2Q',hdmi,232);mode,range_,source=struct.unpack_from('<3I',hdmi,288)
    words=struct.unpack_from('<8I',hdmi,248)
    require(hdmi[328:334]==bytes((2,1,1,2,1,1)),'This bounded profile requires admitted inherited PL0/PL1 mux2/pullup/drive1.')
    authorized=struct.unpack_from('<13I',hdmi,336);irq=struct.unpack_from('<I',hdmi,388)[0]
    require(authorized==tuple(3 if b==11 else 0 for b in range(13)) and not irq,'Only PL0/PL1 and no GPIO IRQ owner are admitted.')
    latch_id,latch_owner,known,value=struct.unpack_from('<2Q2I',hdmi,392)
    require(latch_id and latch_owner==pmic and known==0x3fff and not value&~known and hdmi[416:440]==bytes(24),'Qualified complete PL latch handoff required.')
    authority=struct.unpack_from('<Q',hdmi,504)[0]
    require(all((epoch,generation,lease,supervisor,owner,pmic,analog_id,analog_generation,authority)) and owner!=pmic and
        owner not in (9,10,11) and pmic not in (9,10,11) and client<16 and client!=7 and not reserved and identity==generation and not deadline and
        (mode,range_,source)==(4,0,1) and struct.unpack_from('<2I',hdmi,536)==(0x10007,0x10007),'HDMI ownership/mode/authority differs.')
    require(not words[0]&0x80000300 and not words[1]&1 and not words[2]&0xe8000000 and not words[3]&0x100,'Unsupported initial live/PHY control bits.')
    return dict(epoch=epoch,generation=generation,lease=lease,supervisor=supervisor,owner=owner,framebuffer_authority=authority)

def physical_record(record):
    # Explicitly separate modeled parser fixtures from externally reviewed
    # physical admission. This declaration still requires all bound proof bytes;
    # it cannot establish the asserted hardware conditions by itself.
    require(record.get('physical_access') is True and record.get('reviewed') is True,'Reviewed physical record required; software fixtures grant no authority.')

def validate_admission(admission_raw,hdmi,inputs,build,image_hash,cache=None,frozen=False,*,image_raw=None):
    cache={} if cache is None else cache
    def evidence(ref):
        require(isinstance(ref,dict) and set(ref)=={'path','sha256'} and isinstance(ref['path'],str) and Path(ref['path']).is_absolute(),'Absolute hashed proof reference required.')
        name=ref['path']
        if name not in cache:
            require(not frozen,'Missing frozen physical proof.');cache[name]=Path(name).read_bytes()
        require(sha(cache[name])==ref['sha256'],'Physical proof bytes changed.');return cache[name]
    record=json.loads(admission_raw);profile=session_profile(hdmi);validate_inputs(inputs);physical_record(record)
    require(record.get('schema')==ADMISSION and record.get('execution_profile')==PROFILE and record.get('image_sha256')==image_hash and
        all(record.get(k)==v for k,v in profile.items()) and record.get('identity') and record.get('port') and
        type(record.get('memory_authority')) is int and 0<record['memory_authority']<2**64 and record.get('session_seconds')==20 and
        record.get('combined_lifetime') is True and record.get('inputs_sha256')==sha(inputs),'Explicit combined GPU/display physical admission required.')
    require(hdmi[48:80]==build and hdmi[80:112]==hashlib.sha256(admission_raw).digest(),'HDMI session does not bind combined entry and image.')
    ddr=json.loads(evidence(record['ddr']));physical_record(ddr)
    require(ddr.get('schema')=='a733-ddr-reservation-v1' and all(ddr.get(k)==record[k] for k in ('identity','port','image_sha256')) and
        ddr.get('ranges')==RANGES and ddr.get('memory_authority')==record['memory_authority'] and
        all(ddr.get(k) is True for k in ('exclusive','published_cache_disabled','remote_writers_excluded')) and ddr.get('evidence'),'Actual full extended DDR authority required.')
    for ref in ddr['evidence']:evidence(ref)
    offsets={'analog':112,'power':144,'supervision':176,'isolation':440,'memory':472}
    gpu_digests=[]
    for name,conditions in CONDITIONS.items():
        data=evidence(record[name]);facts=json.loads(data);physical_record(facts)
        require(facts.get('schema')=='a733-gpu-display-'+name.replace('_','-')+'-v1' and facts.get('reviewed') is True and facts.get('combined_lifetime') is True and
            all(facts.get(k)==record[k] for k in ('identity','port','image_sha256','memory_authority','epoch','generation','lease','supervisor')) and
            set(facts.get('conditions',{}))==conditions,'Physical fact does not cover combined lifetime: '+name)
        for ref in facts['conditions'].values():evidence(ref)
        if name in offsets:require(hashlib.sha256(data).digest()==hdmi[offsets[name]:offsets[name]+32],'HDMI proof digest differs: '+name)
        else:gpu_digests.append(hashlib.sha256(data).digest())
        if name=='analog':require(facts.get('request_sha256')==sha(hdmi[232:304]),'Qualified analog request differs.')
        if name=='power':require(facts.get('pmic_pins_sha256')==sha(hdmi[304:440]),'Inherited PMIC/pins differ.')
        if name in ('memory','gpu_memory'):
            require(facts.get('ranges')==RANGES and facts.get('display_parent_bytes')==4194304 and facts.get('framebuffer_authority')==profile['framebuffer_authority'],'Compact or different display memory authority refused.')
        if name=='memory':require(facts.get('display_master')==8 and facts.get('gpu_system_iommu_master') is None and facts.get('display_bank_bytes')==3739648 and facts.get('framebuffer_snapshot_sha256')==sha(hdmi[504:544]),'Display mapping/snapshot differs.')
    pin=json.loads((ROOT/'sources.lock.json').read_text())
    require(sha(evidence(record['firmware']))==pin['gpu-firmware']['sha256'] and sha(evidence(record['firmware_license']))==pin['gpu-firmware-license']['sha256'],'Opaque runtime firmware/license provenance differs.')
    programs=json.loads(evidence(record['programs']))
    require(programs.get('schema')=='a733-gpu-display-trusted-programs-v1' and programs.get('reviewed') is True and programs.get('profile')==2 and programs.get('image_sha256')==image_hash and
        programs.get('source_sha256')==digest(ROOT/'drivers/gpu/pvr_render_texture.c') and
        programs.get('mesa_revision')=='86c7176d2dd0065555521bf8c61f208bf53c75d6','Resident graphics program/source provenance differs.')
    require(sha(evidence(programs['source']))==programs['source_sha256'] and sha(evidence(programs['producer']))==programs['producer_sha256'] and
        sha(evidence(programs['compiler_source']))=='ca5e3ac4b61b8dc659cc489c7eb3c7bcc750f6820fbf39f0b831ac87fb00da58','Resident program provenance bytes differ.')
    # The existing producer writes this complete compiled-input list, including
    # generated includes, compiler libraries and its actual project C inputs.
    closure=json.loads(evidence(programs['producer_inputs']))
    require(isinstance(closure,list) and closure and len({v['path'] for v in closure})==len(closure),'Unique reviewed producer-input closure required.')
    for ref in closure:evidence(ref)
    require(any(v==programs['producer'] for v in closure),'Producer script differs from reviewed compiled closure.')
    pending=['checks/pvr-texture-producer.py','checks/pvr-texture-producer.c','checks/pvr-draw-producer.c','checks/pvr-render-producer.c'];needed=set()
    while pending:
        name=pending.pop()
        if name in needed:continue
        needed.add(name);matches=[v for v in closure if v['path'].endswith('/'+name)]
        require(len(matches)==1,'Reviewed producer closure omits or ambiguously names a project shader input: '+name)
        text=evidence(matches[0]).decode()
        for include in re.findall(r'#include\s+"([^"]+)"',text):
            candidates=[str(Path(name).parent/include),'include/'+include,'checks/'+include]
            for candidate in candidates:
                if (ROOT/candidate).is_file() or any(v['path'].endswith('/'+candidate) for v in closure):pending.append(candidate);break
    require(isinstance(image_raw,bytes) and sha(image_raw)==image_hash,'Exact native bytes required for resident shader comparison.')
    elf=ELFFile(io.BytesIO(image_raw));symbols={v.name:v for v in elf.get_section_by_name('.symtab').iter_symbols()}
    require(set(programs.get('shaders',{}))=={'vertex','fragment'},'Both retained compiler shader outputs required.')
    for name,ref in programs['shaders'].items():
        compiled=evidence(ref);symbol=symbols['texture_'+name];address,size=symbol['st_value'],symbol['st_size']
        loads=[v for v in elf.iter_segments() if v['p_type']=='PT_LOAD' and not v['p_flags']&2 and v['p_vaddr']<=address and address+size<=v['p_vaddr']+v['p_filesz']]
        require(len(loads)==1 and size>0,'Shader is not backed by one immutable upload: '+name)
        offset=address-loads[0]['p_vaddr'];native=loads[0].data()[offset:offset+size]
        require(0<len(compiled)<=len(native) and native[:len(compiled)]==compiled and not any(native[len(compiled):]),'Resident shader differs from retained producer output: '+name)
    return record,cache,b''.join(gpu_digests)

def assemble(hdmi,inputs,record,gpu_digests):
    raw=struct.pack('<3Q',SESSION_MAGIC,1,704+len(inputs))+hdmi+gpu_digests+struct.pack('<Q',record['memory_authority'])+inputs
    require(len(raw)==704+len(inputs)<=1048576,'Combined session size differs.');return raw

def request_bytes(address,build,raw,e,g,l):
    require(848<=len(raw)<=1048576 and struct.unpack_from('<3Q',raw)==(SESSION_MAGIC,1,len(raw)) and
        struct.unpack_from('<3Q',raw,48)==(e,g,l) and raw[72:104]==build and len(build)==32 and
        all(type(v) is int and 0<v<2**64 for v in (e,g,l)) and 0x41000000<=address<=0x411fffe0,'Launch/session identity differs.')
    authority=struct.unpack_from('<Q',raw,696)[0];require(authority,'Missing extended memory authority.');h=hashlib.sha256(raw).digest()
    request=struct.pack('<8Q',REQUEST_MAGIC,2,128,e,g,g^((1<<64)-1),l,address)+build+h
    contract=struct.pack('<6Q',0,2,288,e,g,g^((1<<64)-1))+build
    contract+=struct.pack('<12Q',0,0,0,0,l,0,0,0,0,0,0,0)
    contract+=struct.pack('<6Q',1,authority,0x48000000,0x78000000,0x78000000,0x79000000)+h+raw[104:136]
    require(len(contract)==288,'Contract layout differs.');return request,contract+bytes(4096-len(contract))

def build_launcher(out,clang,lld,objcopy='/usr/lib/llvm-21/bin/llvm-objcopy'):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text();(out/'base-supervised-launch.c').write_text(text)
    replacements={'0x4153373353554c33':hex(REQUEST_MAGIC),'REQUEST[1]==3':'REQUEST[1]==2','CONTRACT->version==3':'CONTRACT->version==2',
        'CONTRACT->bytes==sizeof(*CONTRACT)':'CONTRACT->bytes==288',
        'CONTRACT->reserved[0] && !CONTRACT->reserved[1]':'CONTRACT->reserved[0]==1 && CONTRACT->reserved[1] && CONTRACT->reserved[2]==0x48000000 && CONTRACT->reserved[3]==0x78000000 && CONTRACT->reserved[4]==0x78000000 && CONTRACT->reserved[5]==0x79000000',
        'CONTRACT->reserved[2+i]':'((volatile uint64_t *)((uintptr_t)CONTRACT+224))[i]'}
    for old,new in replacements.items():require(old in text,'Maintained supervised launcher changed: '+old);text=text.replace(old,new)
    (out/'gpu-display-launch-base.c').write_text(text)
    for name,path in [('launch.c',ROOT/'apps/gpu-display/launch.c'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-MD','-MF',out/(name+'.d'),'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands.extend([[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],[objcopy,'-O','binary',out/'launch.elf',out/'launch.bin']])
    save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as log:log.write(r.stdout+r.stderr)
        require(r.returncode==0,'SRAM launcher build failed; inspect retained log.')
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

def prepare(image,manifest,hdmi_session,inputs,admission,output,clang,lld,objcopy):
    image_raw=Path(image).read_bytes();manifest_raw=Path(manifest).read_bytes();hdmi=Path(hdmi_session).read_bytes();data=Path(inputs).read_bytes();entry=Path(admission).read_bytes()
    parts,address,build=image_data(image_raw);m=json.loads(manifest_raw)
    require(m.get('target')=='sunstone-a733-gpu-display' and m.get('execution_profile')==PROFILE and m.get('workload')=='copied-two-frame-HDMI-720p60' and m.get('build_id')==build.hex() and m.get('final_elf')==sha(image_raw),'Exact combined manifest required.')
    record,cache,gpu_digests=validate_admission(entry,hdmi,data,build,sha(image_raw),image_raw=image_raw);raw=assemble(hdmi,data,record,gpu_digests)
    out=Path(output);out.mkdir(parents=True,exist_ok=False);build_launcher(out,clang,lld,objcopy)
    fixed={'image.elf':image_raw,'image-input-manifest.json':manifest_raw,'session.bin':raw,'admission.json':entry};uploads=[]
    for i,part in enumerate(parts):
        if part['data']:
            name=f'segment-{i}.bin';fixed[name]=part['data'];uploads.append({'name':name,'address':part['address']})
    request,contract=request_bytes(address,build,raw,*(record[k] for k in ('epoch','generation','lease')))
    fixed.update({'request.bin':request,'contract.bin':contract,'result-zero.bin':bytes(16384),'report-zero.bin':bytes(128)})
    for name,at in [('session.bin',0x41f00000),('request.bin',0x52c00),('contract.bin',0x42004000),('result-zero.bin',0x42000000),('report-zero.bin',0x52d00),('launch.bin',0x52000)]:uploads.append({'name':name,'address':at})
    evidence={}
    for i,(path,value) in enumerate(cache.items()):
        name=f'entry-evidence-{i}.bin';fixed[name]=value;evidence[path]={'name':name,'sha256':sha(value)}
    for name,value in fixed.items():(out/name).write_bytes(value)
    p={'schema':SCHEMA,'physical_access':False,'image_sha256':sha(image_raw),'build_id':build.hex(),'execution_profile':PROFILE,'session_bytes':len(raw),
        'geometry':list(GEOMETRY),'payloads':uploads,'reservations':[{'address':v['address'],'bytes':v['memory_bytes']} for v in parts],
        'entry_evidence':evidence,'files':{v.name:digest(v) for v in out.iterdir() if v.is_file()}}
    save_json(out/'prepared.json',p);return validate_bundle(out)

def validate_bundle(bundle):
    out=Path(bundle);metadata=(out/'prepared.json').read_bytes();p=json.loads(metadata);require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Unsupported GPU/display bundle.')
    data={}
    for name,expected in p['files'].items():
        require(Path(name).name==name,'Invalid bundle filename.');data[name]=(out/name).read_bytes();require(sha(data[name])==expected,'Frozen bytes changed: '+name)
    parts,address,build=image_data(data['image.elf']);raw=data['session.bin'];m=json.loads(data['image-input-manifest.json'])
    require(p['image_sha256']==sha(data['image.elf'])==m.get('final_elf') and p['build_id']==build.hex()==m.get('build_id') and m.get('target')=='sunstone-a733-gpu-display' and m.get('execution_profile')==p['execution_profile']==PROFILE and m.get('workload')=='copied-two-frame-HDMI-720p60' and p['geometry']==list(GEOMETRY) and p['session_bytes']==len(raw),'Frozen image/profile metadata differs.')
    cache={path:data[ref['name']] for path,ref in p['entry_evidence'].items()}
    require(all(sha(cache[path])==ref['sha256'] for path,ref in p['entry_evidence'].items()),'Frozen proof metadata differs.')
    record,_,digests=validate_admission(data['admission.json'],raw[24:568],raw[704:],build,p['image_sha256'],cache,True,image_raw=data['image.elf'])
    require(raw==assemble(raw[24:568],raw[704:],record,digests),'Frozen session assembly differs.')
    expected=[]
    for i,part in enumerate(parts):
        if part['data']:
            name=f'segment-{i}.bin';require(data[name]==part['data'],'Frozen ELF upload differs.');expected.append({'name':name,'address':part['address']})
    for name,at in [('session.bin',0x41f00000),('request.bin',0x52c00),('contract.bin',0x42004000),('result-zero.bin',0x42000000),('report-zero.bin',0x52d00),('launch.bin',0x52000)]:expected.append({'name':name,'address':at})
    require(p['payloads']==expected and p['reservations']==[{'address':v['address'],'bytes':v['memory_bytes']} for v in parts],'Frozen upload/reservation geometry differs.')
    for name,value in zip(('request.bin','contract.bin'),request_bytes(address,build,raw,*(record[k] for k in ('epoch','generation','lease')))):require(data[name]==value,'Frozen launch binding differs.')
    require(data['result-zero.bin']==bytes(16384) and data['report-zero.bin']==bytes(128) and 0<len(data['launch.bin'])<=0xc00,'Launcher/result extent differs.')
    return {**p,'_frozen':data,'_prepared_sha256':sha(metadata)}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','hdmi-session','inputs','admission','output','clang','lld','objcopy'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();prepare(a.image,a.manifest,a.hdmi_session,a.inputs,a.admission,a.output,a.clang,a.lld,a.objcopy)
