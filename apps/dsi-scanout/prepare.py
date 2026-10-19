#!/usr/bin/env python3
"""Prepare a retained native DSI session from actual panel/PLL/memory authority."""
import argparse,hashlib,importlib.util,io,json,secrets,shutil,struct,subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
board=module('dsi_board_prepare',ROOT/'apps/board/prepare.py');profile=module('dsi_panel_values',Path(__file__).with_name('profile.py'))
base=board.base;require=base.require;SCHEMA='a733-dsi-supervised-bundle-v1';CONTRACT_MAGIC=0x4153373344534331;REQUEST_MAGIC=0x4153373344534c31
FACTS={
'initial':('all_masters_excluded','prior_transactions_drained','prior_walks_drained','inherited_tables_resolved','de_vo_consumers_inactive','remote_secure_writers_excluded','iommu_irqs_owned_masked','dsi_irq_owned_masked','whole_board_power_removal','permanent_retention','no_reuse'),
'memory':('all_ranges_exclusive','no_physical_aliases','iommu_banks_disabled','all_remote_writers_excluded','fetch_padding_qualified'),
'electrical':('de_vo_power_stable','operating_points_qualified','reference_24mhz_stable','qualified_initial_analog_words','complete_initial_pll_provenance','supplies_and_bank_voltages_qualified'),
'panel':('selected_panel_identified','connector_pinout_verified','cable_orientation_verified','exact_power_reset_command_sequence_qualified','pre_video_completion_sufficient','post_video_backlight_sequence_qualified','write_only_initialization_sufficient')}
def session_check(raw):
    require(len(raw)==4096,'DSI session must be4096bytes.');require(board.session_check(raw[:592],(2,))==2,'DSI requires USB0-only mode2.')
    require(struct.unpack_from('<Q',raw,592)[0]==profile.MAGIC and struct.unpack_from('<Q',raw,600)[0] in (1,2) and struct.unpack_from('<Q',raw,608)[0]==4096,'Wrong DSI profile page')
    require(any(raw[704:832]),'Missing DSI facts')
    if struct.unpack_from('<Q',raw,600)[0]==1:require(raw[3584:]==bytes(512),'Nonzero reserved bytes')
    else:require(struct.unpack_from('<4Q',raw,3584)==(profile.REPLAY_MAGIC,1,2,profile.frame_bytes(raw)) and any(raw[3616:3648]) and any(raw[3648:3680]) and raw[3680:]==bytes(416) and 32768+2*profile.frame_bytes(raw)+8192<=0x1000000,'Replay bank geometry/hash differs')
    # The linked C validator performs exact real timing/PLL/pin/packet planning before providers.
    v=struct.unpack_from('<32I',raw,832);require(0<v[28]<=48 and v[29]<=1536 and 10<=v[30]<=16 and 0<v[31]<=8,'Profile exceeds capacities')
    profile.frame_digest(raw)
def image_manifest_check(manifest,image,session):
    require(manifest.get('target')=='sunstone-a733-dsi-scanout' and manifest.get('execution_profile')=='dsi-single-core-supervised-v1' and manifest.get('final_elf')==hashlib.sha256(image).hexdigest(),'Tested DSI image manifest required')
    e=ELFFile(io.BytesIO(image));symbols={s.name:s for s in e.get_section_by_name('.symtab').iter_symbols()}
    for name,value in {'__device_pool_start':0x48000000,'__device_pool_end':0x78000000,'__uncached_pool_start':0x78000000,'__uncached_pool_end':0x79000000,'__device_memory_profile':1}.items():require(name in symbols and int(symbols[name]['st_value'])==value,'Extended DSI linker geometry differs')
    require('sd_single_core_entry' in symbols and int(e['e_entry'])==int(symbols['sd_single_core_entry']['st_value']) and 'FreeRTOS_SGI_Handler' not in symbols,'DSI startup differs')
    if struct.unpack_from('<Q',session,600)[0]==2:
        require(all(name in symbols and int(symbols[name]['st_size'])>0 for name in ('dsi_input_feed','a733_display_native_replay')),'Image lacks immutable replay capability')
    s=symbols.get('dsi_application_geometry');require(s is not None and int(s['st_size'])==64,'DSI immutable marker absent')
    a=int(s['st_value']);parts=[p for p in e.iter_segments() if p['p_type']=='PT_LOAD' and not int(p['p_flags'])&2 and int(p['p_vaddr'])<=a and a+64<=int(p['p_vaddr'])+int(p['p_filesz'])]
    require(len(parts)==1 and parts[0].data()[a-int(parts[0]['p_vaddr']):a-int(parts[0]['p_vaddr'])+64]==struct.pack('<8Q',profile.MAGIC,1,4096,288,256,0x78000000,0x79000000,60),'DSI marker differs');session_check(session)
