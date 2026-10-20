#!/usr/bin/env python3
"""Check ISP0 host identities, copied-byte authentication and exact FS/HS stream framing.

Requires an ordinary bundle prepared from the actual frozen ISP0 ELF and source.
Sessions use its real board/build/lease identity and the actual producer;
they are software-only wire fixtures, not additional physical admissions.
USB cold initialization, device completions and pixels are modeled. No native code, USB or queue executes.
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
    names=('success-fs','success-hs','native-failure','header-fs','wrong-digest','identity','geometry','proof','receipt-identity',
           'premature-zlp','premature-short','output-failure','receipt-save-failure','upload-failure','deadline')
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--case',action='append',choices=names)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    caller=module('isp0_supervised_host_check',ROOT/'tools/check-a733-isp0-supervised.py')
    producer=module('isp0_host_check_producer',ROOT/'apps/isp0/producer.py')
    # This checker accepts only an explicitly modeled bundle. The unchanged
    # production boundary must refuse it before the isolated authority mock.
    model=json.loads((args.bundle/'admission.json').read_bytes())
    assert model.get('modeled') is True and model.get('physical_authority') is False
    try:caller.freeze_bundle(args.bundle)
    except ValueError as error:assert 'Modeled ISP0 evidence' in str(error)
    else:raise AssertionError('Production accepted modeled authority')
    with patch.object(caller.preparation,'physical_authority',lambda record:None):
        prepared=caller.freeze_bundle(args.bundle)
    host,prep=caller.host,caller.preparation
    base=prepared['_frozen']['session.bin'];source=prepared['_frozen']['input.bggr12']
    admission=json.loads(prepared['_frozen']['admission.json'])
    facts={name:prepared['_frozen'][prepared['entry_evidence'][admission[name]['path']]['name']]
           for name in ('initial','memory','electrical','supervision')}
    owner,authority,client=struct.unpack_from('<3Q',base,616)
    actual_geometry=struct.unpack_from('<8I',base,848)
    assert actual_geometry==(640,480,1280,640,12,0,0,0) and len(source)==614400
    results=[]

    for case in args.case or names:
        folder=args.output/case;folder.mkdir();record={};failed=False
        geometry=actual_geometry;pixels=source
        session=producer.extend(base[:592],pixels,owner=owner,authority=authority,client=client,**facts)
        prep.embedded_reference(prepared['_frozen']['image.elf'],session)
        upload=prep.upload_bytes(pixels,session)
        # This arbitrary copied output models transport only. It is not an ISP oracle.
        output=bytes((i*37+11)&255 for i in range(614400))
        transmitted=bytearray(output)
        epoch,generation=struct.unpack_from('<2Q',session,24)
        header=bytearray(192)
        struct.pack_into('<4I2Q',header,0,0x30495053,1,1,len(transmitted),epoch,generation)
        header[32:64]=session[768:800];header[64:96]=hashlib.sha256(session).digest()
        header[96:128]=hashlib.sha256(transmitted).digest()
        struct.pack_into('<8I8I',header,128,*geometry,1,0xc,0x100,1,1,6,1,614400)
        if case=='identity':header[64]^=1
        if case=='wrong-digest':transmitted[0]^=1
        if case=='geometry':header[128]^=1
        if case=='proof':header[184]=0
        response=bytes(header)+bytes(transmitted)
        if case=='native-failure':
            header[96:128]=bytes(32);struct.pack_into('<II',header,8,2,0)
            struct.pack_into('<8I',header,160,0,0x20,0,0,0x11,0,0,0);response=bytes(header)
        mps=64 if case in ('success-fs','header-fs') else 512
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
                    item=saved['isp0_output'];assert item['numerical_qualified'] is False and item['ack_write_intent']
                    assert (folder/item['path']).read_bytes()==output
                    assert packet==host.ack(response,session);self.ack=True
                    if case=='deadline':self.clock=101
                else:
                    assert packet==b'\0';self.queries+=1
                    wire=bytearray(response if not self.ack else response[:192])
                    if self.ack:
                        struct.pack_into('<II',wire,8,3,0)
                        if case=='receipt-identity':wire[96]^=1
                    elif case=='header-fs' and self.queries==1:
                        wire=wire[:192];struct.pack_into('<II',wire,8,0,0);wire[96:128]=bytes(32)
                        struct.pack_into('<8I',wire,160,0,0,0,0,0,0,0,0)
                    self.reply=wire;self.zlp=len(wire)%mps==0
                return len(packet)
            def read(self,endpoint,size,timeout):
                assert endpoint==0x81 and timeout>0 and size%mps==0;self.reads+=1
                if case=='premature-zlp' and self.reads==1:return b''
                if case=='premature-short' and self.reads==1:
                    data=bytes(self.reply[:512]);del self.reply[:512];return data
                count=min(size,len(self.reply))
                data=bytes(self.reply[:count]);del self.reply[:count]
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
            if case=='receipt-save-failure' and record.get('isp0_output',{}).get('ack_observed') and not failed:
                failed=True;raise OSError('modeled final receipt journal failure')
            caller.supervisor.save_json(folder/'session.json',record)
        original=host.supervisor.durable_frame
        def durable(path,data):
            nonlocal failed
            if case=='output-failure' and path.name=='output.nv16' and not failed:
                failed=True;raise OSError('modeled copied-output durability failure')
            return original(path,data)
        failure_cases=('identity','wrong-digest','geometry','proof','receipt-identity','premature-zlp','premature-short','output-failure','receipt-save-failure','upload-failure','deadline')
        with patch.object(host.supervisor,'durable_frame',durable):
            try:
                result=host.capture_device(device,lambda:None,100,session,folder,record,save,upload=upload,
                    now=lambda:device.clock,pause=lambda _:None)
            except (OSError,ValueError):
                assert case in failure_cases
                assert record['isp0_result']=='unobserved' and 'durable_result' not in record
            else:
                assert case not in failure_cases
                assert record['isp0_result']==('observed-failure' if case=='native-failure' else 'observed-success')
        if case in ('success-fs','success-hs','header-fs'):
            assert device.ack and record['isp0_output']['ack_observed']
        elif case in ('receipt-save-failure','receipt-identity','deadline'):
            assert device.ack and record['isp0_output']['ack_sent']
            if case=='receipt-save-failure':assert record['isp0_output']['ack_observed'] and record['isp0_ack_receipt']['received_hex']
        else:assert not device.ack
        if case=='success-fs':assert device.explicit_zlp==1
        if case=='header-fs':assert device.queries==3 and device.explicit_zlp==1
        if case=='upload-failure':
            intent=record['isp0_writes'][0]
            assert len(record['isp0_writes'])==1 and intent['accepted_bytes']==512 and intent['attempt_offset']==512 and not intent['last_write_completed']
        if case=='deadline':assert device.queries==1
        assert not record['physical_qualified']
        results.append(dict(case=case,packet_size=mps,queries=device.queries,ack_sent=device.ack,
            explicit_terminal_zlps=device.explicit_zlp,source_sha256=hashlib.sha256(pixels).hexdigest(),
            session_sha256=hashlib.sha256(session).hexdigest()))
    for offset in (0,1,2,3,4,5,6,7):
        geometry=list(actual_geometry);geometry[offset]^=1
        try:prep.geometry_check(geometry)
        except ValueError:pass
        else:raise AssertionError('invalid geometry accepted')
    changed=bytearray(source);changed[0]^=1
    for invalid in (source[:-2],source+b'\0\0',b'\0\x10'+source[2:],bytes(changed)):
        try:prep.upload_bytes(invalid,base)
        except ValueError:pass
        else:raise AssertionError('invalid input accepted')
    proofs=(
        (160,0),(164,4),(164,0x2c),(168,0),(168,0x101),(172,0),(172,0x41),
        (176,0),(176,0x11),(176,0x10001),(180,2),(184,0),(188,614399))
    for offset,value in proofs:
        # Use the same sealed session and valid full output as the capture cases.
        frame=bytearray(192+len(output))
        struct.pack_into('<4I2Q',frame,0,0x30495053,1,1,614400,epoch,generation)
        frame[32:64]=session[768:800];frame[64:96]=hashlib.sha256(session).digest()
        frame[96:128]=hashlib.sha256(output).digest();struct.pack_into('<8I',frame,128,*actual_geometry)
        struct.pack_into('<8I',frame,160,1,0xc,0x100,1,1,6,1,614400);frame[192:]=output
        struct.pack_into('<I',frame,offset,value)
        try:host.decode(bytes(frame),session)
        except ValueError:pass
        else:raise AssertionError('invalid completion proof accepted')
    summary=dict(passed=True,physical_access=False,scope=__doc__,native_image_sha256=prepared['image_sha256'],
        launcher_sha256=caller.digest(args.bundle/'launch.bin'),caller_sha256=caller.digest(Path(caller.__file__)),
        host_sha256=caller.digest(Path(host.__file__)),cases=results,geometry_refusals=8,input_refusals=4,proof_refusals=len(proofs))
    (args.output/'result.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))


if __name__=='__main__':main()
