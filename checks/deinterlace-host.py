#!/usr/bin/env python3
"""Check copied DI output, durable ACK ordering and exact FS/HS USB framing.

The baseline response must come from the actual native fixture for this bundle.
Fault cases mutate those bytes or the transport. All runs remain software models.
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
    names=('copy-fs','copy-hs','pending-fs','identity','busy','guard','reserved',
        'premature-zlp','premature-short','output-failure','receipt-save-failure','upload-failure','deadline')
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','reply','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--case',action='append',choices=names)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    caller=module('deinterlace_host_check',ROOT/'apps/deinterlace/observe.py')
    model=json.loads((args.bundle/'admission.json').read_bytes())
    assert model.get('modeled') is True and model.get('physical_authority') is False
    try:caller.freeze_bundle(args.bundle)
    except ValueError as error:assert 'Modeled deinterlace evidence' in str(error)
    else:raise AssertionError('Production accepted modeled authority')
    with patch.object(caller.preparation,'physical_authority',lambda record:None):
        prepared=caller.freeze_bundle(args.bundle)
    host=caller.host;session=prepared['_frozen']['session.bin'];upload=prepared['_upload']
    native=args.reply.read_bytes();assert host.decode(native,session)['status']==1
    expected=host.expected_output(upload[32:],session)
    assert native[192:]==expected
    results=[]
    for case in args.case or names:
        folder=args.output/case;folder.mkdir();record={};failed=False
        response=bytearray(native)
        if case=='identity':response[64]^=1
        if case=='busy':struct.pack_into('<I',response,164,0x101)
        if case=='guard':struct.pack_into('<I',response,172,0)
        if case=='reserved':response[176]=1
        mps=64 if case in ('copy-fs','pending-fs') else 512
        class Device:
            def __init__(self):
                self.received=bytearray();self.reply=bytearray();self.zlp=False;self.ack=False
                self.queries=0;self.reads=0;self.writes=0;self.clock=1;self.explicit_zlp=0
            def get_active_configuration(self):return {(0,0):[SimpleNamespace(bEndpointAddress=0x81,bmAttributes=2,wMaxPacketSize=mps)]}
            def write(self,endpoint,packet,timeout):
                assert endpoint==2 and timeout>0;self.writes+=1
                if case=='upload-failure' and self.writes==2:raise OSError('modeled upload transport failure')
                if len(self.received)<len(upload):
                    self.received.extend(packet);assert upload.startswith(self.received);return len(packet)
                assert bytes(self.received)==upload and not self.reply and not self.zlp
                if len(packet)==41:
                    saved=json.loads((folder/'session.json').read_text());item=saved['deinterlace_output']
                    assert item['matches_cpu_reference'] and item['ack_write_intent']
                    assert (folder/item['path']).read_bytes()==expected
                    assert packet==host.ack(native,session);self.ack=True
                    if case=='deadline':self.clock=101
                else:
                    assert packet==b'\0';self.queries+=1
                    wire=bytearray(response if not self.ack else response[:192])
                    if self.ack:struct.pack_into('<II',wire,8,3,0)
                    elif case=='pending-fs' and self.queries==1:
                        wire=wire[:192];struct.pack_into('<II',wire,8,0,0)
                        wire[96:128]=bytes(32);wire[160:176]=bytes(16)
                    self.reply=wire;self.zlp=len(wire)%mps==0
                return len(packet)
            def read(self,endpoint,size,timeout):
                assert endpoint==0x81 and timeout>0 and size%mps==0;self.reads+=1
                if case=='premature-zlp' and self.reads==1:return b''
                if case=='premature-short' and self.reads==1:
                    data=bytes(self.reply[:512]);del self.reply[:512];return data
                count=min(size,len(self.reply));data=bytes(self.reply[:count]);del self.reply[:count]
                if count<size and not self.reply:
                    assert count%mps or self.zlp
                    if not count:self.explicit_zlp+=1
                    self.zlp=False
                return data
        device=Device()
        def save():
            nonlocal failed
            if case=='receipt-save-failure' and record.get('deinterlace_output',{}).get('ack_observed') and not failed:
                failed=True;raise OSError('modeled receipt journal failure')
            caller.supervisor.save_json(folder/'session.json',record)
        original=host.supervisor.durable_frame
        def durable(path,data):
            nonlocal failed
            if case=='output-failure' and path.name=='output.nv12' and not failed:
                failed=True;raise OSError('modeled copied-output durability failure')
            return original(path,data)
        success=case in ('copy-fs','copy-hs','pending-fs')
        with patch.object(host.supervisor,'durable_frame',durable):
            try:host.capture_device(device,lambda:None,100,session,folder,record,save,
                upload=upload,now=lambda:device.clock,pause=lambda _:None)
            except (OSError,ValueError):
                assert not success
                assert record['deinterlace_result']=='unobserved' and 'durable_result' not in record
            else:assert success and record['deinterlace_result']=='observed-success'
        if success:assert device.ack and record['deinterlace_output']['ack_observed']
        elif case in ('receipt-save-failure','deadline'):assert device.ack
        else:assert not device.ack
        if case=='copy-fs':assert device.explicit_zlp==1
        if case=='pending-fs':assert device.explicit_zlp==1 and device.queries==3
        if case=='copy-hs':assert device.explicit_zlp==0
        assert not record['physical_qualified']
        results.append(dict(case=case,packet_size=mps,queries=device.queries,
            ack_sent=device.ack,explicit_terminal_zlps=device.explicit_zlp))
    summary=dict(passed=True,physical_access=False,native_reply_sha256=hashlib.sha256(native).hexdigest(),
        image_sha256=prepared['image_sha256'],cases=results)
    (args.output/'result.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))


if __name__=='__main__':main()