def validate_admission(record,image_hash,template_hash,raw,cache=None,frozen=False):
    require(record.get('schema')=='a733-dsi-supervised-entry-v1','Wrong DSI admission')
    cache=board.validate_admission({**record,'schema':'a733-board-supervised-entry-v2'},image_hash,template_hash,cache,frozen,'board-single-core-supervised-v1')
    def evidence(item):
        require(isinstance(item,dict) and set(item)=={'path','sha256'} and Path(item['path']).is_absolute(),'Invalid DSI evidence reference')
        path=item['path']
        if path not in cache:require(not frozen,'Frozen DSI evidence absent');cache[path]=Path(path).read_bytes()
        require(hashlib.sha256(cache[path]).hexdigest()==item['sha256'],'DSI evidence changed');return cache[path]
    for i,key in enumerate(FACTS):
        data=evidence(record[key]);fact=json.loads(data)
        require(hashlib.sha256(data).digest()==raw[704+32*i:736+32*i] and fact.get('schema')=='a733-dsi-'+key+'-v1' and fact.get('identity')==record['identity'] and fact.get('port')==record['port'] and fact.get('image_sha256')==image_hash and fact.get('profile_values_sha256')==profile.value_digest(raw) and fact.get('reviewed') is True and fact.get('physical_access_performed') is True,'DSI physical record is not bound to actual selected inputs')
        require(all(fact.get(k) is True for k in FACTS[key]) and fact.get('evidence'),'Required DSI physical facts absent')
        for ref in fact['evidence']:evidence(ref)
        if key=='initial':require(raw[80:112]==hashlib.sha256(data).digest() and fact.get('cold_master')==struct.unpack_from('<Q',raw,192)[0],'DSI initial entry differs')
        if key=='memory':require(fact.get('ranges')==[[0x48000000,0x78000000],[0x78000000,0x79000000]] and fact.get('reservation')==record['reservation'],'Complete784MiB DSI authority required')
        if key=='memory' and struct.unpack_from('<Q',raw,600)[0]==2:require(all(fact.get(k) is True for k in ('rcq_fetches_use_display_master8','rcq_iova_path_qualified','immutable_command_arenas')),'Replay RCQ memory-path authority required')
        if key=='panel':require(isinstance(fact.get('panel_model'),str) and fact['panel_model'].strip() and fact.get('datasheet'),'Selected panel/source required');evidence(fact['datasheet'])
    return cache

def request_bytes(address,identity,raw,epoch,generation,lease,reservation):
    entry=raw[80:112];require(len(identity)==32 and len(raw)==4096 and any(entry) and all(type(x)is int and 0<x<2**64 for x in (epoch,generation,lease,reservation)),'Invalid DSI launch identity')
    request=struct.pack('<8Q',REQUEST_MAGIC,2,128,epoch,generation,generation^((1<<64)-1),lease,address)+identity+entry
    contract=struct.pack('<6Q',0,2,288,epoch,generation,generation^((1<<64)-1))+identity
    contract+=struct.pack('<18Q',0,0,0,0,lease,0,0,0,0,0,0,0,1,reservation,0x48000000,0x78000000,0x78000000,0x79000000)
    contract+=hashlib.sha256(raw).digest()+entry
    require(len(contract)==288,'DSI contract differs');return request,contract+bytes(4096-288)
