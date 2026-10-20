#!/usr/bin/env python3
"""Check motion input extents and copied capture against actual native reply bytes.

USB transport and ACK durability retain predecessor evidence. These cases use
an exchange model carrying the native frame/receipt; no live device is opened.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','reply','receipt','output'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    caller=module('motion_caller_check',ROOT/'apps/deinterlace/observe.py');prep=caller.preparation;host=caller.host
    try:caller.freeze_bundle(args.bundle)
    except ValueError as error:assert 'Modeled deinterlace evidence' in str(error)
    else:raise AssertionError('Modeled motion admission authorized physical execution')
    with patch.object(prep,'physical_authority',lambda record:None):prepared=caller.freeze_bundle(args.bundle)
    session=prepared['_frozen']['session.bin'];source=prepared['_frozen']['input.nv12'];upload=prepared['_upload']
    assert len(source)==921600 and len(upload)==921632 and struct.unpack_from('<I',session,876)[0]==2
    frame=args.reply.read_bytes();receipt=args.receipt.read_bytes()
    assert host.decode(frame,session)['status']==1 and len(frame)==460992
    host.receipt_check(receipt,session,frame[:192]);assert host.expected_output(source,session) is None
    refused=[]
    for case in ('one-frame','extra-frame','second-frame-mutated','mode-unknown','bottom-first','result-input-sized','result-mode'):
        raw=bytearray(session);pixels=source;wire=frame
        if case=='one-frame':pixels=source[:460800]
        if case=='extra-frame':pixels=source+source[:460800]
        if case=='second-frame-mutated':pixels=source[:460800]+bytes([source[460800]^1])+source[460801:]
        if case=='mode-unknown':struct.pack_into('<I',raw,876,3)
        if case=='bottom-first':struct.pack_into('<I',raw,880,0)
        if case=='result-input-sized':
            wire=bytearray(frame);struct.pack_into('<I',wire,12,921600);wire=bytes(wire)
        if case=='result-mode':
            wire=bytearray(frame);struct.pack_into('<I',wire,148,1);wire=bytes(wire)
        try:
            if case.startswith('result-'):host.decode(wire,session)
            else:prep.upload_bytes(pixels,bytes(raw))
        except ValueError:refused.append(case)
        else:raise AssertionError('Invalid motion input/result accepted: '+case)
    record={};writes=[];observations=iter((frame,receipt))
    def save():caller.supervisor.save_json(args.output/'session.json',record)
    def upload_once(data):assert not writes and data==upload;writes.append('upload')
    def acknowledge(data):
        saved=json.loads((args.output/'session.json').read_text());copied=saved['deinterlace_output']
        assert copied['ack_write_intent'] and copied['matches_cpu_reference'] is None and not copied['pixel_reference_checked']
        assert (args.output/copied['path']).read_bytes()==frame[192:]
        assert data==host.ack(frame,session);writes.append('ack')
    result=host.capture(lambda request,timeout:next(observations),session,args.output,record,save,100,
        upload=upload,send_upload=upload_once,send_ack=acknowledge,now=lambda:1,pause=lambda _:None)
    assert result['ack_observed'] and not result['pixel_reference_checked'] and result['matches_cpu_reference'] is None
    assert writes==['upload','ack'] and record['deinterlace_result']=='observed-success'
    summary=dict(passed=True,physical_access=False,physical_qualified=False,pixel_reference_checked=False,
        image_sha256=prepared['image_sha256'],session_sha256=hashlib.sha256(session).hexdigest(),
        input_sha256=hashlib.sha256(source).hexdigest(),native_reply_sha256=hashlib.sha256(frame).hexdigest(),
        native_receipt_sha256=hashlib.sha256(receipt).hexdigest(),refused=refused,
        source_sha256={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in
            [Path(__file__),Path(prep.__file__),Path(host.__file__),Path(caller.__file__),ROOT/'apps/deinterlace/producer.py']})
    (args.output/'result.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))


if __name__=='__main__':main()
