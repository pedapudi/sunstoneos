#!/usr/bin/env python3
"""Replay actual GPU lifecycle producer bytes with modeled host failure effects."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import time
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
compute=module('gpu_lifecycle_check_compute',ROOT/'apps/gpu-program/lifecycle.py')
render=module('gpu_lifecycle_check_render',ROOT/'apps/gpu-render/lifecycle.py')


def artifact(record,base):
    path=Path(record['path']);path=path if path.is_absolute() else base/path
    raw=path.read_bytes()
    if hashlib.sha256(raw).hexdigest()!=record['sha256']:raise ValueError('Actual producer artifact changed: '+str(path))
    return raw


class Replay:
    def __init__(self,fixture,base,case):
        self.profile=fixture['profile'];self.texture=self.profile in ('texture','depth');self.case=case
        self.rows=[(artifact(row['request'],base),artifact(row['response'],base),
                    artifact(row['body'],base) if 'body' in row else b'') for row in fixture['exchanges']]
        self.cursor=0;self.header=None;self.body=b'';self.body_offset=0;self.injected=False;self.requests=[]
        self.first_epoch=None

    def mutate(self,request,response):
        raw=bytearray(response);offset=64 if self.texture else 40
        op=struct.unpack_from('<I',request,8)[0];state=struct.unpack_from('<I',raw,8)[0]
        epoch=struct.unpack_from('<Q',raw,offset)[0]
        if self.first_epoch is None and epoch:self.first_epoch=epoch
        if not self.injected:
            if self.case=='session-epoch':
                struct.pack_into('<Q',raw,48 if self.texture else 28,0);self.injected=True
            elif self.case=='queue-epoch' and op!=0:
                struct.pack_into('<Q',raw,offset,epoch+7);self.injected=True
            elif self.case=='open-epoch' and state==1 and self.first_epoch is not None and epoch>self.first_epoch:
                struct.pack_into('<Q',raw,offset,epoch+1);self.injected=True
            elif self.case=='open-status-closed' and self.texture and op==0 and state==1 and self.first_epoch is not None and epoch>self.first_epoch:
                struct.pack_into('<I',raw,44,1);self.injected=True
        return bytes(raw)

    def accept(self,request):
        request=bytes(request)
        if self.cursor>=len(self.rows):raise AssertionError('Host requested beyond actual producer transcript')
        expected,response,body=self.rows[self.cursor]
        if request!=expected:raise AssertionError('Host request differs from actual producer at '+str(self.cursor))
        self.cursor+=1;self.requests.append(request)
        op=struct.unpack_from('<I',request,8)[0]
        if self.case=='interrupt-transport' and op!=0 and not self.injected:
            self.injected=True;raise KeyboardInterrupt('Modeled interruption after request acceptance')
        return self.mutate(request,response),body

    def exchange(self,request,remaining):
        assert remaining>0
        response,body=self.accept(request);assert not body
        return response

    def write(self,request,remaining):
        assert remaining>0 and self.header is None and self.body_offset==len(self.body)
        self.header,self.body=self.accept(request);self.body_offset=0
        return len(request)

    def read(self,n,remaining):
        assert remaining>0
        if self.header is not None:
            result=self.header;self.header=None;assert len(result)<=n;return result
        if self.case=='partial-stream':
            if self.body_offset and not self.injected:
                self.injected=True;raise OSError('Modeled body transport failure')
            n=min(n,max(1,len(self.body)//2))
        result=self.body[self.body_offset:self.body_offset+n];self.body_offset+=len(result)
        if not result:raise AssertionError('Read beyond actual producer body')
        return result


def client_for(fixture,wire,retain):
    deadline=time.monotonic()+50;epoch=fixture['session_epoch'];profile=fixture['profile']
    if profile=='compute':return compute.Compute(wire.exchange,epoch,fixture['words'],deadline=deadline,retain=retain)
    if profile=='triangle':return render.Triangle(wire.exchange,epoch,deadline=deadline,retain=retain)
    return render.Texture(wire.write,wire.read,epoch,fixture['width'],fixture['height'],
                          deadline=deadline,retain=retain,profile=3 if profile=='depth' else 2)


def run_case(fixture,base,case):
    wire=Replay(fixture,base,case);retained=[];clock={'late':False}
    def retain(kind,identity,data):
        retained.append((kind,dict(identity),bytes(data)))
        if (case=='output-save' and kind=='output') or (case=='disposed-save' and kind=='disposed'):
            wire.injected=True;raise OSError('Modeled durable publication failure')
        if case=='interrupt-retain' and kind=='output':wire.injected=True;raise KeyboardInterrupt('Modeled retention interruption')
        if case=='late-output' and kind=='output':wire.injected=True;clock['late']=True
    client=client_for(fixture,wire,retain)
    inputs=[]
    for item in fixture.get('inputs',[]):
        data=artifact(item,base)
        inputs.append((tuple(item['shape']),data,item.get('state',0)) if wire.texture else data)
    original_clock=time.monotonic
    def now():return client.deadline+1 if clock['late'] else original_clock()
    caught=None
    try:
        with patch.object(time,'monotonic',now):
            if fixture['profile']=='compute':client.read()
            else:client.submit(inputs[0])
            client.close();client.dispose()
            if case=='epoch-wrap':
                client.queue_epoch=compute.MAX_EPOCH;wire.injected=True
            client.open()
            if fixture['profile']=='compute':client.read()
            else:client.submit(inputs[1])
            client.close()
    except BaseException as error:caught=error
    if case=='healthy':
        assert caught is None,caught
        assert wire.cursor==len(wire.rows) and client.state==3 and not client.failure
        assert [row[0] for row in retained]==['output','closed','disposed','opened','output','closed']
        assert len(client.outputs)==2 and client.outputs[0]['identity']['queue_epoch']+1==client.outputs[1]['identity']['queue_epoch']
    else:
        assert wire.injected,'Requested injection was not reached in actual fixture: '+case
        assert isinstance(caught,(ValueError,RuntimeError,OSError,TimeoutError,KeyboardInterrupt)) and client.failure is not None,(case,caught)
        before=len(wire.requests)
        try:client.status()
        except RuntimeError:pass
        else:raise AssertionError('Stopped lifecycle resumed')
        assert len(wire.requests)==before
        if case in ('output-save','interrupt-retain','late-output'):assert client.outputs and retained[0][0]=='output'
        if case=='disposed-save':assert client.state==5 and retained[-1][0]=='disposed'
        if case=='partial-stream':assert client.failure['partial']
        if case=='late-output':assert isinstance(caught,TimeoutError)
        if case.startswith('interrupt-'):assert isinstance(caught,KeyboardInterrupt)
    return {'case':case,'requests':len(wire.requests),'retained':[row[0] for row in retained],
            'stopped':client.failure is not None,'partial_bytes':len(client.failure['partial']) if client.failure else 0}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--case',action='append',choices=('healthy','session-epoch','queue-epoch','open-epoch','open-status-closed',
        'output-save','disposed-save','interrupt-transport','interrupt-retain','late-output','partial-stream','epoch-wrap'))
    args=parser.parse_args();fixture=json.loads(args.fixture.read_bytes());base=args.fixture.parent
    if fixture.get('schema')!='pvr-gpu-lifecycle-host-fixture-v1' or fixture.get('physical_authority') is not False or fixture.get('modeled') is not True:
        raise ValueError('Fixture must explicitly identify modeled hardware authority')
    image=artifact(fixture['image'],base)
    if image[:4]!=b'\x7fELF' or fixture['profile'] not in ('compute','triangle','texture','depth') or not 0<fixture['session_epoch']<2**64:
        raise ValueError('Actual native fixture identity')
    args.output.mkdir(parents=True,exist_ok=False)
    results=[]
    for case in args.case or ['healthy']:
        result=run_case(fixture,base,case);results.append(result)
        (args.output/(case+'.json')).write_text(json.dumps(result,indent=2)+'\n')
    summary={'passed':True,'physical_access':False,'physical_authority':False,'profile':fixture['profile'],
             'fixture_sha256':hashlib.sha256(args.fixture.read_bytes()).hexdigest(),
             'image_sha256':hashlib.sha256(image).hexdigest(),'session_epoch':fixture['session_epoch'],'cases':results,
             'scope':'Actual frozen native request/reply bytes; modeled transport, interruption, persistence and clock failures. No GPU arithmetic, pixels, device, queue, physical drain or timing qualification.'}
    (args.output/'result.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))

if __name__=='__main__':main()
