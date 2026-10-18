#!/usr/bin/env python3
"""Check new Bluetooth host callbacks and wrapper admission without device access."""
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sqlite3
import struct
import subprocess
import sys
import types
sys.dont_write_bytecode=True
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def load(path):
    spec=importlib.util.spec_from_file_location('bluetooth_host_check',path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
class Clock:
    value=100.
    def now(self):return self.value
    def pause(self,n):self.value+=max(.001,n)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--inputs',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True)
    p.add_argument('--cc',default='/usr/bin/clang-21');a=p.parse_args();out=a.evidence.resolve();out.mkdir(parents=True)
    host=load(ROOT/'tools/check-a733-bluetooth-supervised.py');shared=host.supervisor
    frame=(a.inputs/'captured-frame.bin').read_bytes();profile=(a.inputs/'captured-profile.bin').read_bytes()
    session=bytearray(592);struct.pack_into('<6Q',session,0,0x4153373342535332,2,592,31,37,41)
    struct.pack_into('<Q',session,312,7);struct.pack_into('<I',session,400,2);session[320:352]=hashlib.sha256(profile).digest();session=bytes(session)
    errors=[]
    for label,check in [('session',lambda:host.preparation.board.session_check(session)),
        ('profile',lambda:host.preparation.profile_check(profile,session)),('actual-result',lambda:host.decoder.decode(frame,session,profile))]:
        try:check()
        except (ValueError,RuntimeError,struct.error) as error:errors.append(label+': '+str(error))
    launcher=out/'launcher';launcher.mkdir()
    (launcher/'board-launch-base.c').write_text('static uint64_t request_model[16];\n#define REQUEST request_model\nstatic unsigned supervisor_calls,refusal_model;\n#define ADMIT(ok,reason) do {if(!(ok)){refusal_model=(reason);return 0;}} while(0)\nint a733_smp_launch_prepare(void){++supervisor_calls;return 1;}\n')
    input_identities={str(p):host.digest(p) for p in [Path(__file__),ROOT/'checks/bluetooth-launcher.c',ROOT/'apps/bluetooth/launch.c',ROOT/'include/a733_board_application.h',*host.software_sources().values(),a.inputs/'captured-frame.bin',a.inputs/'captured-profile.bin',a.inputs/'bluetooth-native.elf',a.inputs/'launcher/launch.bin',launcher/'board-launch-base.c']}
    for i,path in enumerate(input_identities):shutil.copy2(path,out/(f'input-{i}-'+Path(path).name))
    tool_identities={str(Path(name).resolve()):host.digest(Path(name).resolve()) for name in [a.cc,sys.executable]}
    (out/'initial-inputs.json').write_text(json.dumps({'inputs':input_identities,'tools':tool_identities},indent=2)+'\n')
    executable=launcher/'wrapper.elf'
    command=[a.cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-DSUNSTONE_BOARD_SUPERVISED=1',
        '-fsanitize=address,undefined','-Iinclude','-I'+str(launcher),'-MMD','-MF',str(launcher/'wrapper.d'),
        'checks/bluetooth-launcher.c','-o',str(executable)]
    compiled=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
    (launcher/'compile.log').write_text(compiled.stdout+compiled.stderr)
    (launcher/'command.json').write_text(json.dumps(command,indent=2)+'\n')
    if compiled.returncode:errors.append('launcher wrapper compile: '+str(compiled.returncode))
    shared.save_json(out/'setup.json',{'physical_access':False,'errors':errors,'session_bytes':len(session),
        'profile_bytes':len(profile),'authority_boundary':'Host queue/metadata model only; no physical entry validation or transport execution.'})
    assert not errors,'Setup mismatches: '+'; '.join(errors)
    state={'inputs':input_identities,'tools':tool_identities,'physical_access':False,'cases':{},'scope':'New host callbacks over actual C-produced identity; modeled queue/admission boundary; unchanged protected transport is not executed.'}
    def case(name,operation):
        directory=out/name;directory.mkdir();operation(directory);state['cases'][name]='pass';shared.save_json(out/'results.json',state)
    def refused(operation):
        try:operation()
        except (ValueError,RuntimeError,OSError):return
        raise AssertionError('Expected refusal')
    def status(value):
        raw=bytearray(frame[:80]);struct.pack_into('<II',raw,8,value,0);raw[64:80]=bytes(16);return bytes(raw)
    terminal_record={}
    def capture(directory,variation):
        clock=Clock();calls=[];record={'complete':False};committed=[];received=[]
        def exchange(packet,timeout):
            assert packet==bytes(1) and 0<timeout<=500;calls.append(packet)
            raw=status(0) if variation=='pending-success' and len(calls)==1 else status(2) if variation=='failure' else frame
            if variation=='stale':raw=raw[:16]+struct.pack('<Q',99)+raw[24:]
            if variation=='malformed':raw=raw[:-1]
            if variation=='wrong-profile':raw=raw[:32]+bytes(32)+raw[64:]
            if variation=='late':clock.value=200
            if variation=='read-failure':raise OSError('Modeled transfer failure')
            received.append(raw);return raw
        def save():
            if variation=='save-failure' and record.get('bluetooth_result')=='observed-success':raise OSError('Modeled final save failure')
            committed.append(copy.deepcopy(record))
        operation=lambda:host.capture(exchange,session,profile,directory,record,save,150,clock.now,clock.pause)
        if variation in ('pending-success','failure'):operation()
        else:refused(operation)
        assert len(calls)==(2 if variation=='pending-success' else 1)
        expected={'pending-success':'observed-success','failure':'observed-failure'}.get(variation,'unobserved')
        assert record['bluetooth_result']==expected and not record['complete'] and record['bluetooth_retention']=='whole-board-power-removal'
        if expected=='unobserved':assert 'durable_result' not in record and not any(x.get('bluetooth_result')=='observed-success' for x in committed)
        if variation=='pending-success':terminal_record.update(copy.deepcopy(record))
        for i,raw in enumerate(received,1):assert (directory/f'bluetooth-{i:03}.bin').read_bytes()==raw
    for variation in ('pending-success','failure','stale','malformed','wrong-profile','late','read-failure','save-failure'):
        case('capture-'+variation,lambda d,v=variation:capture(d,v))
    def transfer(directory,variation):
        clock=Clock();calls=[];actual=host.capture
        def write(endpoint,packet,timeout):
            assert endpoint==2 and packet==bytes(1);calls.append('write')
            if variation=='write-failure':raise OSError('Modeled write failure')
            if variation=='late':clock.value=200
            return 0 if variation=='zero-length' else 1
        def read(endpoint,length,timeout):
            calls.append('read');assert endpoint==0x81 and length==177;return frame
        with patch.object(host,'capture',lambda *args,**kw:actual(*args,**kw,now=clock.now,pause=clock.pause)),patch.object(host.time,'monotonic',clock.now):
            operation=lambda:host.capture_device(types.SimpleNamespace(write=write,read=read),lambda:None,150,session,profile,directory,{},lambda:None)
            if variation=='success':operation()
            else:refused(operation)
        assert calls==(['write','read'] if variation=='success' else ['write'])
    for variation in ('success','zero-length','late','write-failure'):case('transfer-'+variation,lambda d,v=variation:transfer(d,v))
    # One explicit host-binding model; this metadata is never passed to FEL or
    # physical admission. Actual packaging/admission cases are reused separately.
    bundle=out/'binding-model';bundle.mkdir()
    for name,raw in [('session.bin',session),('profile.bin',profile),('admission.json',b'{}'),('session-template.bin',session),('image.elf',(a.inputs/'bluetooth-native.elf').read_bytes()),('launch.bin',(a.inputs/'launcher/launch.bin').read_bytes())]:
        (bundle/name).write_bytes(raw)
    metadata={'files':{p.name:host.digest(p) for p in bundle.iterdir()},'entry_evidence':{},'image_sha256':host.digest(bundle/'image.elf')}
    shared.save_json(bundle/'prepared.json',metadata)
    def freeze():
        with patch.object(host.preparation,'validate_bundle',lambda _:copy.deepcopy(metadata)):return host.freeze_bundle(bundle)
    prepared=freeze()
    def binding(directory):
        def ref(p):return {'path':str(p.resolve()),'sha256':host.digest(p)}
        proof=directory/'proof';proof.write_text('Host software fixture only; no physical authority.\n')
        software={'schema':'a733-bluetooth-supervised-software-v1','physical_access':False,
            **dict.fromkeys(('admission_cases','launcher_cases','host_capture','host_lifecycle'),True),
            'image':ref(bundle/'image.elf'),'launcher':ref(bundle/'launch.bin'),
            'sources':{name:ref(path) for name,path in host.software_sources().items()},'evidence':[ref(proof)]}
        software_path=directory/'software.json';shared.save_json(software_path,software)
        refs={'supervised-ram-recovery':ref(bundle/'admission.json'),'bluetooth-supervised-software':ref(software_path)}
        submission={'prerequisites':list(refs),'artifacts':[ref(bundle/n) for n in ('prepared.json','image.elf','launch.bin')]}
        dbpath=directory/'queue.db';db=sqlite3.connect(dbpath)
        db.execute('CREATE TABLE settings(key TEXT,value TEXT)');db.execute("INSERT INTO settings VALUES('halted','')")
        db.execute('CREATE TABLE tests(id TEXT,state TEXT,owner TEXT,submission TEXT,review TEXT)')
        db.execute('INSERT INTO tests VALUES(?,?,?,?,?)',('fixture','claimed','owner',json.dumps(submission),json.dumps({'prerequisites':refs})));db.commit()
        host.claimed(dbpath,'fixture','owner',bundle,prepared);refused(lambda:host.claimed(dbpath,'fixture','wrong',bundle,prepared))
        altered=copy.deepcopy(software);name=next(iter(altered['sources']));altered['sources'][name]=ref(proof);refused(lambda:host.validate_software(altered,prepared))
        for name in ('prepared.json','admission.json','image.elf','launch.bin'):
            path=bundle/name;raw=path.read_bytes();path.write_bytes(raw+b'changed')
            try:
                rebound=copy.deepcopy(software)
                if name in ('image.elf','launch.bin'):rebound['image' if name=='image.elf' else 'launcher']=ref(path)
                shared.save_json(software_path,rebound);refs['supervised-ram-recovery']=ref(bundle/'admission.json');refs['bluetooth-supervised-software']=ref(software_path)
                submission['artifacts']=[ref(bundle/n) for n in ('prepared.json','image.elf','launch.bin')]
                db.execute('UPDATE tests SET submission=?,review=?',(json.dumps(submission),json.dumps({'prerequisites':refs})));db.commit()
                refused(lambda:host.claimed(dbpath,'fixture','owner',bundle,prepared))
            finally:path.write_bytes(raw)
        db.close()
    case('queue-source-binding',binding)
    def snapshot(directory):
        path=bundle/'session.bin';raw=path.read_bytes();path.write_bytes(raw+b'changed')
        try:refused(freeze)
        finally:path.write_bytes(raw)
        assert prepared['_frozen']['session.bin']==session
    case('frozen-input',snapshot)
    def routing(directory):
        def runner(args,b,p,ready,entry,capture,**kw):
            assert kw['result_key']=='bluetooth_result' and kw['capture_seconds']==59 and kw['product']==host.PRODUCT and kw['serial']==host.SERIAL
            assert p['_frozen']['session.bin']==session;return 0
        argv=['--bundle',str(bundle),'--fel','never-executed','--evidence',str(directory),'--database','never-opened','--claim','fixture','--owner','fixture','--supervised-manual-reconnect']
        with patch.object(host,'freeze_bundle',lambda _:prepared),patch.object(shared,'run_session',runner):assert host.main(argv)==0
    case('shared-runner-route',routing)
    def completion(directory):
        record=copy.deepcopy(terminal_record);calls=[]
        def transaction(*args):
            calls.append(args);return '0' if args[0]=='readl' else ''
        with patch.object(shared.base,'preload',lambda *a:None):
            shared.launch_once(transaction,lambda started:None,bundle,
                {'epoch':31,'generation':37,'_backup_hashes':{}},directory,record,lambda:None,lambda:None,result_key='bluetooth_result')
        assert record['complete'] and record['manual_reconnect_required'] and calls==[('readl','0x02050014'),('exe','0x52000')]
    case('shared-completion-contract',completion)
    def launcher_case(directory):
        result=subprocess.run([str(executable)],capture_output=True,text=True)
        (directory/'run.log').write_text(result.stdout+result.stderr);result.check_returncode()
    case('launcher-wrapper-admission',launcher_case)
    assert all(host.digest(Path(path))==identity for path,identity in input_identities.items())
    shared.save_json(out/'results.json',state);print(json.dumps({'cases':len(state['cases']),'physical_access':False}))
if __name__=='__main__':main()
