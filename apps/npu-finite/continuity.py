#!/usr/bin/env python3
"""Derive ROM continuity from a fully restored finite NPU numerical failure.

This offline operation preserves the original session and failed numerical
result. It adds the existing kernel controlled-outcome representation only after
revalidating the frozen bundle, captured reports, uploads and full DRAM restore.
It grants no device, restart, queue or recovery authority.
"""
import argparse,hashlib,importlib.util,json,os
from pathlib import Path

def require(ok,message):
    if not ok:raise ValueError(message)
def sha(data):return hashlib.sha256(data).hexdigest()
def read_input(path):return path.read_bytes()
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def derive(bundle,session,output):
    bundle,session,output=(Path(p).resolve() for p in (bundle,session,output))
    require(not output.exists(),'Continuity output already exists.')
    inputs={}
    def read(path):
        path=path.resolve();data=read_input(path);digest=sha(data)
        require(str(path) not in inputs or inputs[str(path)]==digest,'Input changed during derivation.')
        inputs[str(path)]=digest;return data
    prepared=json.loads(read(bundle/'prepared.json'))
    require(prepared.get('execution_profile')=='npu-finite-return-v1','Only the numerical finite profile is supported.')
    sources=prepared.get('host_sources');require(isinstance(sources,dict) and sources,'Frozen host source closure absent.')
    for name,digest in sources.items():
        path=bundle/'host'/name
        require(not Path(name).is_absolute() and '..' not in Path(name).parts and
                path.resolve().is_relative_to(bundle/'host'),'Invalid frozen host path.')
        require(sha(read(path))==digest,'Frozen host source changed: '+name)
    require('apps/npu-finite/host.py' in sources and 'tools/check-a733-kernel.py' in sources,'Required frozen validators absent.')
    host=module('npu_continuity_frozen_host',bundle/'host/apps/npu-finite/host.py')
    p=host.validate_bundle(bundle)
    require(p['host_sources']==sources,'Frozen host closure differs.')
    for name,digest in p['files'].items():require(sha(read(bundle/name))==digest,'Frozen bundle input changed: '+name)
    original=json.loads(read(session));runtime=session.parent
    require('controlled' not in original and 'continuity_derivation' not in original,'Session already has a controlled outcome.')
    require(original.get('mode')=='kernel' and original.get('execution_profile')==p['execution_profile'] and
            original.get('image_sha256')==p['image_sha256'] and
            all(original.get(k) is True for k in ('complete','launch_attempted','dram_restored','rom_usable','ownership_release_permitted','automatic_access')) and
            all(original.get(k) is False for k in ('dirty','transport_failed','sram_restored')),'Incomplete or uncertain normal restoration.')
    entry=json.loads(p['_frozen']['admission.json'])
    require(original.get('initial_identity')==entry['identity'] and original.get('port')==entry['port'],'Original target differs from frozen admission.')
    for key in ('initial_node','returned_node'):
        path=Path(original.get(key,''));require(path.parent.parent==Path('/dev/bus/usb') and path.parent.name.isdigit() and path.name.isdigit(),'Invalid ROM node.')
    returned=original.get('returned_identity')
    require(isinstance(returned,list) and len(returned)==5 and all(type(v) is int for v in returned) and
            returned!=original['initial_identity'] and original['returned_node']!=original['initial_node'],'Distinct returned ROM identity absent.')
    require(isinstance(original.get('tool_sha256'),str) and len(original['tool_sha256'])==64,'Transport tool identity absent.')
    reports=[read(runtime/(name+'.bin')) for name in ('monitor','platform','receipt')]
    result=host.validate_reports(*reports,p)
    require(result==original.get('npu_result') and result.get('normal_return') is True and
            result.get('status')==1 and result.get('functional_success') is False and
            result.get('controlled_negative') is True and
            original.get('result')=={'magic':0x534b4552,'stage':5,'status':1},'Captured outcome is not the original controlled numerical failure.')
    for segment in p['segments']:
        name=segment['name'];require(read(runtime/('readback-'+name))==p['_frozen'][name],'Uploaded image/session readback differs: '+name)
    require(read(runtime/'readback-launch.bin')==p['_frozen']['launch.bin'],'Launcher readback differs.')
    before,restored=(read(runtime/name) for name in ('before-DRAM.bin','DRAM-restored.bin'))
    require(len(before)==len(restored)==host.DRAM[1] and before==restored and
            sha(before)==original.get('dram_backup_sha256'),'Full DRAM restoration differs.')
    kernel=module('npu_continuity_frozen_kernel',bundle/'host/tools/check-a733-kernel.py')
    markers=['NPU_STAGE=QUIESCENT','FAIL NPU numerical comparison']
    outcome={'name':'npu-numerical-mismatch-after-quiescent-normal-return','status':1,
             'interrupt':None,'fail_lines':0,'fault':None,'log_contains':markers}
    controlled={'outcome':outcome,'faults':kernel.fault_lines(result['log']),
                'log_found':[text for text in markers if text in result['log']]}
    derived={**original,'controlled':controlled}
    require(kernel.controlled_return(derived),'Existing controlled-return validator refused the outcome.')
    read(Path(__file__))
    derived['continuity_derivation']={'kind':'offline-from-validated-capture',
        'original_session':{'path':str(session),'sha256':inputs[str(session)]},
        'inputs':[{'path':path,'sha256':digest} for path,digest in sorted(inputs.items())],
        'scope':'ROM continuity only. Original numerical failure and session remain unchanged; no NPU restart or queue authority.'}
    # Recheck all inputs before publishing an exclusively created output.
    for path,digest in inputs.items():require(sha(read_input(Path(path)))==digest,'Input changed before publication: '+path)
    encoded=(json.dumps(derived,indent=2)+'\n').encode()
    with output.open('xb') as stream:stream.write(encoded);stream.flush();os.fsync(stream.fileno())
    return derived

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','session','output'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();derive(args.bundle,args.session,args.output)
    print('Derived controlled numerical-failure continuity; functional_success remains false.')
if __name__=='__main__':main()
