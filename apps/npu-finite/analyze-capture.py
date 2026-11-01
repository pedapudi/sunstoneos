#!/usr/bin/env python3
"""Compare audited stopped NPU RAM offline; never grant execution or recovery."""
import argparse,hashlib,importlib.util,io,json,os,struct
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/"tools"))
from host_evidence import unique_directory
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
host=module('npu_analysis_host',ROOT/'apps/npu-finite/host.py');capture=host.stopped
require,sha=capture.require,capture.sha
REFERENCE_FILES=('comparison.json','buffer-0.bin','buffer-1.bin','buffer-3.bin')
END=struct.pack('<II',0x10000000,0)

def difference(actual,expected):
    offsets=[i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b]
    return {'equal':actual==expected,'actual_bytes':len(actual),'expected_bytes':len(expected),
            'different_bytes':len(offsets)+abs(len(actual)-len(expected)),
            'first_offsets':offsets[:8],'actual_sha256':sha(actual),'expected_sha256':sha(expected)}

def expected_tables():
    # a733_npu_memory.c canonical(): 256 roots, zero padding, one 4KiB-page table.
    raw=bytearray(20480)
    for i in range(256):struct.pack_into('<I',raw,4*i,2)
    struct.pack_into('<I',raw,4*0xe0,(capture.ARENA+4096)|1)
    for i in range(4096):struct.pack_into('<I',raw,4096+4*i,2)
    for offset,size,device,role in capture.SLOTS:
        for delta in range(0,size,4096):
            struct.pack_into('<I',raw,4096+4*((device+delta>>12)&4095),capture.PARENT+offset+delta|3|(4 if role==1 else 0))
    return bytes(raw)

def walk(tables,va):
    # checks/npu-memory.c walk(): only this image's single 4KiB-page table.
    root=struct.unpack_from('<I',tables,(va>>24)*4)[0]
    if root&15!=1:return {'valid':False,'reason':'root flags','root':root}
    table=root&0xffffffc0
    if table!=capture.ARENA+4096:return {'valid':False,'reason':'table outside fixed arena','root':root}
    leaf=struct.unpack_from('<I',tables,table-capture.ARENA+((va>>12)&4095)*4)[0]
    if leaf&3!=3:return {'valid':False,'reason':'leaf flags','leaf':leaf}
    pa=(leaf&0xfffff000)|((leaf&0xff0)<<28)|(va&4095)
    known=any(capture.PARENT+off<=pa<capture.PARENT+off+size for off,size,_,_ in capture.SLOTS)
    return {'valid':known,'physical':pa,'writable':bool(leaf&4),'leaf':leaf,
            'reason':'known slot' if known else 'physical address outside known slots'}