def build_launcher(out,clang,lld):
    text=(ROOT/'platform/a733-smp-supervised/launch.c').read_text()
    changes={'0x4153373353554c33':hex(REQUEST_MAGIC),'REQUEST[1]==3':'REQUEST[1]==2','CONTRACT->version==3':'CONTRACT->version==2','CONTRACT->bytes==sizeof(*CONTRACT)':'CONTRACT->bytes==288','CONTRACT->reserved[0] && !CONTRACT->reserved[1]':'CONTRACT->reserved[0]==1 && CONTRACT->reserved[1] && CONTRACT->reserved[2]==0x48000000 && CONTRACT->reserved[3]==0x78000000 && CONTRACT->reserved[4]==0x78000000 && CONTRACT->reserved[5]==0x79000000','CONTRACT->reserved[2+i]':'((volatile uint64_t *)((uintptr_t)CONTRACT+256))[i]','20*A733_SMP_COUNTER_HZ':'60*A733_SMP_COUNTER_HZ'}
    for old,new in changes.items():require(old in text,'Supervised launcher source changed');text=text.replace(old,new)
    (out/'dsi-launch-base.c').write_text(text)
    for name,path in [('launch.c',Path(__file__).with_name('launch.c')),('launch.S',ROOT/'platform/a733-smp-observe/launch.S'),('launch.ld',ROOT/'platform/a733-smp-observe/launch.ld'),('boot.h',ROOT/'platform/a733-smp-cached/boot.h'),('a733_board_application.h',ROOT/'include/a733_board_application.h')]:shutil.copy2(path,out/name)
    flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-Os','-g','-Wall','-Wextra','-Werror',f'-DA733_SMP_CONTRACT_MAGIC={hex(CONTRACT_MAGIC)}ULL','-I'+str(out)]
    commands=[[str(clang),*flags,'-MMD','-MF',str(out/(n+'.d')),'-c',str(out/n),'-o',str(out/(n+'.o'))] for n in ('launch.c','launch.S')]
    commands += [[str(lld),'-T',str(out/'launch.ld'),str(out/'launch.c.o'),str(out/'launch.S.o'),'-o',str(out/'launch.elf')],['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',str(out/'launch.elf'),str(out/'launch.bin')]]
    base.save_json(out/'launcher-commands.json',commands)
    for command in commands:
        r=subprocess.run(command,capture_output=True,text=True)
        with (out/'launcher-build.log').open('a') as f:f.write(r.stdout+r.stderr)
        require(r.returncode==0,'DSI launcher build failed; inspect retained log')
    require(0<(out/'launch.bin').stat().st_size<=0xc00,'Launcher exceeds SRAM')
def prepare(image,manifest,admission,template,output,clang,lld,frames=None):
    snapshots={name:Path(path).read_bytes() for name,path in [('image.elf',image),('image-input-manifest.json',manifest),('admission.json',admission),('session-template.bin',template)]}
    raw=bytearray(snapshots['session-template.bin']);session_check(raw)
    replay=struct.unpack_from('<Q',raw,600)[0]==2
    require(bool(frames)==replay,'Exactly two caller frames are required only for replay sessions')
    if replay:
        require(len(frames)==2,'Two immutable frames required')
        for slot,path in enumerate(frames):
            data=Path(path).read_bytes();profile.upload_bytes(raw,slot,data);snapshots[f'frame{slot}.bin']=data
    ih=hashlib.sha256(snapshots['image.elf']).hexdigest();record=json.loads(snapshots['admission.json'])
    image_manifest_check(json.loads(snapshots['image-input-manifest.json']),snapshots['image.elf'],raw)
    cache=validate_admission(record,ih,hashlib.sha256(raw).hexdigest(),raw)
    out=Path(output);out.mkdir(parents=True,exist_ok=False)
    for name,data in snapshots.items():(out/name).write_bytes(data)
    segments,address,identity=base.image_parts(out/'image.elf');epoch,generation,lease=(secrets.randbits(64) or 1 for unused in range(3))
    struct.pack_into('<3Q',raw,24,epoch,generation,lease);raw[48:80]=identity;build_launcher(out,clang,lld)
    uploads=[]
    for i,s in enumerate(segments):
        if s['data']:name=f'segment-{i}.bin';(out/name).write_bytes(s['data']);uploads.append({'name':name,'address':s['address']})
    (out/'session.bin').write_bytes(raw);uploads.append({'name':'session.bin','address':0x41f00000})
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,epoch,generation,lease,record['reservation'])):(out/name).write_bytes(data)
    for name,count in [('result-zero.bin',16384),('report-zero.bin',128)]:(out/name).write_bytes(bytes(count))
    evidence={}
    for i,(path,data) in enumerate(cache.items()):name=f'evidence-{i}.bin';(out/name).write_bytes(data);evidence[path]={'name':name,'sha256':hashlib.sha256(data).hexdigest()}
    p=dict(schema=SCHEMA,physical_access=False,image_sha256=ih,build_id=identity.hex(),epoch=epoch,generation=generation,lease=lease,reservation=record['reservation'],session='session.bin',profile=None,manifest='image-input-manifest.json',image='image.elf',segments=uploads,entry_evidence=evidence,backup_sizes={name:n for name,a,n in base.REGIONS},files={x.name:base.digest(x) for x in out.iterdir() if x.is_file()})
    base.save_json(out/'prepared.json',p);return validate_bundle(out)
