#!/usr/bin/env python3
"""Build focused finite headset/Mimus fixtures and run only selected new cases."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import wave
ROOT=Path(__file__).resolve().parents[1]
CACHE=Path('/home/sunil/.cache/sunstoneos')
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--setup-only',action='store_true')
p.add_argument('--run-only',action='store_true')
p.add_argument('--resume-setup',action='store_true')
p.add_argument('--capture',type=Path,help='Actual copied PCM from the new modeled headset speech case')
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
cc='/usr/bin/clang-21';cxx='/usr/bin/clang++-21';model_dir=CACHE/'inputs/mimus-bench';mimus=Path('/home/sunil/git/mimus-worktrees/sunstoneos')
commands=[]
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def run(name,args,input=None):
    args=list(map(str,args));commands.append(args)
    r=subprocess.run(args,cwd=ROOT,input=input,capture_output=True,text=True,env={**os.environ,'PYTHONDONTWRITEBYTECODE':'1','ASAN_OPTIONS':'halt_on_error=1'})
    log=out/(name+'.log')
    if log.exists():
        prior=out/'prior'/sha(log)/log.name;prior.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(log,prior)
    log.write_text(r.stdout+r.stderr)
    (out/('runtime-commands.json' if a.run_only else 'setup-commands.json')).write_text(json.dumps(commands,indent=2)+'\n')
    if r.returncode:raise RuntimeError(name+' failed; inspect '+str(log))
    return r
if not a.run_only:
    # Input creation uses one explicit development clip and an independently
    # specified pair average to construct modeled narrowband input, not hardware.
    clip=Path(json.loads((model_dir/'inputs.json').read_text())['audio'])
    with wave.open(str(clip),'rb') as w:
        assert (w.getnchannels(),w.getsampwidth(),w.getframerate())==(1,2,16000)
        raw=w.readframes(48000);assert len(raw)==96000
    def signed(at):return int.from_bytes(raw[at:at+2],'little',signed=True)
    def toward_zero(n):return n//2 if n>=0 else -((-n)//2)
    pcm=b''.join(toward_zero(signed(at)+signed(at+2)).to_bytes(2,'little',signed=True) for at in range(0,len(raw),4))
    (out/'input8k.pcm').write_bytes(pcm)
    samples=[int.from_bytes(pcm[i:i+2],'little',signed=True) for i in range(0,len(pcm),2)]
    expanded=[]
    for i,value in enumerate(samples):expanded.extend((value,toward_zero(value+samples[min(i+1,len(samples)-1)])))
    with wave.open(str(out/'reference16k.wav'),'wb') as w:
        w.setnchannels(1);w.setsampwidth(2);w.setframerate(16000);w.writeframes(b''.join(v.to_bytes(2,'little',signed=True) for v in expanded))
    assert (out/'reference16k.wav').stat().st_size==96044
    sources=['apps/bluetooth/pcm.c','drivers/storage/storage_digest.c','apps/bluetooth/recognition.cc',
             'apps/mimus/recorded.cc','checks/bluetooth-recognition.cc','checks/bluetooth-pcm.c']
    objects=[];failures=[]
    common=['-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',
            '-Ichecks/bluetooth-mimus-fixture','-Iinclude','-I'+str(mimus/'firmware/components/asr/include'),
            '-I'+str(mimus/'firmware/components/pipeline/include'),'-DSUNSTONE_MIMUS_NATIVE=1']
    for i,source in enumerate(sources):
        obj=out/(str(i)+'.o');objects.append(obj)
        compiler=cxx if source.endswith('.cc') else cc
        language=['-std=c++23','-ffp-contract=off','-fno-exceptions','-fno-rtti'] if source.endswith('.cc') else ['-std=c11']
        cmd=[compiler,*common,*language,'-MMD','-MF',out/(str(i)+'.d'),'-c',source,'-o',obj]
        dep=out/(str(i)+'.d')
        if a.resume_setup and obj.exists() and dep.exists():
            names=shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1])
            if all((Path(name) if Path(name).is_absolute() else ROOT/name).stat().st_mtime_ns<=obj.stat().st_mtime_ns for name in names):
                commands.append(list(map(str,cmd)));continue
        try:run('compile-'+str(i),cmd)
        except RuntimeError as e:failures.append(str(e))
    if failures:sys.exit('\n'.join(failures))
    archive=CACHE/'build/mimus-host/asr/libmimus_asr.a'
    run('link-pcm',[cc,'-fsanitize=address,undefined',objects[0],objects[5],'-o',out/'pcm-model'])
    run('link-bridge',[cxx,'-fsanitize=address,undefined',*objects[:5],archive,
                       mimus/'firmware/components/pipeline/src/formatter.cc','-std=c++23','-ffp-contract=off',
                       '-I'+str(mimus/'firmware/components/asr/include'),'-I'+str(mimus/'firmware/components/pipeline/include'),'-o',out/'recognition-model'])
    paths={Path(__file__).resolve(),Path(cc).resolve(),Path(cxx).resolve(),archive.resolve(),clip.resolve(),
           (mimus/'firmware/components/pipeline/src/formatter.cc').resolve(),(CACHE/'build/mimus-host/asr_cli').resolve(),
           out/'input8k.pcm',out/'reference16k.wav',out/'pcm-model',out/'recognition-model',out/'setup-commands.json',model_dir/'inputs.json',ROOT/'apps/bluetooth/recognition-models.json'}
    for dep in [*out.glob('*.d'),*(CACHE/'build/mimus-host/asr/CMakeFiles/mimus_asr.dir').rglob('*.d')]:
        paths.add(dep.resolve())
        for name in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]):
            path=Path(name);paths.add(path.resolve() if path.is_absolute() else (ROOT/path).resolve())
    pin=json.loads((ROOT/'apps/bluetooth/recognition-models.json').read_text())
    for item in pin['models']:
        model=model_dir/item['name'];assert model.stat().st_size==item['bytes'] and sha(model)==item['sha256'];paths.add(model)
    inputs={}
    for path in paths:
        identity=sha(path);copy=out/'inputs'/identity/path.name;copy.parent.mkdir(parents=True,exist_ok=True)
        if not copy.exists():shutil.copyfile(path,copy)
        inputs[str(path)]={'sha256':identity,'snapshot':str(copy.relative_to(out))}
    (out/'setup-inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
if a.setup_only:sys.exit(0)
for path,item in json.loads((out/'setup-inputs.json').read_text()).items():
    assert sha(path)==item['sha256'] and sha(out/item['snapshot'])==item['sha256'],'Changed preflight input: '+path
assert a.capture and a.capture.read_bytes()==(out/'input8k.pcm').read_bytes()
run('converter',[out/'pcm-model'])
for case in ('success','loss','failed-capture','model-hash','deadline','cancel'):
    run(case,[out/'recognition-model',case,a.capture,model_dir/'base.mimus',model_dir/'overlay.mimus',out/(case+'-transcription.log')])
reference=run('reference',[CACHE/'build/mimus-host/asr_cli','--model',model_dir/'base.mimus','--overlay',model_dir/'overlay.mimus','--format','AUTO'],input='headset\t'+str(out/'reference16k.wav')+'\n#end\n')
identity=sha(out/'reference16k.wav')
(out/'reference.json').write_text(json.dumps({'exit':0,'bundle_sha256':identity,'stdout':reference.stdout})+'\n')
(out/'candidate.json').write_text(json.dumps({'exit':0,'bundle_sha256':identity,'image_sha256':'host-composition','stderr':(out/'success-transcription.log').read_text()})+'\n')
run('parity',['/usr/bin/python3','checks/mimus-parity.py','--reference',out/'reference.json','--candidate',out/'candidate.json','--output',out/'parity.json'])
# Native C++ snapshot binds exact converted WAV bytes, not original16k parity.
import struct
snapshot=(out/'success-transcription.log.snapshot.bin').read_bytes();assert len(snapshot)==144
assert struct.unpack_from('<2I',snapshot,36)==(24000,48000)
assert snapshot[48:80]==hashlib.sha256(a.capture.read_bytes()).digest()
assert snapshot[80:112]==hashlib.sha256((out/'reference16k.wav').read_bytes()).digest()
print('PASS finite headset PCM→conversion→unchanged recognition/formatting and matching converted-input host parity; hardware/scheduler modeled')
