#!/usr/bin/env python3
"""Check preparation and single-attempt transfer ordering with a host-only fake."""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import sqlite3
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / 'tools/check-a733-smp.py'
    spec = importlib.util.spec_from_file_location('smp_host',source)
    host = importlib.util.module_from_spec(spec); spec.loader.exec_module(host)
    prepared = host.validate_bundle(args.bundle)
    cases = []

    def refused(name, operation):
        try: operation()
        except (ValueError, OSError, KeyError): cases.append({'case':name,'result':'refused'})
        else: raise AssertionError('Accepted '+name)

    def protocol(name, *, fail=None, corrupt=False, active=False, high=False, queue_lost=False, return_failure=False):
        with tempfile.TemporaryDirectory() as scratch:
            evidence = Path(scratch); calls = []; memory = {}; record = {'launch_attempted':False}; executed = []; returned = []
            def save(): (evidence/'record.json').write_text(json.dumps(record))
            def ready():
                if queue_lost: raise ValueError('Queue claim changed')
            def transaction(*arguments):
                calls.append(tuple(str(x) for x in arguments))
                if len(calls) == fail: raise OSError('injected transport failure')
                op = arguments[0]
                assert not executed, 'Protocol request after exe before distinct-instance check'
                if op == 'read':
                    _, address, length, destination = arguments
                    Path(destination).write_bytes(bytes(length)); return ''
                if op == 'readl':
                    if len(arguments) == 8: return '100 100 100 100' if high else '0 0 0 0'
                    assert arguments == ('readl','0x02050014'); return '41' if active else '0'
                if op == 'write':
                    _, address, file, read, read_address, size, destination = arguments
                    assert read == 'read' and address == read_address and size == Path(file).stat().st_size
                    data = Path(file).read_bytes(); memory[int(address,16)] = data
                    Path(destination).write_bytes(bytes([data[0]^1])+data[1:] if corrupt else data); return ''
                assert arguments == ('exe','0x52000')
                intent = json.loads((evidence/'record.json').read_text())
                assert intent['launch_attempted'] and intent['execution_intent']['entry'] == 0x52000
                assert memory[0x42000000] == bytes(16384)
                assert memory[host.CONTRACT] == (args.bundle/'contract.bin').read_bytes()
                assert memory[0x52800] == (args.bundle/'request.bin').read_bytes()
                assert memory[0x52000] == (args.bundle/'launch.bin').read_bytes()
                for item in prepared['segments']: assert memory[item['address']] == (args.bundle/item['name']).read_bytes()
                executed.append(True); return ''
            def wait_return():
                assert len(executed) == 1
                if return_failure: raise ValueError('No distinct ROM instance')
                returned.append(True)
            error = None
            try: host.launch_once(transaction,wait_return,args.bundle,prepared,evidence,record,save,ready)
            except (OSError,ValueError) as caught: error = str(caught)
            expected_error = any((fail,corrupt,active,high,queue_lost,return_failure))
            assert bool(error) == bool(expected_error), (name,error)
            if fail: assert len(calls) == fail
            if not expected_error:
                assert len(executed)==1 and returned and not record['complete'] and record['scheduler_result']=='unobserved'
            if corrupt or active or high or queue_lost: assert not executed and not record['launch_attempted']
            if return_failure: assert executed and record['launch_attempted'] and not record.get('launch_protocol_complete')
            cases.append({'case':name,'result':'refused' if error else 'protocol-complete-scheduler-unobserved','requests':len(calls)})
            return len(calls)

    class Held(Exception): pass
    for phase in ('save', 'quarantine'):
        calls = []
        def save_failure():
            calls.append('save')
            if phase == 'save': raise OSError('disk failure')
        def quarantine_failure():
            calls.append('quarantine')
            if phase == 'quarantine': raise OSError('quarantine failure')
        def hold(): calls.append('hold'); raise Held()
        try: host.stop_after_failure({'launch_attempted':True},save_failure,quarantine_failure,RuntimeError('transport failure'),hold)
        except Held: pass
        else: raise AssertionError('Released live ownership after '+phase+' failed')
        assert calls[-1] == 'hold'
        cases.append({'case':phase+' failure still retains live ownership','result':'held'})

    count = protocol('complete preloading and one execution')
    for index in range(1,count+1): protocol(f'transport failure at request {index}',fail=index)
    protocol('corrupt upload',corrupt=True); protocol('watchdog active',active=True)
    protocol('physical input high',high=True); protocol('queue claim changed before execution',queue_lost=True)
    protocol('ROM observation timeout retains execution intent',return_failure=True)
    with tempfile.TemporaryDirectory() as scratch:
        root = Path(scratch); bundle = root/'bundle'; shutil.copytree(args.bundle,bundle)
        metadata = json.loads((bundle/'prepared.json').read_text())
        metadata['segments'][0]['address'] += 8
        (bundle/'prepared.json').write_text(json.dumps(metadata))
        refused('redirected image segment',lambda: host.validate_bundle(bundle))
        metadata = json.loads((args.bundle/'prepared.json').read_text())
        (bundle/'prepared.json').write_text(json.dumps(metadata))
        content = bytearray((bundle/'contract.bin').read_bytes()); content[0]=1
        (bundle/'contract.bin').write_bytes(content); metadata['files']['contract.bin']=host.digest(bundle/'contract.bin')
        (bundle/'prepared.json').write_text(json.dumps(metadata))
        refused('published contract despite recomputed file hash',lambda: host.validate_bundle(bundle))
        proof=root/'proof'; proof.write_text('synthetic host fixture; never recovery qualification')
        artifact={'path':str(proof),'sha256':host.digest(proof)}
        qualification={'schema':'a733-independent-reset-v1','qualified':True,'target':{'port':'fake','sysfs':'/fake','soc':'00001903'},
                       'watchdog':{'mode':0x41,'configuration':1,'output':2,'source':'rtc32k','clock_hz':32768},
                       'physical_counter':{'clock_hz':24000000,'continuous_across_rmr':True},'rom_sha256':host.ROM_HASH,
                       'cases':{name:artifact for name in ['clock-source','masked-cpu-expiry','whole-system-reset','protected-rom-return','interconnect-stall-recovery']}}
        host.validate_qualification(qualification,'fake','/fake')
        for label,key,value in [('unqualified watchdog','qualified',False),('counter continuity absent','physical_counter',{}),('missing independent cases','cases',{}),('wrong ROM','rom_sha256','0'*64)]:
            trial=dict(qualification); trial[key]=value
            refused(label,lambda:host.validate_qualification(trial,'fake','/fake'))
        refused('wrong physical port',lambda:host.validate_qualification(qualification,'other','/fake'))
        software={'schema':'a733-smp-launcher-software-v1','host_protocol':True,'instruction_model':True,'physical_access':False,
                  'host_tool':{'path':str(source),'sha256':host.digest(source)},'evidence':[artifact]}
        for key,name in [('prepared','prepared.json'),('image','image.elf'),('launcher','launch.bin')]:
            path=(args.bundle/name).resolve(); software[key]={'path':str(path),'sha256':host.digest(path)}
        host.validate_software(software,args.bundle)
        wrong=dict(software); wrong['launcher']=artifact
        refused('software evidence for another launcher',lambda:host.validate_software(wrong,args.bundle))
        paths={}
        for name,value in [('qualification',qualification),('input',{}),('ddr',{}),('software',software)]:
            path=root/(name+'.json'); path.write_text(json.dumps(value)); paths[name]=path
        prerequisites=dict(zip(['independent-watchdog-rom-return','stable-protected-fel','ddr-reservation','smp-launcher-software'],[{'path':str(paths[x]),'sha256':host.digest(paths[x])} for x in ['qualification','input','ddr','software']]))
        submission={'prerequisites':list(host.PREREQUISITES),'artifacts':[software[key] for key in ['prepared','image','launcher']]}
        dbpath=root/'queue.db'; db=sqlite3.connect(dbpath)
        db.executescript('CREATE TABLE settings(key TEXT,value TEXT); CREATE TABLE tests(id TEXT,state TEXT,owner TEXT,submission TEXT,review TEXT);')
        db.execute('INSERT INTO settings VALUES(?,?)',('halted',''))
        db.execute('INSERT INTO tests VALUES(?,?,?,?,?)',('test','claimed','coordinator',json.dumps(submission),json.dumps({'prerequisites':prerequisites}))); db.commit()
        def claim(): host.claimed(dbpath,'test','coordinator',args.bundle,paths['qualification'],paths['input'],paths['ddr'])
        claim(); cases.append({'case':'exact software and independent prerequisites accepted','result':'host fixture only'})
        db.execute("UPDATE tests SET owner='other'"); db.commit(); refused('foreign queue ownership',claim)
        db.execute("UPDATE tests SET owner='coordinator'"); db.execute("UPDATE settings SET value='halted'"); db.commit(); refused('halted queue',claim); db.close()
    result={'physical_access':False,'host_tool_sha256':host.digest(source),'check_sha256':host.digest(Path(__file__)),
            'prepared_sha256':host.digest(args.bundle/'prepared.json'),'cases':cases,
            'limits':'Fake transport and local SQLite only; no USB, board discovery, reset, oscillator, or scheduler execution.'}
    args.output.write_text(json.dumps(result,indent=2)+'\n'); print(f'{len(cases)} host protocol cases passed.')


if __name__ == '__main__': main()
