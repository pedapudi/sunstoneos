#!/usr/bin/env python3
"""Focused shared-transport refusal cases; service/provider effects modeled."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('accelerator_fixture',ROOT/'checks/accelerator-runtime.py')
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
host=base.host

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','package','output'):parser.add_argument('--'+name,type=Path,required=True)
    a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    image=base.large.Image(a.image);package=a.package.read_bytes();results={}
    def machine():
        m=base.Machine(image,package,{})
        m.put(m.g+m.gl['sequence'],1,4);m.put(m.g+m.gl['result'],1,4)
        return m
    def npu_fail(m):m.call('npu_runtime_fail',71)
    def transact(m,packet):
        assert not m.incoming;m.incoming.extend(packet);start=len(m.tx)
        gpu=struct.unpack_from('<I',packet)[0]==host.gpu.MAGIC
        for _ in range(3000):
            m.usb();raw=b''.join(m.tx[start:])
            total=40+4*struct.unpack_from('<I',raw,16)[0] if gpu and len(raw)>=40 else 1040 if not gpu else None
            if total is not None and len(raw)==total:return raw
        raise AssertionError('Focused transport bound')
    def gpu_read(m):
        raw=transact(m,host.gpu.frame(host.gpu.READ,1,m.prepared['epoch']))
        assert host.gpu.decode(raw,m.prepared['epoch'],992)[0]==1
        assert not m.gf('failed') and not m.incoming
    m=machine();begin=host.npu.packet(1,host.npu.BEGIN,m.prepared,1,5000000,total=150528)
    transact(m,begin)
    part=bytes(range(256))*2;packet=host.npu.packet(2,host.npu.UPLOAD,m.prepared,1,data=part)
    m.incoming.extend(packet[:128])
    while m.incoming:m.usb()
    npu_fail(m)
    assert not m.incoming
    raw=transact(m,packet[128:])
    assert len(raw)==1040 and m.state()==host.npu.FAILED
    gpu_read(m);results['npu-failure-mid-upload-drains-before-gpu']=True
    m=machine();npu_fail(m)
    raw=transact(m,host.npu.packet(1,host.npu.UPLOAD,m.prepared,1,data=part))
    assert len(raw)==1040;gpu_read(m);results['failed-npu-header-drains-bounded-upload']=True
    m=machine();ll=m.layout
    for key,value in [('state',host.npu.DONE),('id',1),('deadline',m.ticks+120000000),
                      ('start',m.ticks),('end',m.ticks+24),('job',2),('jobs',1),('irq_count',2)]:
        m.put(m.slot+ll[key],value,4 if key in ('state','jobs','irq_count') else 8)
    m.max_tx=128
    m.incoming.extend(host.npu.packet(1,host.npu.READ,m.prepared,1));start=len(m.tx)
    m.usb();assert 0<sum(map(len,m.tx[start:]))<1040
    npu_fail(m)
    for _ in range(20):
        m.usb()
        if sum(map(len,m.tx[start:]))==1040:break
    assert sum(map(len,m.tx[start:]))==1040 and m.state()==host.npu.FAILED
    gpu_read(m);results['failed-npu-copied-frame-drains-without-ack-or-stall']=True
    for words in (6,22):
        m=machine()
        # Only the transport's admitted output extent is modeled here. Neither
        # this metadata edit nor any arithmetic is presented as program admission.
        m.put(0x43010000+32,words*4,4)
        m.incoming.extend(host.gpu.frame(host.gpu.READ,1,m.prepared['epoch']))
        for _ in range(10):
            m.usb(ack=False)
            if len(b''.join(m.tx))==40+words*4:break
        raw=b''.join(m.tx);assert len(raw)==40+words*4
        m.call('tud_vendor_tx_cb',0,len(raw));assert not m.gf('transport_ready')
        next_request=host.gpu.frame(host.gpu.READ,1,m.prepared['epoch'])
        m.incoming.extend(next_request);m.usb(ack=False)
        assert bytes(m.incoming)==next_request
        m.call('tud_vendor_tx_cb',0,0);assert m.gf('transport_ready')==1
        results[f'full-speed-{words}-word-tail-retains-owner-through-zlp']=True
    m=machine();m.incoming.extend(host.gpu.frame(host.gpu.READ,1,m.prepared['epoch'])[:7]);m.usb()
    m.ticks+=24000000;m.usb()
    assert m.gf('failed') and m.get(image.address('a733_npu_application_failed'),4)
    results['partial-header-expiry-revokes-both-without-reuse']=True
    m=machine();m.put(m.g+m.gl['sequence'],2,4)
    raw=transact(m,host.gpu.frame(host.gpu.READ,1,m.prepared['epoch']))
    assert struct.unpack_from('<I',raw,8)[0]==4 and len(raw)==40 and m.gf('result')==1
    results['stale-gpu-read-copies-no-output']=True
    record=dict(passed=True,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),cases=results,
        physical_access=False,scope='Actual shared USB parsing/TX callbacks, NPU runtime state and failure latches. Provider health, service operations, selected output extents, scheduler/USB effects modeled; no CPU/startup/IRQ execution.')
    (a.output/'result.json').write_text(json.dumps(record,indent=2)+'\n');print('PASS focused shared accelerator transport ownership')
if __name__=='__main__':main()
