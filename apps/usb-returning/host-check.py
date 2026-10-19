#!/usr/bin/env python3
"""Check bounded host capture and result consistency without USB access."""
import argparse
import errno
from contextlib import contextmanager
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace

ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();e=a.evidence
host=module('usb_return_host',ROOT/'tools/check-a733-usb-returning.py');kernel=module('kernel_return_host',ROOT/'tools/check-a733-kernel.py')
real_find,real_dispose=host.usb.core.find,host.usb.util.dispose_resources
cases=[]
def passed(name):cases.append({'name':name,'passed':True})
try:
 for variant in ['complete','wrong-generation','short-read','read-failure','write-failure','absent','rom-first','expired-write','identity-enodev','target-enoent','late-native','target-valueerror','setup-late']:
    with tempfile.TemporaryDirectory() as tmp:
        root=Path(tmp);node=root/'003'/'001';node.parent.mkdir();node.touch()
        (root/'product').write_text('sunstoneos returning USB');(root/'bcdDevice').write_text('02b3' if variant=='rom-first' else '0100')
        state={'time':1.06 if variant=='late-native' else 0.,'writes':0,'reads':0,'disposed':0}
        def sleep(dt):state['time']+=dt
        host.time=SimpleNamespace(monotonic=lambda:state['time'],sleep=sleep)
        access=SimpleNamespace(session_node=lambda c:None if variant=='absent' else node,identity=lambda n:[1,2,3,4,5],target=lambda n:root,require_session=lambda n:None)
        class Device:
            def write(self,ep,data,timeout):
                assert ep==2 and data==b'USB?' and 1<=timeout<=100;state['writes']+=1
                if variant=='write-failure':raise OSError('modeled write failure')
                if variant=='expired-write':state['time']=2.
                return 4
            def read(self,ep,n,timeout):
                assert ep==0x81 and n==40 and 1<=timeout<=100;state['reads']+=1
                if variant=='read-failure':raise OSError('modeled read failure')
                return b'short' if variant=='short-read' else struct.pack('<5Q',0x3154455253425355,2 if variant=='wrong-generation' else 1,1,5,9)
        def find(**kw):
            if variant=='setup-late':state['time']=1.1
            return Device()
        host.usb.core.find=find
        def dispose(d):state['disposed']+=1
        host.usb.util.dispose_resources=dispose
        if variant=='identity-enodev':
            def missing(n):raise OSError(errno.ENODEV,'modeled disappearance')
            access.identity=missing
        if variant=='target-valueerror':
            def missing(n):raise ValueError('Device disappeared from sysfs.')
            access.target=missing
        if variant=='target-enoent':
            def missing(n):raise FileNotFoundError(errno.ENOENT,'modeled disappearance')
            access.target=missing
        record={'observation_start_monotonic':0.};error=None
        try:host.observe(access,{},[0]*5,5.,record,lambda:None,1)
        except (RuntimeError,OSError) as ex:error=str(ex)
        negative=variant in ('absent','rom-first','identity-enodev','target-enoent','late-native','target-valueerror','setup-late')
        if variant=='complete':assert record['native_usb']['complete'] and error is None
        elif negative:assert not record['native_usb']['complete'] and not state['writes'] and error is None
        else:assert record['transport_failed'] and error and state['writes']==1
        assert state['reads']==(1 if variant in ('complete','wrong-generation','short-read','read-failure') else 0)
        assert state['time']<=2. and state['disposed']==0
        passed(variant)
 nominal='\n'.join(f'{k}=0x{v:016x}' for k,v in {'USB_OBSERVATION_OUTCOME':0,'USB_RECEIVED':1,'USB_SENT':40,'USB_TRANSFERRED':40,'USB_QUIESCED':1}.items())
 for field in ['nominal','outcome','sent','transferred','quiesced','host-incomplete','wrong-generation','return-incomplete']:
    record={'complete':field!='return-incomplete','native_usb':{'complete':field!='host-incomplete','generation':2 if field=='wrong-generation' else 1}}
    log=nominal
    for token,key,wrong in [('outcome','USB_OBSERVATION_OUTCOME',1),('sent','USB_SENT',39),('transferred','USB_TRANSFERRED',39),('quiesced','USB_QUIESCED',0)]:
        if field==token:log=log.replace(f'{key}=0x{(0 if token=="outcome" else 1 if token=="quiesced" else 40):016x}',f'{key}=0x{wrong:016x}')
    assert host.functional_result(record,log,1)==(field=='nominal')
    passed('consistency-'+field)
 # Execute the real existing kernel runner with modeled host/device services.
 # Its callback must run after launch, under the held lock and before any ROM
 # protocol request. The same total deadline must survive native enumeration.
 for variant in ['callback-success','callback-failure']:
    with tempfile.TemporaryDirectory() as tmp:
        root=Path(tmp);image=root/'probe.bin';image.write_bytes(b'native fixture');fel=root/'fel';fel.write_bytes(b'host fixture')
        rom=root/'003'/'001';native=root/'003'/'002';returned=root/'003'/'003';rom.parent.mkdir();[x.touch() for x in (rom,native,returned)]
        port=root/'3-3';port.mkdir();(port/'bcdDevice').write_text('02b3')
        state={'locked':False,'launched':False,'callback':False,'time':0.,'polls':0,'requests_after_launch':0,'retained':False}
        @contextmanager
        def lock():
            state['locked']=True
            try:yield rom
            finally:state['locked']=False
        def identity(n):return [1,2,int(n.name),4,5]
        def session(c):
            state['polls']+=1
            (port/'bcdDevice').write_text('0100' if state['polls']==1 else '02b3')
            return native if state['polls']==1 else returned
        access=SimpleNamespace(board_lock=lock,require_session=lambda n:None,require_rom=lambda n:None,identity=identity,target=lambda n:port,session_node=session,quarantine_instance=lambda *v:None)
        helper=SimpleNamespace(FEL_HASH='fixture',stop_after_failure=lambda *v:state.update(retained=True))
        kernel.load_module=lambda name,path:access if name=='access' else helper
        @contextmanager
        def reenumeration(*v):yield
        sys.modules['usb_guard']=SimpleNamespace(check_health=lambda:None,reenumeration=reenumeration)
        kernel.time=SimpleNamespace(monotonic=lambda:state['time'],sleep=lambda dt:state.update(time=state['time']+dt),strftime=lambda s:'fixture')
        def run(command,**kwargs):
            assert state['locked'];args=command[3:]
            if state['launched']:state['requests_after_launch']+=1;assert state['callback']
            out=''
            if args[0]=='version':out='soc=00001903(A733)'
            elif args[0]=='readl':out='eb00008f e3a00911 e3a0193d e3a0605c ea00004d'
            elif args[0]=='read':
                size=int(args[2]);data=bytearray(size)
                if size==128:struct.pack_into('<I',data,0,0x534b4552)
                else:
                    text=b'COMPLETE fixture';struct.pack_into('<4Q',data,0,0x534b4552,5,0,len(text));data[256:256+len(text)]=text
                Path(args[3]).write_bytes(data)
            elif args[0]=='write' and 'read' in args:
                i=args.index('read');Path(args[i+3]).write_bytes(Path(args[2]).read_bytes())
            elif args[0]=='reset64':state['launched']=True
            return SimpleNamespace(returncode=0,stdout=out,stderr='')
        kernel.subprocess=SimpleNamespace(run=run)
        def observe(*values):
            assert state['locked'] and state['launched'] and state['requests_after_launch']==0
            assert values[3]==5.
            state['callback']=True;state['time']=1.
            if variant=='callback-failure':raise RuntimeError('modeled observation transport failure')
        try:kernel.main(['--fel',str(fel),'--image',str(image),'--evidence',str(root/'runtime'),'--return-timeout','5','--expect','COMPLETE fixture'],observe=observe)
        except SystemExit:assert variant=='callback-failure'
        record=json.loads((root/'runtime/session.json').read_text())
        assert state['callback'] and not state['locked']
        if variant=='callback-success':assert record['complete'] and state['polls']==2 and state['time']==1.01
        else:assert state['retained'] and not record['complete'] and not state['requests_after_launch']
        passed(variant)
finally:
 host.usb.core.find=real_find;host.usb.util.dispose_resources=real_dispose
result={'passed':True,'physical_access':False,'checker_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'host_sha256':hashlib.sha256((ROOT/'tools/check-a733-usb-returning.py').read_bytes()).hexdigest(),'kernel_runner_sha256':hashlib.sha256((ROOT/'tools/check-a733-kernel.py').read_bytes()).hexdigest(),'cases':cases}
(e/'host-check-result.json').write_text(json.dumps(result,indent=2)+'\n');print(f'{len(cases)} focused host cases passed')