def validate_bundle(bundle):
    out=Path(bundle);p=json.loads((out/'prepared.json').read_text());require(p.get('schema')==SCHEMA and p.get('physical_access') is False,'Wrong DSI bundle')
    required={'image.elf','image-input-manifest.json','admission.json','session-template.bin','session.bin','request.bin','contract.bin','launch.bin','result-zero.bin','report-zero.bin'}
    require(required<=set(p['files']) and all(Path(item['name']).name==item['name'] and item['name'] in p['files'] for item in p['entry_evidence'].values()),'Unsealed DSI authority/transport input')
    for name,h in p['files'].items():require(Path(name).name==name and base.digest(out/name)==h,'DSI bundle input changed')
    raw=(out/'session.bin').read_bytes();template=(out/'session-template.bin').read_bytes();session_check(raw)
    cache={path:(out/item['name']).read_bytes() for path,item in p['entry_evidence'].items()}
    require(all(hashlib.sha256(cache[path]).hexdigest()==item['sha256'] for path,item in p['entry_evidence'].items()),'Frozen evidence changed')
    record=json.loads((out/'admission.json').read_text());validate_admission(record,p['image_sha256'],hashlib.sha256(template).hexdigest(),template,cache,True)
    if struct.unpack_from('<Q',raw,600)[0]==2:
        for slot in range(2):
            name=f'frame{slot}.bin';require(name in p['files'],'Replay frame missing');profile.upload_bytes(raw,slot,(out/name).read_bytes())
    else:require(not any(f'frame{slot}.bin' in p['files'] for slot in range(2)),'Ordinary DSI bundle contains replay frames')
    image_manifest_check(json.loads((out/'image-input-manifest.json').read_text()),(out/'image.elf').read_bytes(),raw)
    segments,address,identity=base.image_parts(out/'image.elf');derived=bytearray(template);struct.pack_into('<3Q',derived,24,p['epoch'],p['generation'],p['lease']);derived[48:80]=identity
    require(raw==derived and p['build_id']==identity.hex() and p['reservation']==record['reservation'],'DSI session derivation changed')
    expected=[]
    for i,s in enumerate(segments):
        if s['data']:name=f'segment-{i}.bin';require((out/name).read_bytes()==s['data'],'ELF upload changed');expected.append({'name':name,'address':s['address']})
    expected.append({'name':'session.bin','address':0x41f00000});require(p['segments']==expected and all(x['name'] in p['files'] for x in expected),'Upload spans differ')
    for name,data in zip(('request.bin','contract.bin'),request_bytes(address,identity,raw,p['epoch'],p['generation'],p['lease'],record['reservation'])):require((out/name).read_bytes()==data,'Launch contract differs')
    require((out/'result-zero.bin').read_bytes()==bytes(16384) and (out/'report-zero.bin').read_bytes()==bytes(128) and 0<(out/'launch.bin').stat().st_size<=0xc00 and p['backup_sizes']=={name:n for name,a,n in base.REGIONS},'Protected SRAM geometry differs')
    p['_sizes']={name:(out/name).stat().st_size for name in p['files']};return p
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','admission','template','output','clang','lld'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--frame',type=Path,action='append')
    args=parser.parse_args();prepare(args.image,args.manifest,args.admission,args.template,args.output,args.clang,args.lld,args.frame)
