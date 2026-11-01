#!/usr/bin/env python3
"""Check preparation and single-attempt transfer ordering with a host-only fake."""
import argparse
import fcntl
import hashlib
import os
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--focus-refusal', action='store_true')
    parser.add_argument('--focus-freeze', action='store_true')
    parser.add_argument('--focus-archive', action='store_true',help='Check archival files cannot admit or block native input validation.')
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / 'tools/check-a733-smp.py'
    spec = importlib.util.spec_from_file_location('smp_host',source)
    host = importlib.util.module_from_spec(spec); spec.loader.exec_module(host)
    if args.focus_archive:
        schema=json.loads((args.bundle/'prepared.json').read_text())['schema']
        filename={2:'check-a733-smp.py','a733-smp-observe-bundle-v1':'check-a733-smp-observe.py',
                  'a733-smp-supervised-bundle-v1':'check-a733-smp-supervised.py',
                  'a733-device-launch-bundle-v2':'check-a733-device-launch.py'}[schema]
        source=source.parent/filename
        spec=importlib.util.spec_from_file_location('native_archive_host',source)
        host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
    prepared = host.validate_bundle(args.bundle)
    cases = []
    if args.focus_archive:
        with tempfile.TemporaryDirectory() as scratch:
            root=Path(scratch);bundle=root/'bundle';shutil.copytree(args.bundle,bundle)
            native={'image.elf','launch.bin','request.bin','contract.bin','result-zero.bin','report-zero.bin'} | {item['name'] for item in prepared['segments']}
            metadata=json.loads((bundle/'prepared.json').read_text())
            manifest=bundle/'image-input-manifest.json';manifest.write_bytes(b'optional historical review metadata')
            metadata['files'][manifest.name]=hashlib.sha256(manifest.read_bytes()).hexdigest()
            metadata['base_host_sha256']='0'*64
            (bundle/'prepared.json').write_text(json.dumps(metadata))
            archived=set(metadata['files'])-native
            assert archived,'Bundle has no archival source/log fixture'
            for name in archived:
                (bundle/name).write_bytes(b'changed archival source or log; never uploaded')
            host.validate_bundle(bundle)
            cases.append({'case':'changed archival files with unchanged historical hashes','result':'accepted'})
            saved=root/'archival-inputs';saved.mkdir()
            for name in archived:(bundle/name).rename(saved/name)
            host.validate_bundle(bundle)
            cases.append({'case':'archival files absent from executable bundle','result':'accepted'})
            segment=bundle/prepared['segments'][0]['name'];content=bytearray(segment.read_bytes());content[0]^=1;segment.write_bytes(content)
            try:host.validate_bundle(bundle)
            except ValueError:cases.append({'case':'changed runtime segment bytes','result':'refused'})
            else:raise AssertionError('Runtime bytes accepted after archival removal')
        result={'physical_access':False,'schema':schema,'cases':cases,'archival_files':sorted(archived),
                'native_files':sorted(native),'host_tool_sha256':host.digest(source),
                'limits':'Host file validation only; no device operation or native workload.'}
        args.output.write_text(json.dumps(result,indent=2)+'\n')
        print(f'{len(cases)} native/archive separation cases passed.');return

    if args.focus_freeze:
        def inspect_seal(path, descriptors, expected):
            assert len(descriptors)==1 and str(path)==f'/proc/self/fd/{descriptors[0]}'
            descriptor=descriptors[0]
            assert os.fstat(descriptor).st_size==len(expected) and Path(path).read_bytes()==expected
            assert fcntl.fcntl(descriptor,fcntl.F_GET_SEALS)==15
            for change in (lambda:os.pwrite(descriptor,b'X',0),lambda:os.ftruncate(descriptor,len(expected)+1),lambda:os.ftruncate(descriptor,0)):
                try: change()
                except PermissionError: pass
                else: raise AssertionError('Mutable transfer source')

        for mode in ('before-growth','before-replacement','after-growth','after-replacement'):
            with tempfile.TemporaryDirectory() as scratch:
                root=Path(scratch); bundle=root/'bundle'; shutil.copytree(args.bundle,bundle); evidence=root/'evidence'; evidence.mkdir()
                admitted=host.validate_bundle(bundle); target=bundle/admitted['segments'][0]['name']
                expected={item['address']:(bundle/item['name']).read_bytes() for item in admitted['segments']}
                expected.update({address:(bundle/name).read_bytes() for name,address in [('result-zero.bin',0x42000000),('contract.bin',host.CONTRACT),('request.bin',0x52c00),('report-zero.bin',0x52d00),('launch.bin',0x52000)]})
                def mutate():
                    if mode.endswith('growth'):
                        with target.open('ab') as stream: stream.write(b'G'*65536)
                    else:
                        target.unlink(); target.write_bytes(b'R'*131072)
                if mode.startswith('before'): mutate()
                writes=[]
                def transaction(*arguments,pass_fds=()):
                    if arguments[0]=='read': Path(arguments[3]).write_bytes(bytes(arguments[2])); return ''
                    if arguments[0]=='readl': return '0'
                    assert arguments[0]=='write' and arguments[3]=='read'
                    address=int(arguments[1],16)
                    if not writes and mode.startswith('after'): mutate()
                    inspect_seal(arguments[2],pass_fds,expected[address])
                    assert arguments[5]==len(expected[address])
                    writes.append(address); Path(arguments[6]).write_bytes(Path(arguments[2]).read_bytes()); return ''
                try: host.preload(transaction,bundle,admitted,evidence)
                except ValueError:
                    assert mode.startswith('before') and not writes
                    outcome='refused-before-transfer'
                else:
                    assert mode.startswith('after') and len(writes)==len(expected)
                    outcome='admitted-bytes-and-extent-preserved'
                cases.append({'case':mode,'result':outcome,'writes':len(writes)})

        with tempfile.TemporaryDirectory() as scratch:
            evidence=Path(scratch); record={}; saved={}; originals={}; prepared['_backup_hashes']={}
            for index,(name,address,length) in enumerate(host.REGIONS):
                data=bytes([index+10])*length; saved[address]=data; originals[address]=evidence/f'before-{name}.bin'; originals[address].write_bytes(data)
                prepared['_backup_hashes'][name]=hashlib.sha256(data).hexdigest()
            words=[1,0,0,prepared['epoch'],prepared['generation'],prepared['lease'],0x4153373352454631]+[0]*9
            writes=[]
            def transaction(*arguments,pass_fds=()):
                if arguments[0]=='read': Path(arguments[3]).write_bytes(struct.pack('<16Q',*words)); return ''
                if arguments[0]=='readl': return '0'
                address=int(arguments[1],16)
                originals[address].write_bytes(b'changed backup'*8192)
                inspect_seal(arguments[2],pass_fds,saved[address])
                Path(arguments[6]).write_bytes(Path(arguments[2]).read_bytes()); writes.append(address); return ''
            assert host.observe_refusal(transaction,evidence,prepared,record,lambda:None)=='refused' and len(writes)==len(host.REGIONS)
            cases.append({'case':'backup replacement after sealing','result':'restored-admitted-bytes-and-extent'})

        data=b'immutable child input'*100
        with host.sealed_source(data) as (path,descriptor):
            child=subprocess.run([sys.executable,'-c',"import hashlib,pathlib,sys; data=pathlib.Path(sys.argv[1]).read_bytes(); print(len(data),hashlib.sha256(data).hexdigest())",str(path)],pass_fds=(descriptor,),capture_output=True,text=True,check=True)
            assert child.stdout.strip()==f'{len(data)} {hashlib.sha256(data).hexdigest()}'
        cases.append({'case':'sealed source inherited by child process','result':'exact-bytes-and-size'})
        result={'physical_access':False,'host_tool_sha256':host.digest(source),'check_sha256':host.digest(Path(__file__)),
                'prepared_sha256':host.digest(args.bundle/'prepared.json'),'cases':cases,
                'limits':'Linux memory-file and fake transport checks only; no board discovery or USB operation.'}
        args.output.write_text(json.dumps(result,indent=2)+'\n'); print(f'{len(cases)} immutable transfer cases passed.'); return

    def refused(name, operation):
        try: operation()
        except (ValueError, OSError, KeyError): cases.append({'case':name,'result':'refused'})
        else: raise AssertionError('Accepted '+name)

    def protocol(name, *, fail=None, corrupt=False, active=False, high=False, ready_lost=False, return_failure=False, mutated=False, clean_refusal=False):
        with tempfile.TemporaryDirectory() as scratch:
            evidence = Path(scratch); calls = []; memory = {}; record = {'launch_attempted':False}; executed = []; returned = []
            def save(): (evidence/'record.json').write_text(json.dumps(record))
            def ready():
                if ready_lost: raise ValueError('Live precondition changed')
            def transaction(*arguments, pass_fds=()):
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
                assert memory[0x52c00] == (args.bundle/'request.bin').read_bytes()
                assert memory[0x52000] == (args.bundle/'launch.bin').read_bytes()
                for item in prepared['segments']: assert memory[item['address']] == (args.bundle/item['name']).read_bytes()
                executed.append(True); return ''
            def wait_return():
                assert len(executed) == 1
                if return_failure: raise ValueError('No distinct ROM instance')
                returned.append(True)
                return 'refused' if clean_refusal else None
            error = None
            try: host.launch_once(transaction,wait_return,args.bundle,prepared,evidence,record,save,ready)
            except (OSError,ValueError) as caught: error = str(caught)
            expected_error = any((fail,corrupt,active,high,ready_lost,return_failure,mutated))
            assert bool(error) == bool(expected_error), (name,error)
            if fail: assert len(calls) == fail
            if not expected_error:
                assert len(executed)==1 and returned and not record['complete'] and record['scheduler_result']=='unobserved'
            if corrupt or active or high or ready_lost or mutated: assert not executed and not record['launch_attempted']
            if clean_refusal: assert record['clean_refusal'] and record['complete'] is False
            if return_failure: assert executed and record['launch_attempted'] and not record.get('launch_protocol_complete')
            cases.append({'case':name,'result':'refused' if error else 'protocol-complete-scheduler-unobserved','requests':len(calls)})
            return len(calls)

    class Held(Exception): pass
    for phase in (() if args.focus_refusal else ('save', 'quarantine')):
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

    if not args.focus_refusal:
        count = protocol('complete preloading and one execution without process records')
        for index in range(1,count+1): protocol(f'transport failure at request {index}',fail=index)
        protocol('corrupt upload',corrupt=True); protocol('watchdog active',active=True)
        protocol('physical input high',high=True); protocol('live precondition changed before execution',ready_lost=True)
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
    protocol('clean refusal is never scheduler success',clean_refusal=True)
    with tempfile.TemporaryDirectory() as scratch:
        copy = Path(scratch)/'bundle'; shutil.copytree(args.bundle,copy)
        original_bundle, original_prepared = args.bundle, prepared
        args.bundle = copy; prepared = host.validate_bundle(copy)
        segment=copy/prepared['segments'][0]['name']; contents=bytearray(segment.read_bytes()); contents[0]^=1; segment.write_bytes(contents)
        protocol('segment changed after admission',mutated=True)
        args.bundle, prepared = original_bundle, original_prepared
        metadata=json.loads((copy/'prepared.json').read_text()); metadata['schema']=1; (copy/'prepared.json').write_text(json.dumps(metadata))
        refused('obsolete SRAM layout',lambda:host.validate_bundle(copy))

    def refusal_probe(name, *, bad_word=None, active=False, fail=None):
        with tempfile.TemporaryDirectory() as scratch:
            evidence=Path(scratch); record={}; calls=[]; saved={}
            for index,(label,address,length) in enumerate(host.REGIONS):
                saved[address]=bytes([index+1])*length
                (evidence/f'before-{label}.bin').write_bytes(saved[address])
                prepared.setdefault('_backup_hashes', {})[label] = hashlib.sha256(saved[address]).hexdigest()
            words=[1,0,0,prepared['epoch'],prepared['generation'],prepared['lease'],0x4153373352454631]+[0]*9
            if bad_word: words[bad_word[0]]=bad_word[1]
            def transaction(*arguments, pass_fds=()):
                calls.append(arguments)
                if len(calls)==fail: raise OSError('injected transfer failure')
                if arguments[0]=='read':
                    assert arguments[:3]==('read','0x52d00',128)
                    Path(arguments[3]).write_bytes(struct.pack('<16Q',*words)); return ''
                if arguments[0]=='readl':
                    assert arguments==('readl','0x02050014'); return '41' if active else '0'
                assert arguments[0]=='write' and arguments[3]=='read'
                address=int(arguments[1],16); assert address in saved
                assert Path(arguments[2]).read_bytes()==saved[address]
                Path(arguments[6]).write_bytes(saved[address]); return ''
            try:
                outcome=host.observe_refusal(transaction,evidence,prepared,record,lambda:None)
            except (ValueError,OSError):
                assert bad_word or active or fail
                if bad_word: assert len(calls)==1
                if active: assert len(calls)==2
                if fail: assert len(calls)==fail
                result='refused'
            else:
                assert not (bad_word or active or fail) and outcome=='refused' and record['sram_restored']
                result='clean-refusal-with-SRAM-restored'
            cases.append({'case':name,'result':result,'requests':len(calls)})
            return len(calls)

    requests=refusal_probe('fresh refusal preserves and restores reserved SRAM')
    for index in range(1,requests+1): refusal_probe(f'refusal observation failure at request {index}',fail=index)
    for name,word,value in [('stale refusal epoch',3,prepared['epoch']^1),('stale refusal generation',4,prepared['generation']^1),
                            ('stale resource lease',5,prepared['lease']^1),('missing refusal marker',6,0),('post-mutation failure',0,7)]:
        refusal_probe(name,bad_word=(word,value))
    refusal_probe('active watchdog prevents clean-refusal classification',active=True)
    result={'physical_access':False,'host_tool_sha256':host.digest(source),'check_sha256':host.digest(Path(__file__)),
            'prepared_sha256':host.digest(args.bundle/'prepared.json'),'cases':cases,
            'limits':'Fake transport and local SQLite only; no USB, board discovery, reset, oscillator, or scheduler execution.'}
    args.output.write_text(json.dumps(result,indent=2)+'\n'); print(f'{len(cases)} host protocol cases passed.')


if __name__ == '__main__': main()
