#!/usr/bin/env python3
"""Check only added DMA0 host authority boundaries using preserved native images.

No native instructions, launcher build, transport or physical qualification run.
Positive cases bypass only explicit modeled/physical decisions. Inherited model
facts remain unchanged; the new sessions do not inherit old runtime results.
"""
import argparse,copy,hashlib,importlib.util,json,shutil,struct,types
from contextlib import ExitStack
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def sha(data):return hashlib.sha256(data).hexdigest()
def encoded(value):return (json.dumps(value,sort_keys=True,indent=2)+'\n').encode()
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('uart','i2s','hdmi','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    # Read and seal the accepted bundle closure before loading host policy.
    originals={};bundles={}
    for kind in ('uart','i2s','hdmi'):
        folder=getattr(a,kind);metadata=(folder/'prepared.json').read_bytes()
        originals[folder/'prepared.json']=metadata;m=json.loads(metadata);data={}
        for name,digest in m['files'].items():
            assert Path(name).name==name
            data[name]=(folder/name).read_bytes();assert sha(data[name])==digest,(kind,name)
            originals[folder/name]=data[name]
        for ref in m.get('entry_evidence',{}).values():assert sha(data[ref['name']])==ref['sha256']
        bundles[kind]=(m,data)
    candidates=set((ROOT/'tools').glob('*.py'))
    for app in ('uart','i2s','board','hdmi','hdmi-audio','dma-copy'):candidates.update((ROOT/'apps'/app).glob('*.py'))
    candidates.add(Path(__file__).resolve())
    source_before={p.resolve():p.read_bytes() for p in candidates}
    modules={kind:module(kind+'_security_check',ROOT/('apps/'+('hdmi-audio' if kind=='hdmi' else kind)+'/prepare.py')) for kind in ('uart','i2s','hdmi')}
    cases=[];overrides=[];inputs={}
    def save(name,data):
        path=(a.output/name).resolve();path.write_bytes(data);return dict(path=str(path),sha256=sha(data))
    note=save('MODEL-NOT-PHYSICAL.json',encoded(dict(modeled=True,physical_authority=False,scope=__doc__)))
    def modeled_require(original,kind,fact):
        def require(condition,message):
            if message=='DMA security requires reviewed physical authority.':
                assert condition is False and fact.get('reviewed') is True and fact.get('modeled') is True and fact.get('physical_authority') is False
                overrides.append(kind);return
            original(condition,message)
        return require
    def no_disk(*args,**kwargs):raise AssertionError('Frozen validation attempted filesystem access')
    for kind,host in modules.items():
        folder=getattr(a,kind);m,frozen=bundles[kind]
        image=frozen['image_elf.bin' if kind=='hdmi' else 'image.elf']
        raw=frozen['source-session.bin' if kind=='hdmi' else 'session-template.bin' if kind=='uart' else 'session.bin']
        admission=json.loads(frozen['admission.bin' if kind=='hdmi' else 'admission.json'])
        inputs[kind]=dict(bundle=str(folder.resolve()),image_sha256=sha(image),prior_session_sha256=sha(raw))
        cache={path:frozen[item['name']] for path,item in m.get('entry_evidence',{}).items()}
        cache[note['path']]=Path(note['path']).read_bytes()
        if kind=='uart':
            host.session_check(raw);ids=(101,103,107)
            _,handoff,controller,evidence=struct.unpack_from('<4Q',raw,1872)
            expected=dict(zip(('epoch','generation','lease'),ids),controller=controller,service_channels=3,
                dma_evidence=evidence,dma_handoff=handoff,configuration_sha256=sha(raw[1824:1920]),session_template_sha256=sha(raw))
            runtime=bytearray(frozen['session.bin']);struct.pack_into('<3Q',runtime,24,*ids)
        elif kind=='i2s':
            _,_,identity,layout,pcm=host.image_data(image);profile=host.session_profile(raw,layout,identity,pcm)
            expected={k:profile[k] for k in ('epoch','generation','lease')}
            expected.update(controller=host.number(raw,layout,'dma.controller'),service_channels=host.number(raw,layout,'dma.service_channels'),
                dma_evidence=host.number(raw,layout,'dma.evidence'),dma_handoff=host.number(raw,layout,'dma.allowed.handoff'),configuration_sha256=sha(host.field(raw,layout,'dma')))
            assert expected['service_channels']==3
        else:
            pcm=frozen['pcm.bin'];fields=host.session_check(raw,pcm)
            expected={k:fields[k] for k in ('epoch','generation','lease')}
            controller,evidence,_,_,channels=struct.unpack_from('<2QI2H',raw,632)
            expected.update(controller=controller,service_channels=channels,dma_evidence=evidence,
                dma_handoff=struct.unpack_from('<Q',raw,608)[0],configuration_sha256=sha(raw[552:656]))
            admission.update(identity=[0,1,2,3,4],port='no-device')
            records={k:frozen[k+'.bin'] for k in ('analog','power','supervision','audio_memory','audio_electrical','sink_edid')}
        expected.update(identity=admission['identity'],port=admission['port'],image_sha256=sha(image))
        security=dict(expected,schema='a733-dma-nonsecure-access-v1',reviewed=True,physical_authority=False,modeled=True,
            nonsecure_channels=expected['service_channels'],evidence=[note],**{k:True for k in host.security_authority.FACTS})
        def invoke(fact,case,*,production=False,missing_support=False,runtime_override=None):
            current=copy.deepcopy(admission);support=dict(cache)
            data=encoded(fact);ref=save(kind+'-'+case+'-security.json',data)
            current['dma_nonsecure_access']=ref;support[ref['path']]=data
            if missing_support:support.pop(note['path'])
            if case=='missing-primary':current.pop('dma_nonsecure_access')
            admission_raw=encoded(current);trial=bytearray(raw)
            if kind=='i2s':
                offset=layout['fields']['evidence']['offset'];trial[offset:offset+32]=hashlib.sha256(admission_raw).digest()
            if kind=='hdmi':trial[80:112]=hashlib.sha256(admission_raw).digest()
            with ExitStack() as stack:
                if not production:stack.enter_context(patch.object(host,'require',modeled_require(host.require,kind,fact)))
                if kind=='hdmi':
                    # These old fixture records are explicitly modeled too.
                    def modeled_hdmi(record):
                        assert record.get('modeled') is True and record.get('physical_authority') is False
                    stack.enter_context(patch.object(host,'physical_authority',modeled_hdmi))
                    facts=dict(records,dma_nonsecure_access=data)
                    if not missing_support:facts['dma_security_evidence_0']=cache[note['path']]
                    stack.enter_context(patch.object(Path,'read_bytes',no_disk))
                    result=host.validate_data(image,bytes(trial),admission_raw,pcm,facts)
                elif kind=='uart':
                    stack.enter_context(patch.object(Path,'read_bytes',no_disk))
                    result=host.validate_admission(current,sha(image),sha(raw),raw,support,True,runtime=runtime_override or runtime)
                else:
                    stack.enter_context(patch.object(Path,'read_bytes',no_disk))
                    result=host.validate_admission(admission_raw,bytes(trial),layout,identity,pcm,sha(image),support,True)
            return result,current,bytes(trial),support
        mutations={'nominal-model':None,'missing-primary':None,'modeled-production':None,
            'wrong-channel-mask':lambda f:f.update(nonsecure_channels=0),
            'wrong-config':lambda f:f.update(configuration_sha256='0'*64),
            'wrong-session':lambda f:f.update(generation=f['generation']+1),
            'missing-post-reset':lambda f:f.pop('valid_after_reset_release'),
            'missing-frozen-support':None}
        if kind=='hdmi':mutations['duplicate-support']=lambda f:f.update(evidence=[note,note])
        if kind=='uart':mutations.update({'boolean-runtime-id':lambda f:f.update(epoch=True),'wrong-template':lambda f:f.update(session_template_sha256='0'*64)})
        errors={'duplicate-support':'Duplicate HDMI audio','missing-primary':'nonsecure authority is missing','modeled-production':'reviewed physical authority',
            'wrong-channel-mask':'omits selected','wrong-config':'digest binding differs','wrong-session':'binding differs: generation',
            'missing-post-reset':'access/lifetime','missing-frozen-support':'Missing frozen','boolean-runtime-id':'binding differs: epoch','wrong-template':'digest binding differs'}
        nominal=None
        for case,mutate in mutations.items():
            fact=copy.deepcopy(security)
            if mutate:mutate(fact)
            try:result=invoke(fact,case,production=case=='modeled-production',missing_support=case=='missing-frozen-support')
            except ValueError as error:
                assert case!='nominal-model' and errors[case] in str(error),(kind,case,str(error))
            else:
                assert case=='nominal-model',(kind,case);nominal=result
            cases.append(kind+':'+case)
        if kind=='uart':
            # The same full record validates before IDs are adopted and then
            # validates actual runtime IDs again; a new runtime cannot substitute.
            _,current,_,support=nominal
            with patch.object(host,'require',modeled_require(host.require,kind,security)),patch.object(Path,'read_bytes',no_disk):
                host.validate_admission(current,sha(image),sha(raw),raw,support,True)
                bad=bytearray(runtime);struct.pack_into('<Q',bad,24,109)
                try:host.validate_admission(current,sha(image),sha(raw),raw,support,True,runtime=bad)
                except ValueError as error:assert 'binding differs: epoch' in str(error)
                else:raise AssertionError('UART runtime IDs escaped reviewed authority')
            cases.append('uart:runtime-revalidation')
            # Exercise the actual ID-adoption producer and its frozen bundle
            # validator, reusing the unchanged accepted launcher bytes.
            admission_path=a.output/'uart-preparation-admission.json';admission_path.write_bytes(encoded(current))
            def launcher(out,*args):shutil.copy2(folder/'launch.bin',out/'launch.bin')
            with patch.object(host,'require',modeled_require(host.require,kind,security)),patch.object(host,'build_launcher',launcher):
                prepared=host.prepare(folder/'image.elf',folder/'image-input-manifest.json',admission_path,
                    folder/'session-template.bin',a.output/'uart-producer-bundle',None,None)
            assert tuple(prepared[k] for k in ('epoch','generation','lease'))==ids
            cases.append('uart:actual-producer-id-adoption-frozen-bundle')
        if kind=='hdmi':
            _,current,trial,_=nominal
            source=a.output/'hdmi-producer-inputs';source.mkdir()
            for key in ('analog','power','supervision','audio_memory','audio_electrical'):
                current[key]=str((source/(key+'.json')).resolve());(source/(key+'.json')).write_bytes(records[key])
            current['sink_edid']['path']=str((source/'sink.edid').resolve());(source/'sink.edid').write_bytes(records['sink_edid'])
            admission_raw=encoded(current);trial=bytearray(trial);trial[80:112]=hashlib.sha256(admission_raw).digest()
            for name,data in (('image.elf',image),('session.bin',bytes(trial)),('admission.json',admission_raw),('input.pcm',pcm)):(source/name).write_bytes(data)
            def modeled_hdmi(record):assert record.get('modeled') is True and record.get('physical_authority') is False
            with patch.object(host,'require',modeled_require(host.require,kind,security)),patch.object(host,'physical_authority',modeled_hdmi):
                (_,address,identity,fields,_),snapshots=host.validate(source/'image.elf',source/'session.bin',source/'admission.json',source/'input.pcm')
                bundle=a.output/'hdmi-frozen-bundle';shutil.copytree(folder,bundle)
                for key,data in snapshots.items():(bundle/('source-session.bin' if key=='session' else key+'.bin')).write_bytes(data)
                (bundle/'session.bin').write_bytes(bytes(trial)+bytes(4096-656))
                request,contract=host.base.request_bytes(address,identity,bytes(trial),fields['epoch'],fields['generation'],fields['lease'],fields['supervisor'])
                (bundle/'request.bin').write_bytes(request);(bundle/'contract.bin').write_bytes(contract)
                metadata=copy.deepcopy(m);metadata['session_sha256']=sha(bytes(trial))
                metadata['files']={p.name:sha(p.read_bytes()) for p in bundle.iterdir() if p.is_file() and p.name!='prepared.json'}
                (bundle/'prepared.json').write_bytes(encoded(metadata));host.validate_bundle(bundle)
            cases.append('hdmi:actual-loader-frozen-bundle')
        inputs[kind]['configuration_sha256']=expected['configuration_sha256']
        assert image==(folder/('image_elf.bin' if kind=='hdmi' else 'image.elf')).read_bytes()
    # Verify the actual physical UART capture and I2S software/source inventories
    # include the newly shared policy; no native or launcher rebuild is needed.
    uart_capture=module('uart_capture_security_check',ROOT/'apps/uart/capture.py')
    i2s_tool=module('i2s_tool_security_check',ROOT/'tools/check-a733-i2s-supervised.py')
    for listing in (uart_capture.source_files(),i2s_tool.source_files()):
        assert any(Path(path).resolve()==(ROOT/'tools/dma-nonsecure-access.py').resolve() for _,path in listing)
    # Determine the imported repository closure, then compare against bytes
    # captured before any policy import. Preserve those exact source bytes.
    visited=set();paths={Path(__file__).resolve()}
    def closure(value):
        if id(value) in visited:return
        visited.add(id(value));filename=getattr(value,'__file__',None)
        if filename:
            path=Path(filename).resolve()
            if path.is_relative_to(ROOT):
                assert path in source_before,path
                paths.add(path)
                for child in vars(value).values():
                    if isinstance(child,types.ModuleType):closure(child)
    for value in (*modules.values(),uart_capture,i2s_tool):closure(value)
    paths.add(ROOT/'apps/dma-copy/observe.py')
    for path,data in originals.items():assert path.read_bytes()==data,path
    for path in paths:assert path.read_bytes()==source_before[path],path
    (a.output/'sources').mkdir()
    for path in paths:
        destination=a.output/'sources'/path.relative_to(ROOT);destination.parent.mkdir(parents=True,exist_ok=True)
        destination.write_bytes(source_before[path])
    result=dict(passed=True,cases=cases,model_authority_overrides=len(overrides),physical_access=False,physical_qualified=False,native_execution=False,launcher_build=False,
        inputs=inputs,sources={str(p.relative_to(ROOT)):sha(source_before[p]) for p in paths},preserved_inputs={str(p):sha(data) for p,data in originals.items()},scope=__doc__)
    (a.output/'result.json').write_bytes(encoded(result));print(json.dumps(dict(passed=True,cases=len(cases),physical_access=False)))
if __name__=='__main__':main()
