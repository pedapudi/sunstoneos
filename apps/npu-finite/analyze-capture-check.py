#!/usr/bin/env python3
"""Synthetic stopped-RAM decoder cases. No hardware, assembler or native replay."""
import argparse,hashlib,importlib.util,io,json,shutil,struct,tempfile
from pathlib import Path
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('analysis',ROOT/'apps/npu-finite/analyze-capture.py');a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--case',action='append',default=[]);args=p.parse_args();args.evidence.mkdir(parents=True,exist_ok=False)
cache=Path.home()/'.cache/sunstoneos/evidence'
source_bundle=cache/'npu-finite-admission-20261024/pll-preserved/root-review-DRAFT/bundle'
original_session=cache/'processor-completion-20261024/port-3-1/npu-numerical/runtime/session.json'
assembly=cache/'npu-models-20261018/conv12/assembly-comparison'
bootstrap=cache/'npu-convolution-20261016/fixtures/expected-bootstrap.bin'
results=[]
with tempfile.TemporaryDirectory(prefix='npu-synthetic-analysis-') as tmp:
    t=Path(tmp);bundle=t/'bundle';bundle.mkdir();run=t/'runtime';run.mkdir()
    for name in ('image.elf','manifest.json'):shutil.copyfile(source_bundle/name,bundle/name)
    image=(bundle/'image.elf').read_bytes();meta=json.loads((bundle/'manifest.json').read_bytes())
    prepared=json.loads((source_bundle/'prepared.json').read_bytes());prepared['geometry']=a.host.image_profile(image,meta)[4]
    (bundle/'prepared.json').write_text(json.dumps(prepared))
    for name in ('monitor','platform','receipt'):shutil.copyfile(original_session.parent/(name+'.bin'),run/(name+'.bin'))
    original=json.loads(original_session.read_bytes());original['synthetic_decoder_fixture']=True
    elf=ELFFile(io.BytesIO(image));symbols={s.name:s for s in elf.get_section_by_name('.symtab').iter_symbols()}
    def symbol(name):
        s=symbols[name];section=elf.get_section(s['st_shndx']);offset=s['st_value']-section['sh_addr'];return section.data()[offset:offset+s['st_size']]
    parent=bytearray(a.capture.BYTES)
    buffers=[(assembly/'buffer-0.bin').read_bytes(),(assembly/'buffer-1.bin').read_bytes(),(assembly/'buffer-3.bin').read_bytes(),symbol('a733_npu_application_input'),bytes.fromhex(original['npu_result']['second_output']),(bootstrap).read_bytes()]
    for slot,data in zip(a.capture.SLOTS,buffers):parent[slot[0]:slot[0]+len(data)]=data
    parent[0x104000:]=a.expected_tables()
    header=bytearray(368);struct.pack_into('<3QI',header,0,2056,432,400,2)
    for i,(off,size,device,role) in enumerate(a.capture.SLOTS):struct.pack_into('<QQQII',header,32+32*i,a.capture.PARENT+off,a.capture.PARENT+off,size,device,role)
    for i,slot in enumerate((0,1,2,5)):
        off,size,_,_=a.capture.SLOTS[slot];header[224+32*i:256+32*i]=hashlib.sha256(parent[off:off+size]).digest()
    struct.pack_into('<2Q',header,352,136,208)
    base={'lengths-buffers':bytes(header),'extent':struct.pack('<3Q',a.capture.PARENT,a.capture.PARENT,a.capture.BYTES),
        'memory':struct.pack('<5Q2I',6,a.capture.ARENA,20480,a.capture.ARENA,20480,(a.capture.ARENA>>8)|1,1),'parent':bytes(parent)}
    mutations={'descriptor-input-alias':(24,struct.pack('<I',0xe0087000)),
        'coefficient':(0x1000,b'\xfe'),'command':(0x2000+68,struct.pack('<I',0xe0085000)),
        'input':(0x82000,b'\xfe'),'output-copy':(0x83000,b'\xfe'),
        'table-permission':(0x105000+4*0x87,struct.pack('<I',a.capture.PARENT+0x83000|3)),
        'table-alias':(0x105000+4*0x87,struct.pack('<I',a.capture.PARENT+0x82000|7)),
        'table-pointer-outside':(0x104000+4*0xe0,struct.pack('<I',0x03600001)),
        'unused-root-padding':(0x104000+1024,b'\x01'),'command-tail':(0x2000+136,b'\x01')}
    cases=['consistent-backing',*mutations,'partial-parent','hash-corrupt','missing-parent','metadata-corrupt','prepared-list','session-null','journal-list','files-null']
    assert not set(args.case)-set(cases)
    for case in cases:
        if args.case and case not in args.case:continue
        (bundle/'prepared.json').write_text(json.dumps([] if case=='prepared-list' else prepared))
        raw={k:bytearray(v) for k,v in base.items()}
        if case in mutations:
            at,value=mutations[case];raw['parent'][at:at+len(value)]=value
        if case=='partial-parent':raw['parent']=raw['parent'][:-1]
        if case=='metadata-corrupt':raw['extent'][0]^=1
        journal={'complete':True,'metadata_validated':True,'files':[]}
        for name,at,size in [*a.capture.METADATA,('parent',a.capture.PARENT,a.capture.BYTES)]:
            path=run/('npu-stopped-'+name+'.bin');path.write_bytes(raw[name]);journal['files'].append({'name':path.name,'address':at,'requested_bytes':size,'bytes':len(raw[name]),'sha256':a.sha(raw[name])})
        if case=='hash-corrupt':journal['files'][-1]['sha256']='0'*64
        if case=='missing-parent':(run/'npu-stopped-parent.bin').unlink()
        if case=='files-null':journal['files']=None
        session={**original,'stopped_capture':[] if case=='journal-list' else journal}
        if case=='session-null':session=None
        (run/'session.json').write_text(json.dumps(session))
        before={str(path):a.sha(path.read_bytes()) for path in run.iterdir()}
        report=a.analyze(bundle,run/'session.json',assembly,bootstrap,synthetic=True)
        assert report['evidence_kind']=='synthetic decoder fixture' and not report['authorizes_execution']
        malformed=case in ('partial-parent','hash-corrupt','missing-parent','metadata-corrupt','prepared-list','session-null','journal-list','files-null')
        assert report['evidence_complete']!=malformed,(case,report['errors'])
        if case=='consistent-backing':
            assert len(report['next_decision'])==1 and 'remain unresolved' in report['next_decision'][0]
            assert all(row['equal'] for row in report['relocated_addresses'])
            assert [v['zeros'] for v in report['output_channels']]==[100,0,0,0]
            assert [v['saturated_high'] for v in report['output_channels']]==[0,100,100,100]
        elif not malformed:
            key={'descriptor-input-alias':'descriptors','coefficient':'coefficients','command':'command','input':'input','output-copy':'returned_output','command-tail':'command_END_tail'}.get(case,'translation_tables')
            assert not report['comparisons'][key]['equal'],case
            if case=='table-pointer-outside':assert all(not v['translation']['valid'] for v in report['relocated_addresses'])
        assert before=={str(path):a.sha(path.read_bytes()) for path in run.iterdir()},'Analyzer mutated evidence'
        (args.evidence/(case+'.json')).write_text(json.dumps(report,indent=2)+'\n')
        results.append({'case':case,'passed':True,'evidence_complete':report['evidence_complete']})
summary={'passed':True,'synthetic':True,'physical_access':False,'native_executed':False,
    'scope':'Constructed stopped RAM; copied historical reports supply the known numerical-failure bytes only. No captured hardware parent exists. Pure decoder comparisons; no native, assembler, capture or admission replay.',
    'cases':results,'sources':{str(p.relative_to(ROOT)):a.sha(p.read_bytes()) for p in (ROOT/'apps/npu-finite/analyze-capture.py',Path(__file__))},'native_image_sha256':a.sha(image)}
(args.evidence/'result.json').write_text(json.dumps(summary,indent=2)+'\n');print('PASS synthetic offline decoder cases:',len(results))