def analyze(bundle,session,assembly,bootstrap,synthetic=False):
    inputs={};report={'physical_access':False,'authorizes_execution':False,'authorizes_recovery':False,
        'evidence_kind':'synthetic decoder fixture' if synthetic else 'supplied capture files; physical origin not independently established',
        'evidence_complete':False,'inputs':inputs,'errors':[],
        'limits':'Internal report and byte consistency only; no operating admission or continuity validation. Returned RAM cannot establish what the engine fetched, cache visibility during execution, or the cause of saturation.'}
    def read(path):
        path=Path(path).resolve();data=path.read_bytes();digest=sha(data)
        require(str(path) not in inputs or inputs[str(path)]==digest,'Input changed while reading.')
        inputs[str(path)]=digest;return data
    try:
        bundle,session,assembly,bootstrap=map(Path,(bundle,session,assembly,bootstrap))
        p=json.loads(read(bundle/'prepared.json'));require(isinstance(p,dict),'Prepared record must be an object.');image=read(bundle/'image.elf')
        require(sha(image)==p['image_sha256'] and p['execution_profile']==host.PROFILE,'Only audited numerical image supported.')
        _,_,_,_,geometry=host.image_profile(image,{})
        require(geometry.get('stopped_capture') is not None,'Unsupported native capture geometry.')
        require(p['geometry']==geometry,'Prepared geometry differs from exact image.')
        original=json.loads(read(session));require(isinstance(original,dict),'Session must be an object.');require(original.get('image_sha256')==sha(image),'Session image differs.')
        result=host.validate_reports(*(read(session.parent/(name+'.bin')) for name in ('monitor','platform','receipt')),p)
        # Records made before the host reported output_pattern lack it; the
        # field is derived from the same copied outputs.
        recorded=original.get('npu_result');recorded=dict(recorded) if isinstance(recorded,dict) else recorded
        if isinstance(recorded,dict):
            for key in ('output_pattern','timing'):recorded.setdefault(key,result.get(key))
        require(result==recorded and result['status']==1 and result['controlled_negative'] and not result['functional_success'],'No internally consistent numerical failure.')
        journal=original.get('stopped_capture',{});require(isinstance(journal,dict),'Capture journal must be an object.');rows=journal.get('files',[])
        require(isinstance(rows,list),'Capture files must be an array.')
        expected=[(name,at,n) for name,at,n in capture.METADATA]+[('parent',capture.PARENT,capture.BYTES)]
        # A fifth row, the NPU SRAM read, is journal-checked when present.
        require(len(rows)<=5,'Unexpected capture journal size.')
        if len(rows)==5:
            path=session.parent/'npu-stopped-sram.bin';data=read(path);at,size=capture.SRAM
            require(rows[4]=={'name':path.name,'address':at,'requested_bytes':size,'bytes':len(data),'sha256':sha(data)} and len(data)==size,'Capture journal differs: sram')
            report['sram_sha256']=sha(data)
        raw={}
        for index,(name,at,size) in enumerate(expected):
            try:
                path=session.parent/('npu-stopped-'+name+'.bin');data=read(path)
                require(index<len(rows),'Missing capture journal entry: '+name);row=rows[index]
                require(row=={'name':path.name,'address':at,'requested_bytes':size,'bytes':len(data),'sha256':sha(data)},'Capture journal differs: '+name)
                require(len(data)==size,'Partial capture: '+name);raw[name]=data
            except (OSError,ValueError) as error:report['errors'].append(str(error))
        require(not report['errors'] and journal.get('complete') is True and journal.get('metadata_validated') is True,'Incomplete or corrupt capture.')
        capture.validate_metadata(raw)
        elf=ELFFile(io.BytesIO(image));symbols={s.name:s for s in elf.get_section_by_name('.symtab').iter_symbols()}
        def symbol(name):
            s=symbols[name];section=elf.get_section(s['st_shndx']);at=s['st_value']-section['sh_addr'];return section.data()[at:at+s['st_size']]
        model=symbol('a733_npu_application_model');input_bytes=symbol('a733_npu_application_input');reference=symbol('a733_npu_application_reference')
        catalogue=json.loads(read(ROOT/'tools/npu-models.json'))['models'];selected=next(v for v in catalogue if v['name']=='conv12')
        require(sha(model)==selected['model']['sha256'] and sha(input_bytes)==selected['input']['sha256'] and sha(reference)==selected['reference']['sha256'],'Catalogue identity differs.')
        refs={}
        for name in REFERENCE_FILES:
            refs[name]=read(assembly/name)
        comparison=json.loads(refs['comparison.json'])
        require(comparison['result']=='pass' and comparison['model_sha256']==sha(model),'Comparison origin differs.')
        for i,slot in ((0,0),(1,1),(3,2),(4,3),(5,4)):
            require(comparison['buffers'][i]=={'device':hex(capture.SLOTS[slot][2]),'bytes':capture.SLOTS[slot][1]},'Accepted reference VA/capacity differs.')
        boot=read(bootstrap);require(208<=len(boot)<=capture.SLOTS[5][1] and len(boot)%8==0 and boot[208:]==END*((len(boot)-208)//8),'Accepted bootstrap differs.')
        parent=raw['parent'];allocations=[parent[off:off+size] for off,size,_,_ in capture.SLOTS]
        expected_buffers=[refs['buffer-0.bin'],refs['buffer-1.bin'],refs['buffer-3.bin'],input_bytes,reference,boot]
        names=('descriptors','coefficients','command','input','output_reference','bootstrap')
        checks={name:difference(actual[:len(expected)],expected) for name,actual,expected in zip(names,allocations,expected_buffers)}
        checks['returned_output']=difference(allocations[4][:400],bytes.fromhex(result['second_output']))
        for index,slot in enumerate((0,1,2,5)):
            checks[names[slot]+'_prepared_digest']={'equal':hashlib.sha256(allocations[slot]).digest()==raw['lengths-buffers'][224+32*index:256+32*index]}
        for slot,prefix in ((2,136),(5,208)):checks[names[slot]+'_END_tail']=difference(allocations[slot][prefix:],END*((len(allocations[slot])-prefix)//8))
        tables=parent[0x104000:];checks['translation_tables']=difference(tables,expected_tables())
        mappings=[]
        for offset,size,device,role in capture.SLOTS:
            bad=[]
            for delta in range(0,size,4096):
                decoded=walk(tables,device+delta)
                if not decoded['valid'] or decoded.get('physical')!=capture.PARENT+offset+delta or decoded.get('writable')!=(role==1):
                    bad.append({'device':device+delta,'expected_physical':capture.PARENT+offset+delta,'actual':decoded})
            mappings.append({'device':device,'pages':size//4096,'different_pages':len(bad),'first_differences':bad[:4]})
        # Directory wire fields from checks/nbg-directory-reference.py, fixed hash only.
        directory,stride,count=struct.unpack_from('<3I',model,48);require((directory,stride,count)==(60,44,13),'Directory differs.')
        sections={}
        for i in range(count):
            kind,lo,hi,nlo,nhi,_,_,_,width,_,_=struct.unpack_from('<11I',model,directory+i*stride)
            at=lo|(hi<<32);size=nlo|(nhi<<32);require(at+size<=len(model),'Section outside fixed model.');sections[kind]=(at,size,width)
        tensors=[]
        for kind,key in ((1,'input_tensor'),(2,'output_tensor')):
            at,_,_=sections[kind];words=list(struct.unpack_from('<12I',model,at));require(words==selected[key],'Serialized tensor differs from catalogue.')
            tensors.append({'kind':key,'dimensions':words[1:1+words[0]],'format':'UINT8','quantization':'affine asymmetric','scale':struct.unpack('<f',struct.pack('<I',words[10]))[0],'zero_point':words[11]})
        # Exact one-NN operation: relocation sites reused from nbg-assembly.py.
        op_at,op_size,op_width=sections[4];operations=list(struct.iter_unpack('<8I',model[op_at:op_at+op_size]))
        require(op_width==32 and operations[1][:6]==(2,0,0,1,1,4),'Fixed NN operation differs.')
        patch_at,patch_size,patch_width=sections[10];require(patch_width==28,'Patch width differs.')
        patches=list(struct.iter_unpack('<7I',model[patch_at:patch_at+patch_size]));addresses=[]
        for kind,destination,source,index,offset,transformed,_ in patches[1:5]:
            slot,at=(2,48+destination) if kind==0 else (0,destination)
            expected_word=struct.unpack_from('<I',expected_buffers[slot],at)[0]
            actual_word=struct.unpack_from('<I',allocations[slot],at)[0]
            decode=(lambda word:word&~63) if kind==0 else (lambda word:(word&0x03ffffff)<<6) if transformed else (lambda word:word)
            actual_va,expected_va=decode(actual_word),decode(expected_word)
            addresses.append({'site':names[slot],'offset':at,'source':source,'actual_word':actual_word,
                'expected_word':expected_word,'actual_device':actual_va,'expected_device':expected_va,
                'equal':actual_word==expected_word,'translation':walk(tables,actual_va)})
        channels=[]
        for i in range(4):
            actual=allocations[4][i*100:(i+1)*100];target=reference[i*100:(i+1)*100]
            channels.append({'channel':i,'minimum':min(actual),'maximum':max(actual),'zeros':actual.count(0),'saturated_high':actual.count(255),'different_from_reference':sum(a!=b for a,b in zip(actual,target))})
        branches=[]
        if any(not checks[k]['equal'] for k in ('descriptors','command','bootstrap')):branches.append('prepared descriptor or command bytes differ')
        if any(not checks[k]['equal'] for k in ('coefficients','input')):branches.append('coefficient or input bytes differ')
        if not checks['translation_tables']['equal']:branches.append('translation entries, permissions or unused table bytes differ')
        if not checks['returned_output']['equal']:branches.append('stopped output differs from native copied output')
        if any(not v['equal'] for k,v in checks.items() if k.endswith('_prepared_digest')):branches.append('prepared digest metadata differs from stopped allocations')
        if not branches:branches.append('prepared backing bytes and copied output agree; engine fetch, visibility and runtime interpretation remain unresolved')
        report.update(evidence_complete=True,comparisons=checks,mappings=mappings,relocated_addresses=addresses,tensors=tensors,output_channels=channels,next_decision=branches,
                      capture_journal_complete=True,numerical_success=False)
    except (OSError,ValueError,KeyError,StopIteration,struct.error) as error:report['errors'].append(str(error))
    # A changed source/capture invalidates conclusions, but originals stay untouched.
    for path,digest in inputs.items():
        try:require(sha(Path(path).read_bytes())==digest,'Input changed before publication: '+path)
        except (OSError,ValueError) as error:report['errors'].append(str(error));report['evidence_complete']=False
    return report

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','session','assembly','bootstrap','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--synthetic',action='store_true',help='Label decoder fixtures; never a physical observation.')
    args=parser.parse_args();args.output=(unique_directory(args.output.parent)/args.output.name if args.output.exists() else args.output)
    report=analyze(args.bundle,args.session,args.assembly,args.bootstrap,args.synthetic)
    with args.output.open('x') as stream:json.dump(report,stream,indent=2);stream.write('\n');stream.flush();os.fsync(stream.fileno())
    return 0 if report['evidence_complete'] else 2
if __name__=='__main__':raise SystemExit(main())
