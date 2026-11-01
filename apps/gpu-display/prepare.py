#!/usr/bin/env python3
"""Prepare a bounded GPU-to-HDMI RAM bundle offline; no device access."""
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
hdmi_clock=module('gpu_display_hdmi_clock_admission',ROOT/'apps/hdmi/prepare.py')
require,save_json=gpu.require,gpu.save_json
def sha(raw):return hashlib.sha256(raw).hexdigest()
def digest(path):return sha(Path(path).read_bytes())
SCHEMA='a733-gpu-display-supervised-bundle-v2'
ADMISSION='a733-gpu-display-supervised-admission-v2'
PROFILE='gpu-display-supervised-v2'
SESSION_MAGIC=0x4153373347444131
CONTRACT_MAGIC=0x4153373347444331
REQUEST_MAGIC=0x4153373347444c31
GEOMETRY=(SESSION_MAGIC,2,840,288,0x41f00000,1048576,20,544,568,144,1280,720,704,136)
INPUT_OFFSET=840
USB_SUPPLIES=(3300000,3300000,1200000,900000)
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
 'gpu_supervision':{'manual-whole-board-power-removal','retain-all-resources-until-power-removal','stop-after-first-failure','combined-gpu-hdmi-lifetime'},
 'usb_initial':{'rom-pio-lifetime-owned','external-usb-dma-excluded','usb-irqs-188-189-190-owned-quiet','no-concurrent-rom-access'},
 'usb_electrical':{'four-usb-rails-qualified','stable-shared-fabric-clock','dedicated-retained-output-sink'},
 'usb_supervision':{'manual-whole-board-power-removal','retain-all-resources-for-complete-20-second-lifetime','stop-after-first-failure','one-attempt'}}

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
    require(struct.unpack('<14Q',read('gpu_display_geometry',112)[1])==GEOMETRY,'Unsupported native geometry.')
    for name,address in {'__device_pool_start':0x48000000,'__device_pool_end':0x78000000,'__uncached_pool_start':0x78000000,'__uncached_pool_end':0x79000000,'__a733_smp_result':0x42000000,'__a733_smp_contract':0x42004000,'__a733_smp_contract_end':0x42005000}.items():
        require(name in symbols and symbols[name]['st_value']==address,'Linker reservation differs: '+name)
    require(elf.has_dwarf_info(),'Native DWARF ABI information required.')
    wanted={'gpu_display_session':(984,{'hdmi':24,'gpu_initial':568,'gpu_memory':600,'gpu_electrical':632,'gpu_supervision':664,'memory_authority':696,'usb':704,'inputs':840}),
        'hdmi_session':(544,{'image':48,'evidence':80,'analog':112,'power':144,'supervision':176,'supervisor':208,'owner':216,'pck_client':224,'request':232,'pmic':304,'pins':336,'framebuffer':440}),
        'a733_hdmi_request':(72,{'phy_reference_hz':68}),
        'hdmi_framebuffer_session':(104,{'isolation':0,'memory':32,'authority':64}),
        'gpu_display_usb_authority':(136,{'owner':0,'handoff':8,'initial':16,'electrical':48,'supervision':80,'cold_reference':112,'reserved':116,'supply_uv':120}),
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
    require(len(raw)>=144 and struct.unpack_from('<Q2I',raw)==(packer.MAGIC,1,len(raw)) and len(raw)<=1048576-INPUT_OFFSET,'Invalid packed application extent.')
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
    require(next_offset==len(raw) and packer.inputs(frames,limit=1048576-INPUT_OFFSET)==raw,'Noncanonical packed texture input.')
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



def validate_admission(admission_raw,hdmi,inputs,build,image_hash,cache=None,frozen=False,*,image_raw=None):
    profile=session_profile(hdmi);validate_inputs(inputs)
    require(hdmi[48:80]==build,'HDMI session belongs to another native build.')
    require(all(any(hdmi[o:o+32]) for o in (80,112,144,176,440,472)),
        'Native HDMI identity fields must be nonzero.')
    require(struct.unpack_from('<I',hdmi,300)[0] in (24000000,26000000),
        'Unsupported HDMI PHY reference clock.')
    record=json.loads(admission_raw)
    memory=record.get('memory_authority',profile['framebuffer_authority'])
    occupied=(profile['owner'],struct.unpack_from('<Q',hdmi,304)[0],9,10,11)
    owner=record.get('usb_owner',next(v for v in range(1,64) if v not in occupied))
    handoff=record.get('usb_handoff',profile['lease'])
    require(all(type(v) is int and 0<v<2**64 for v in (memory,owner,handoff)) and owner not in occupied,
        'Invalid combined memory or USB ownership.')
    require(record.get('usb_supply_uv',list(USB_SUPPLIES))==list(USB_SUPPLIES),
        'USB supply voltages differ from the supported operating point.')
    require(record.get('session_seconds',20)==20,'Unsupported native lifetime.')
    record={**profile,'memory_authority':memory,'usb_owner':owner,'usb_handoff':handoff}
    seed=hdmi+inputs
    gpu_digests=b''.join(hashlib.sha256(name.encode()+seed).digest() for name in
        ('gpu_initial','gpu_memory','gpu_electrical','gpu_supervision'))
    usb_digests=b''.join(hashlib.sha256(name.encode()+seed).digest() for name in
        ('usb_initial','usb_electrical','usb_supervision'))
    usb=struct.pack('<2Q',owner,handoff)+usb_digests+struct.pack('<2I4I',2,0,*USB_SUPPLIES)
    return record,{},gpu_digests,usb

def assemble(hdmi,inputs,record,gpu_digests,usb):
    raw=struct.pack('<3Q',SESSION_MAGIC,2,INPUT_OFFSET+len(inputs))+hdmi+gpu_digests+struct.pack('<Q',record['memory_authority'])+usb+inputs
    require(len(raw)==INPUT_OFFSET+len(inputs)<=1048576 and len(usb)==136,'Combined session size differs.');return raw

def request_bytes(address,build,raw,e,g,l):
    require(984<=len(raw)<=1048576 and struct.unpack_from('<3Q',raw)==(SESSION_MAGIC,2,len(raw)) and
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
    for name,path in [('launch.c',ROOT/'apps/gpu-display/launch.c'),('usb-authority.h',ROOT/'apps/gpu-display/usb-authority.h'),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror',f'-DA733_SMP_CONTRACT_MAGIC={CONTRACT_MAGIC}ULL','-I'+str(out)]
    commands=[[clang,*flags,'-MD','-MF',out/(name+'.d'),'-c',out/name,'-o',out/(name+'.o')] for name in ('launch.c','launch.S')]
    commands.extend([[lld,'-T',out/'launch.ld',out/'launch.c.o',out/'launch.S.o','-o',out/'launch.elf'],[objcopy,'-O','binary',out/'launch.elf',out/'launch.bin']])
    save_json(out/'launcher-commands.json',[[str(x) for x in c] for c in commands])
    for command in commands:
        r=subprocess.run(list(map(str,command)),capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as log:log.write(r.stdout+r.stderr)
        require(r.returncode==0,'SRAM launcher build failed; inspect retained log.')
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM.')

output_directory=gpu.output_directory

def prepare(image,manifest,hdmi_session,inputs,admission,output,clang,lld,objcopy):
    image_raw=Path(image).read_bytes();manifest_raw=b'{}';hdmi=Path(hdmi_session).read_bytes();data=Path(inputs).read_bytes();entry=Path(admission).read_bytes() if admission else b'{}'
    parts,address,build=image_data(image_raw);m=json.loads(manifest_raw)
    record,cache,gpu_digests,usb=validate_admission(entry,hdmi,data,build,sha(image_raw),image_raw=image_raw);raw=assemble(hdmi,data,record,gpu_digests,usb)
    out=output_directory(output);build_launcher(out,clang,lld,objcopy)
    fixed={'image.elf':image_raw,'image-input-manifest.json':manifest_raw,'session.bin':raw,'admission.json':entry};uploads=[]
    for i,part in enumerate(parts):
        if part['data']:
            name=f'segment-{i}.bin';fixed[name]=part['data'];uploads.append({'name':name,'address':part['address']})
    segments=uploads+[{'name':'session.bin','address':0x41f00000}]
    request,contract=request_bytes(address,build,raw,*(record[k] for k in ('epoch','generation','lease')))
    fixed.update({'request.bin':request,'contract.bin':contract,'result-zero.bin':bytes(16384),'report-zero.bin':bytes(128)})
    for name,at in [('session.bin',0x41f00000),('request.bin',0x52c00),('contract.bin',0x42004000),('result-zero.bin',0x42000000),('report-zero.bin',0x52d00),('launch.bin',0x52000)]:uploads.append({'name':name,'address':at})
    evidence={}
    for i,(path,value) in enumerate(cache.items()):
        name=f'entry-evidence-{i}.bin';fixed[name]=value;evidence[path]={'name':name,'sha256':sha(value)}
    for name,value in fixed.items():(out/name).write_bytes(value)
    p={'schema':SCHEMA,'physical_access':False,'image_sha256':sha(image_raw),'build_id':build.hex(),'execution_profile':PROFILE,'session_bytes':len(raw),
        **session_profile(hdmi),'memory_authority':record['memory_authority'],'usb_owner':record['usb_owner'],'usb_handoff':record['usb_handoff'],
        'geometry':list(GEOMETRY),'segments':segments,'payloads':uploads,'reservations':[{'address':v['address'],'bytes':v['memory_bytes']} for v in parts],
        'entry_evidence':evidence,'files':{name:digest(out/name) for name in {'launch.bin', 'contract.bin', 'session.bin', 'request.bin', 'report-zero.bin', 'image.elf', 'result-zero.bin'}|{v['name'] for v in segments}}}
    save_json(out/'prepared.json',p);return validate_bundle(out)

def native_configuration(raw):
    """Read the consumed USB and memory inputs from the native session."""
    require(len(raw)>=INPUT_OFFSET,'Short GPU/display native session.')
    return json.dumps({'memory_authority':struct.unpack_from('<Q',raw,696)[0],
        'usb_owner':struct.unpack_from('<Q',raw,704)[0],
        'usb_handoff':struct.unpack_from('<Q',raw,712)[0],
        'usb_supply_uv':list(struct.unpack_from('<4I',raw,824))}).encode()

def validate_bundle(bundle):
    out=Path(bundle);metadata=(out/'prepared.json').read_bytes();p=json.loads(metadata);require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Unsupported GPU/display bundle.')
    runtime={'report-zero.bin', 'result-zero.bin', 'contract.bin', 'launch.bin', 'session.bin', 'request.bin', 'image.elf'}|{v['name'] for v in p['segments']}
    p['files']={name:p['files'][name] for name in runtime};p['entry_evidence']={}
    data={}
    for name,expected in p['files'].items():
        require(Path(name).name==name,'Invalid bundle filename.');data[name]=(out/name).read_bytes();require(sha(data[name])==expected,'Frozen bytes changed: '+name)
    parts,address,build=image_data(data['image.elf']);raw=data['session.bin']
    require(p['image_sha256']==sha(data['image.elf']) and p['build_id']==build.hex() and p['execution_profile']==PROFILE and p['geometry']==list(GEOMETRY) and p['session_bytes']==len(raw),'Frozen image/profile metadata differs.')
    cache={}
    record,_,digests,usb=validate_admission(native_configuration(raw),raw[24:568],raw[INPUT_OFFSET:],build,p['image_sha256'],cache,True,image_raw=data['image.elf'])
    require(struct.unpack_from('<3Q',raw)==(SESSION_MAGIC,2,len(raw)) and
        all(any(raw[o:o+32]) for o in (568,600,632,664,720,752,784)) and
        raw[816:824]==struct.pack('<2I',2,0),'Native GPU/display envelope or USB record differs.')
    require(all(p.get(k)==v for k,v in session_profile(raw[24:568]).items()) and
        all(p.get(k)==record[k] for k in ('memory_authority','usb_owner','usb_handoff')),'Frozen lifetime/owner metadata differs.')
    expected=[]
    for i,part in enumerate(parts):
        if part['data']:
            name=f'segment-{i}.bin';require(data[name]==part['data'],'Frozen ELF upload differs.');expected.append({'name':name,'address':part['address']})
    require(p['segments']==expected+[{'name':'session.bin','address':0x41f00000}],'Sealed supervisor segment list differs.')
    for name,at in [('session.bin',0x41f00000),('request.bin',0x52c00),('contract.bin',0x42004000),('result-zero.bin',0x42000000),('report-zero.bin',0x52d00),('launch.bin',0x52000)]:expected.append({'name':name,'address':at})
    require(p['payloads']==expected and p['reservations']==[{'address':v['address'],'bytes':v['memory_bytes']} for v in parts],'Frozen upload/reservation geometry differs.')
    for name,value in zip(('request.bin','contract.bin'),request_bytes(address,build,raw,*(record[k] for k in ('epoch','generation','lease')))):require(data[name]==value,'Frozen launch binding differs.')
    require(data['result-zero.bin']==bytes(16384) and data['report-zero.bin']==bytes(128) and 0<len(data['launch.bin'])<=0xc00,'Launcher/result extent differs.')
    return {**p,'_frozen':data,'_sizes':{name:len(value) for name,value in data.items()},'_prepared_sha256':sha(metadata)}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','hdmi-session','inputs','output','clang','lld','objcopy'):parser.add_argument('--'+name,type=Path,required=True)
    for name in ('manifest','admission'):parser.add_argument('--'+name,type=Path)
    a=parser.parse_args();prepare(a.image,a.manifest,a.hdmi_session,a.inputs,a.admission,a.output,a.clang,a.lld,a.objcopy)
