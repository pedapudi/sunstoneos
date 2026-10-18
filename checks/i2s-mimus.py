#!/usr/bin/env python3
"""Only changed finite capture, WAV handoff and copied transcript boundaries."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def host_observer():
    import struct
    module=load('i2s_observe',ROOT/'apps/i2s/observe.py')
    session=bytearray(640);struct.pack_into('<6Q',session,0,0x4153373341555331,2,640,3,5,7);struct.pack_into('<I',session,552,80000)
    text=b'finite speech.';pcm_hash=hashlib.sha256(bytes(160000)).digest();text_hash=hashlib.sha256(text).digest()
    frame=struct.pack('<6I2Q',0x54533249,1,2,0,len(text),len(text),3,5)+hashlib.sha256(session).digest()
    frame+=struct.pack('<2I',len(text),80000)+pcm_hash+text_hash+text
    assert module.decode_transcript(frame,session,0,len(text),80000)['payload']==text
    for offset in (8,16,24,40,72,76):
        bad=bytearray(frame);bad[offset]^=1
        try:module.decode_transcript(bad,session,0,len(text),80000)
        except ValueError:pass
        else:raise AssertionError('mutated frame accepted: '+str(offset))
    # Duration must match the authenticated session without a caller hint.
    bad=bytearray(frame);struct.pack_into('<I',bad,76,32000)
    try:module.decode_transcript(bad,session,0,len(text))
    except ValueError:pass
    else:raise AssertionError('different admitted duration accepted')
    legacy=bytearray(552);struct.pack_into('<3Q',legacy,0,0x4153373341555331,1,552)
    try:module.decode_transcript(frame,legacy,0,len(text))
    except ValueError:pass
    else:raise AssertionError('legacy session accepted for transcription')
    pending=struct.pack('<6I2Q',0x54533249,1,0,0,0,0,3,5)+hashlib.sha256(session).digest()+bytes(72)
    assert module.decode_transcript(pending,session)['state']==0
    try:module.decode_transcript(pending+text,session)
    except ValueError:pass
    else:raise AssertionError('partial transcript exposed')

def host_capture(out):
    import struct
    from unittest.mock import patch
    host=load('i2s_host_capture',ROOT/'tools/check-a733-i2s-supervised.py')
    session=bytearray(640);struct.pack_into('<6Q',session,0,0x4153373341555331,2,640,3,5,7)
    struct.pack_into('<I',session,552,80000)
    pcm=bytes(i%251 for i in range(160000));text=b'finite speech.\n'
    pcm_hash=hashlib.sha256(pcm).digest();text_hash=hashlib.sha256(text).digest()
    class Device:
        def __init__(self,mode):self.mode=mode;self.held=b'';self.transcript_queries=0;self.part=0
        def write(self,endpoint,packet,timeout):
            assert endpoint==2 and not self.held and timeout>0
            op,offset,count=struct.unpack('<3I',packet)
            if op==3:
                self.transcript_queries+=1;state=0 if self.transcript_queries<=(700 if self.mode==3 else 1) else 2
                payload=text[offset:offset+count] if state==2 else b''
                identity=struct.pack('<2I',len(text),80000)+pcm_hash+text_hash if state==2 else bytes(72)
                if self.mode==1 and state==2:identity=identity[:8]+bytes(32)+identity[40:]
                if self.mode==2 and payload:payload=bytes([payload[0]^1])+payload[1:]
                self.held=struct.pack('<6I2Q',0x54533249,1,state,offset,len(payload),len(text) if state==2 else 0,3,5)+hashlib.sha256(session).digest()+identity+payload
            else:
                assert op in (0,1)
                payload=pcm[offset:offset+count] if op==1 else b''
                self.held=struct.pack('<6I2Q',0x53493241,1,2,offset,len(payload),len(pcm),3,5)+hashlib.sha256(session).digest()+payload
            return 12
        def read(self,endpoint,count,timeout):
            assert endpoint==0x81 and self.held and count>0 and timeout>0
            fragment=min(count,(17,55,32,4192)[self.part%4]);self.part+=1
            answer=self.held[:fragment];self.held=self.held[len(answer):];return answer
    for mode in range(4):
        evidence=out/('host-capture-'+str(mode));evidence.mkdir(exist_ok=True);record={}
        clock=[1.]
        def pause(seconds):clock[0]+=seconds
        with patch.object(host.time,'monotonic',lambda:clock[0]),patch.object(host.time,'sleep',pause):
            try:host.capture_device(Device(mode),lambda:None,60,session,evidence,record,lambda:None,pcm=b'',direction=0,capture_samples=80000)
            except ValueError:assert mode and record['i2s_result']!='observed-success' and not (evidence/'transcription.txt').exists()
            else:
                assert mode in (0,3) and record['i2s_result']=='observed-success'
                if mode==3:assert clock[0]>7.2
                assert (evidence/'pcm-s16le-16000-mono.bin').read_bytes()==pcm
                assert (evidence/'transcription.txt').read_bytes()==text

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--freertos',type=Path,required=True)
    parser.add_argument('--models',type=Path,nargs=2,required=True);parser.add_argument('--resume',action='store_true')
    a=parser.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=a.resume)
    steps=json.loads((out/'steps.json').read_text()) if (out/'steps.json').exists() else {}
    def run(name,command):
        command=list(map(str,command))
        prior=steps.get(name,{})
        if prior.get('exit')==0:
            assert prior['command']==command
            if all(Path(p).is_file() and hashlib.sha256(Path(p).read_bytes()).hexdigest()==h for p,h in prior.get('inputs',{}).items()):return
        result=subprocess.run(command,cwd=ROOT,capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1'})
        log=out/(name+'.log')
        if log.exists():log.rename(out/(name+'-prior.log'))
        log.write_text(result.stdout+result.stderr)
        inputs={(ROOT/Path(p)).resolve() for p in command[1:] if (ROOT/Path(p)).is_file()}
        if '-MF' in command:
            dep=Path(command[command.index('-MF')+1])
            if dep.exists():inputs.update((ROOT/Path(n)).resolve() for n in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]))
        steps[name]={'command':command,'exit':result.returncode,'inputs':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}}
        (out/'steps.json').write_text(json.dumps(steps,indent=2)+'\n')
        assert result.returncode==0,name+': see '+str(log)
    run('port-header',['patch','--silent','--output',out/'portmacro.h',a.freertos/'portable/GCC/ARM_CR82/portmacro.h',ROOT/'patches/freertos-smp-macro.patch'])
    common=['-O1','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_I2S_TRANSCRIBE=1',
        '-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer',
        '-Iapps/i2s','-Iplatform/a733-smp-cached','-Iinclude','-I'+str(a.freertos/'include'),
        '-I'+str(out)]
    # Compile every changed harness input before any case runs.
    c_sources={'client':'apps/i2s/client.c','client-fixture':'checks/i2s-mimus-client.c',
        'admission':'apps/i2s/admission.c','admission-fixture':'checks/i2s-mimus-admission.c','digest':'drivers/storage/storage_digest.c'}
    for name,source in c_sources.items():
        run('compile-'+name,['/usr/bin/clang-21','-std=c11',*common,'-MMD','-MF',out/(name+'.d'),'-c',source,'-o',out/(name+'.o')])
    run('compile-recognition',['/usr/bin/clang++-21','-std=c++23',*common,'-MMD','-MF',out/'recognition.d',
        '-c','checks/i2s-mimus-recognition.cc','-o',out/'recognition.o'])
    run('link-client',['/usr/bin/clang-21','-fsanitize=address,undefined','-Wl,--gc-sections',out/'client.o',out/'client-fixture.o','-o',out/'client'])
    run('link-recognition',['/usr/bin/clang++-21','-fsanitize=address,undefined','-Wl,--gc-sections',
        out/'recognition.o',out/'admission.o',out/'digest.o','-o',out/'recognition'])
    run('link-admission',['/usr/bin/clang-21','-fsanitize=address,undefined','-Wl,--gc-sections',out/'admission-fixture.o',out/'admission.o',out/'digest.o','-o',out/'admission'])
    deps={ROOT/'patches/freertos-smp-macro.patch',a.freertos/'portable/GCC/ARM_CR82/portmacro.h',ROOT/'checks/i2s-mimus.py',ROOT/'apps/i2s/observe.py',ROOT/'apps/i2s/prepare.py',ROOT/'tools/check-a733-i2s-supervised.py'}
    for dep in out.glob('*.d'):
        deps.update((ROOT/Path(n)).resolve() for n in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]))
    manifest={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in deps}
    (out/'dependencies.json').write_text(json.dumps(manifest,indent=2)+'\n')
    # Preserve dependency bytes and executable fixtures before execution.
    snapshot_name='snapshot-'+hashlib.sha256(json.dumps(manifest,sort_keys=True).encode()).hexdigest()[:12]
    snapshot=['python3',ROOT/'tools/evidence-snapshot.py','--output',out/snapshot_name,'--note',
        'Focused finite I2S capture and inference wrapper cases. DMA replies and the recorded engine result are modeled; actual client/WAV/hashing/publication code executes. No hardware or recognition accuracy claim.']
    for p in deps:
        snapshot+=['--source',str(p.relative_to(ROOT))] if ROOT in p.parents else ['--artifact',p]
    for p in [out/'client',out/'recognition',out/'admission',out/'steps.json',*a.models]:snapshot+=['--artifact',p]
    for p in ['/usr/bin/clang-21','/usr/bin/clang++-21']:snapshot+=['--tool',p]
    if not (out/snapshot_name/'manifest.json').exists():run(snapshot_name,snapshot)
    run('admission',[out/'admission',*a.models,out/'session-fixture.bin'])
    for mode in range(5):run('client-'+str(mode),[out/'client',str(mode)])
    for mode in range(5):run('recognition-'+str(mode),[out/'recognition',str(mode),*a.models])
    if not steps.get('observer',{}).get('exit')==0:
        host_observer();steps['observer']={'exit':0};(out/'steps.json').write_text(json.dumps(steps,indent=2)+'\n')
    if not steps.get('host-capture',{}).get('exit')==0:
        host_capture(out);steps['host-capture']={'exit':0};(out/'steps.json').write_text(json.dumps(steps,indent=2)+'\n')
    (out/'result.json').write_text(json.dumps({'passed':True,'hardware_access':False,
        'scope':'Session/model admission, five finite-capture cases, five actual WAV/model/wrapper cases and copied observer/fragmented host capture refusal checks. Existing DMA provider and recognition-engine evidence is reused; their returns are explicit models here.'},indent=2)+'\n')
if __name__=='__main__':main()
