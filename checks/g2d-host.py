#!/usr/bin/env python3
"""Check G2D host identities, copied-byte semantics and exact FS/HS stream framing.

Requires an ordinary bundle prepared from the actual frozen G2D ELF and source.
Variant sessions use its real board/build/lease identity and the actual producer;
they are software-only wire fixtures, not additional physical admissions.
Device completions and pixels are modeled. No native code, USB or queue executes.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from types import SimpleNamespace
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def main():
    names=('fill-fs','copy-hs','header-fs','terminal-hs','short-tail','wrong-pixels','identity',
           'premature-zlp','premature-short','output-failure','receipt-save-failure','upload-failure','deadline',
           'blend-hs','blend-reference-difference','blend-wrong-digest')
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--case',action='append',choices=names)
    parser.add_argument('--copied-output',type=Path)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    caller=module('g2d_supervised_host_check',ROOT/'tools/check-a733-g2d-supervised.py')
    producer=module('g2d_host_check_producer',ROOT/'apps/g2d/producer.py')
    # This checker accepts only an explicitly modeled bundle. The unchanged
    # production boundary must refuse it before the isolated authority mock.
    model=json.loads((args.bundle/'admission.json').read_bytes())
    assert model.get('modeled') is True and model.get('physical_authority') is False
    try:caller.freeze_bundle(args.bundle)
    except ValueError as error:assert 'Modeled G2D evidence' in str(error)
    else:raise AssertionError('Production accepted modeled authority')
    with patch.object(caller.preparation,'physical_authority',lambda record:None):
        prepared=caller.freeze_bundle(args.bundle)
    host,prep=caller.host,caller.preparation
    base=prepared['_frozen']['session.bin'];source=prepared['_frozen']['input.argb']
    admission=json.loads(prepared['_frozen']['admission.json'])
    facts={name:prepared['_frozen'][prepared['entry_evidence'][admission[name]['path']]['name']]
           for name in ('initial','memory','electrical','supervision')}
    owner,authority,client=struct.unpack_from('<3Q',base,616)
    actual_geometry=struct.unpack_from('<12I',base,856)
    assert actual_geometry[0] in (60,64) and actual_geometry[1:3]==(64,256) and len(source)==16384
    results=[]

    def reference(pixels,geometry):
        width,height,stride,format_,operation,dx,dy,w,h,sx,sy,color=geometry
        out=bytearray(height*stride)
        for y in range(h):
            for x in range(w):
                value=struct.pack('<I',color) if operation==1 else pixels[(sy+y)*stride+(sx+x)*4:(sy+y)*stride+(sx+x)*4+4]
                if operation==3:
                    background=struct.pack('<I',color);alpha=value[3]
                    value=bytes((value[c]*alpha+background[c]*(255-alpha)+127)//255 for c in range(3))+b'\xff'
                offset=(dy+y)*stride+(dx+x)*4;out[offset:offset+4]=value
        return bytes(out)

    for case in args.case or names:
        folder=args.output/case;folder.mkdir();record={};failed=False
        geometry=[60,64,256,0,1,7,9,17,21,0,0,0xffc02040] if case=='fill-fs' else list(actual_geometry)
        if case!='fill-fs':geometry[4]=2;geometry[11]=0
        if case in ('blend-hs','blend-reference-difference','blend-wrong-digest'):geometry=[60,64,256,0,3,9,17,31,23,5,7,0xff315a93]
        pixels=source
        if case=='terminal-hs':geometry=[1,506,32,0,2,0,0,1,506,0,0,0];pixels=source[:16192]
        if case=='short-tail':geometry=[1,1,4,0,1,0,0,1,1,0,0,0xff010203];pixels=source[:4]
        session=producer.extend(base[:592],pixels,owner=owner,authority=authority,client=client,
            **facts,width=geometry[0],height=geometry[1],stride=geometry[2],operation=geometry[4],
            dx=geometry[5],dy=geometry[6],w=geometry[7],h=geometry[8],sx=geometry[9],sy=geometry[10],color=geometry[11])
        prep.embedded_reference(prepared['_frozen']['image.elf'],session)
        upload=prep.upload_bytes(pixels,session);output=reference(pixels,geometry)
        assert host.expected_output(pixels,session)==output
        if args.copied_output and case.startswith('blend-'):
            assert list(actual_geometry)==geometry and args.copied_output.read_bytes()==output
            output=args.copied_output.read_bytes()
        transmitted=bytearray(output)
        if case=='wrong-pixels':transmitted[0]^=1
        if case=='blend-reference-difference':transmitted[geometry[6]*geometry[2]+geometry[5]*4]^=1
        epoch,generation=struct.unpack_from('<2Q',session,24)
        header=bytearray(192)
        struct.pack_into('<4I2Q',header,0,0x44324753,1,1,len(transmitted),epoch,generation)
        header[32:64]=session[768:800];header[64:96]=hashlib.sha256(session).digest()
        header[96:128]=hashlib.sha256(transmitted).digest()
        struct.pack_into('<12I4I',header,128,*geometry,1,0x7101,0x400,1)
        response=bytes(header)+bytes(transmitted)
        if case=='blend-wrong-digest':response=response[:192]+bytes([response[192]^1])+response[193:]
        if case=='identity':header[64]^=1;response=bytes(header)+bytes(transmitted)
        mps=64 if case in ('fill-fs','header-fs','short-tail') else 512
        class Device:
            def __init__(self):
                self.received=bytearray();self.reply=bytearray();self.zlp=False;self.ack=False
                self.queries=0;self.reads=0;self.writes=0;self.clock=1;self.explicit_zlp=0
            def get_active_configuration(self):return {(0,0):[SimpleNamespace(bEndpointAddress=0x81,bmAttributes=2,wMaxPacketSize=mps)]}
            def write(self,endpoint,packet,timeout):
                assert endpoint==2 and timeout>0;self.writes+=1
                if case=='upload-failure' and self.writes==2:raise OSError('modeled source upload transport failure')
                if len(self.received)<len(upload):
                    self.received.extend(packet);assert upload.startswith(self.received);return len(packet)
                assert bytes(self.received)==upload and not self.reply and not self.zlp
                if len(packet)==41:
                    saved=json.loads((folder/'session.json').read_text())
                    item=saved['g2d_output'];assert (item['matches_cpu_reference'] or case=='blend-reference-difference') and item['ack_write_intent']
                    assert (folder/item['path']).read_bytes()==bytes(transmitted)
                    assert packet==host.ack(response,session);self.ack=True
                    if case=='deadline':self.clock=101
                else:
                    assert packet==b'\0';self.queries+=1
                    wire=bytearray(response if not self.ack else response[:192])
                    if self.ack:struct.pack_into('<II',wire,8,3,0)
                    elif case=='header-fs' and self.queries==1:
                        wire=wire[:192];struct.pack_into('<II',wire,8,0,0);wire[96:128]=bytes(32)
                        struct.pack_into('<4I',wire,176,0,0,0,0)
                    self.reply=wire;self.zlp=len(wire)%mps==0
                return len(packet)
            def read(self,endpoint,size,timeout):
                assert endpoint==0x81 and timeout>0 and size%mps==0;self.reads+=1
                if case=='premature-zlp' and self.reads==1:return b''
                if case=='premature-short' and self.reads==1:
                    data=bytes(self.reply[:512]);del self.reply[:512];return data
                count=min(size,len(self.reply));data=bytes(self.reply[:count]);del self.reply[:count]
                if count<size and not self.reply:
                    # libusb ends a short successful read after the final short
                    # packet or ZLP. Only an exact-size read leaves a ZLP pending.
                    assert count%mps or self.zlp
                    if not count:self.explicit_zlp+=1
                    self.zlp=False
                return data
        device=Device()
        def save():
            nonlocal failed
            if case=='receipt-save-failure' and record.get('g2d_output',{}).get('ack_observed') and not failed:
                failed=True;raise OSError('modeled final receipt journal failure')
            caller.supervisor.save_json(folder/'session.json',record)
        original=host.supervisor.durable_frame
        def durable(path,data):
            nonlocal failed
            if case=='output-failure' and path.name=='output.argb' and not failed:
                failed=True;raise OSError('modeled copied-output durability failure')
            return original(path,data)
        failure_cases=('blend-wrong-digest','identity','premature-zlp','premature-short','output-failure','receipt-save-failure','upload-failure','deadline')
        with patch.object(host.supervisor,'durable_frame',durable):
            try:
                result=host.capture_device(device,lambda:None,100,session,folder,record,save,upload=upload,
                    now=lambda:device.clock,pause=lambda _:None)
            except (OSError,ValueError):
                assert case in failure_cases
                assert record['g2d_result']=='unobserved' and 'durable_result' not in record
            else:
                assert case not in failure_cases
                assert record['g2d_result']==('observed-failure' if case=='wrong-pixels' else 'observed-success')
        if case in ('fill-fs','copy-hs','header-fs','terminal-hs','short-tail','blend-hs','blend-reference-difference'):
            assert device.ack and record['g2d_output']['ack_observed']
        elif case in ('receipt-save-failure','deadline'):
            assert device.ack and record['g2d_output']['ack_sent']
            if case=='receipt-save-failure':assert record['g2d_output']['ack_observed'] and record['g2d_ack_receipt']['received_hex']
        else:assert not device.ack
        if case in ('fill-fs','terminal-hs'):assert device.explicit_zlp==1
        if case=='header-fs':assert device.queries==3 and device.explicit_zlp==1
        if case=='short-tail':assert device.explicit_zlp==0
        if case=='wrong-pixels':assert (folder/'output.argb').read_bytes()==bytes(transmitted)
        if case=='blend-reference-difference':assert record['g2d_output']['matches_cpu_reference'] is False and record['g2d_output']['reference_qualified'] is False
        if case=='upload-failure':
            intent=record['g2d_writes'][0]
            assert len(record['g2d_writes'])==1 and intent['accepted_bytes']==512 and intent['attempt_offset']==512 and not intent['last_write_completed']
        if case=='deadline':assert device.queries==1
        assert not record['physical_qualified']
        results.append(dict(case=case,packet_size=mps,queries=device.queries,ack_sent=device.ack,
            explicit_terminal_zlps=device.explicit_zlp,source_sha256=hashlib.sha256(pixels).hexdigest(),
            session_sha256=hashlib.sha256(session).hexdigest()))
    invalid_geometry=([1281,1,5124,0,1,0,0,1,1,0,0,0],[1,1,4,0,1,0,0,1,1,1,0,0],
                      [1,1,4,0,2,0,0,1,1,0,0,1],[64,64,256,0,2,60,0,5,1,0,0,0])
    if args.case and all(case.startswith('blend-') for case in args.case):
        invalid_geometry=([60,64,256,0,3,9,17,31,23,5,7,0xfe315a93],
                          [60,64,256,0,3,9,17,31,23,30,7,0xff315a93])
    for geometry in invalid_geometry:
        try:prep.geometry_check(geometry)
        except ValueError:pass
        else:raise AssertionError('invalid geometry accepted')
    summary=dict(passed=True,physical_access=False,scope=__doc__,native_image_sha256=prepared['image_sha256'],
        launcher_sha256=caller.digest(args.bundle/'launch.bin'),caller_sha256=caller.digest(Path(caller.__file__)),
        host_sha256=caller.digest(Path(host.__file__)),cases=results,geometry_refusals=True)
    (args.output/'result.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))


if __name__=='__main__':main()
