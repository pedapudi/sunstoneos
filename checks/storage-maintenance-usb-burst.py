#!/usr/bin/env python3
"""Compare actual maintenance USB tasks with one bounded FIFO/packet model."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn.arm64_const import UC_ARM64_REG_X0

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('maintenance_dma',ROOT/'checks/storage-maintenance-dma.py')
dma=importlib.util.module_from_spec(spec);spec.loader.exec_module(dma)

class Transport(dma.Native):
    # Timing costs and FIFO/packet geometry follow checks/usb0-service-burst.c.
    # Actual maintenance instructions run, but these costs are a model, not CPU
    # or USB measurements. Both images receive the same transport and clock.
    def __init__(self,image,case,payload=0,request=False,delay=20000,reads=True):
        super().__init__(image)
        self.case=case;self.ns=0;self.host_delay=delay;self.host_reads=reads
        self.fifo=bytearray();self.armed=b'';self.event=b'';self.collect=0
        self.wire=bytearray();self.queued=bytearray();self.callback_bytes=0;self.callback=None
        self.host_out=bytearray();self.rx_event=b'';self.rx_ready=0
        self.passes=0;self.packets=0;self.tick_passes=0;self.tick_packets=0;self.ticks=[]
        self.segment_start=0;self.provider_yield=False;self.provider_waited=False;self.clock_calls=0
        self.burst_passes=0;self.burst_cpu_ns=0;self.bursts=[];self.deadline=1000000000
        self.usb();self.put(self.usb_address('polling'),1,4)
        self.transport=self.n+self.nl['transport'];self.tl=self.layouts['a733_storage_native_transport']
        self.channel=self.n+self.nl['channel'];self.cl=self.layouts['a733_storage_channel']
        self.frame=b''
        if payload:
            data=bytes((i*19+7)&255 for i in range(payload))
            body=struct.pack('<IIQII',1,0,0,payload,0)+hashlib.sha256(data).digest()+data
            self.frame=struct.pack('<4I',0x53534f53,1,0xd3555044,len(body))+body
            self.u.mem_write(self.channel+self.cl['response'],body)
            self.put(self.channel+self.cl['response_size'],len(body),4);self.put(self.channel+self.cl['state'],3,4)
            self.put(self.usb_address('sequence'),1,4);self.put(self.transport+self.tl['accepted_command'],2)
        if request:
            body=bytearray(632);struct.pack_into('<IIQII',body,32,1,4,0,512,0)
            self.host_out=bytearray(struct.pack('<4I',0x53534f53,1,0x53555044,len(body))+body)
            self.rx_ready=self.host_delay
        self.request=request
        if case=='expiry':
            self.deadline=1000030
            self.put(self.n+self.nl['boot']+self.layouts['a733_storage_native_boot']['grant']+
                     self.layouts['a733_storage_grant']['deadline_us'],self.deadline)
        for name in ('dcd_int_handler','tud_task_event_ready','usbd_edpt_busy'):
            if name in self.symbols:self.fakes[self.symbols[name]]=name
    def arm(self):
        if self.armed or self.event or not self.fifo:return
        count=min(512,len(self.fifo));self.armed=bytes(self.fifo[:count]);del self.fifo[:count]
        self.collect=self.ns+self.host_delay;self.ns+=1000
    def step(self,u,pc,size,unused):
        name=getattr(self,'fakes',{}).get(pc)
        if not hasattr(self,'ns'):return super().step(u,pc,size,unused)
        a=[u.reg_read(UC_ARM64_REG_X0+i) for i in range(4)]
        if name=='a733_counter_clock_now':
            self.clock_calls+=1;self.ns+=100
            if self.case=='counter-failure' and self.clock_calls==3:self.ret(0);return
            now=1000000+self.ns//1000
            if self.case=='stalled-counter':now=1000000
            if self.case=='backward-counter':
                samples=[1000000,1000010,1000020,1000025,1000022]
                now=samples[min(self.clock_calls-1,len(samples)-1)]
            self.put(a[1],now);self.ret(1);return
        if name=='a733_storage_native_usb_health':
            self.ns+=500
            if self.case=='provider-loss' and self.passes>=2:self.ret(0xffffffffffffffff);return
            if self.case=='provider-wait' and self.passes==2 and not self.provider_waited:
                self.provider_waited=True;self.provider_yield=True;self.ret(0);return
            self.ret(1);return
        if name=='a733_storage_native_live':
            self.ret(self.live and (self.case!='expiry' or 1000000+self.ns//1000<self.deadline));return
        if name=='tud_task_event_ready':self.ret(bool(self.event or self.rx_event));return
        if name=='usbd_edpt_busy':assert a[:2]==[0,0x81];self.ret(bool(self.armed));return
        if name=='dcd_int_handler':
            self.ns+=1500
            if self.armed and self.host_reads and self.ns>=self.collect:
                assert not self.event;self.event=self.armed;self.armed=b'';self.wire.extend(self.event)
                self.packets+=1;self.tick_packets+=1
            if self.host_out and not self.rx_event and self.ns>=self.rx_ready and len(self.incoming)<=1536:
                self.rx_event=bytes(self.host_out[:512]);del self.host_out[:512]
                self.packets+=1;self.tick_packets+=1
            self.ret();return
        if name=='tud_task_ext':
            self.ns+=500;self.passes+=1;self.tick_passes+=1;self.burst_passes+=1
            if self.rx_event:
                self.incoming.extend(self.rx_event);self.rx_event=b'';self.rx_ready=self.ns+self.host_delay
            if self.event:
                self.callback=len(self.event);self.event=b'';self.ret();self.stop='callback';u.emu_stop();return
            self.ret();return
        if name=='tud_vendor_n_read':self.ns+=200
        if name=='tud_vendor_n_write':
            self.ns+=200;count=min(a[2],2048-len(self.fifo));data=bytes(u.mem_read(a[1],count))
            self.fifo.extend(data);self.queued.extend(data)
            if len(self.fifo)>=512:self.arm()
            self.ret(count);return
        if name=='tud_vendor_n_write_flush':self.arm();self.ret();return
        if name=='a733_maintenance_poll':
            complete=self.get(self.transport+self.tl['completed_sequence'])
            assert complete==(1 if self.frame and self.callback_bytes==len(self.frame) else 0)
            self.ret(1);return
        if name=='vTaskDelay':
            assert a[0]==1
            elapsed=self.ns-self.segment_start;self.burst_cpu_ns+=elapsed
            kind='provider' if self.provider_yield else 'retained' if self.failed else 'fairness'
            self.provider_yield=False
            self.ticks.append(dict(kind=kind,passes=self.tick_passes,packets=self.tick_packets,cpu_ns=elapsed))
            if kind!='provider':
                self.bursts.append(dict(passes=self.burst_passes,cpu_ns=self.burst_cpu_ns))
                self.burst_passes=self.burst_cpu_ns=0
            self.tick_passes=self.tick_packets=0
            self.ns=(self.ns//1000000+1)*1000000;self.segment_start=self.ns
            self.ret();self.stop=kind;u.emu_stop();return
        return super().step(u,pc,size,unused)
    def drive(self):
        self.call('a733_storage_native_usb_task',(self.n,))
        while True:
            if self.stop=='callback':
                state=self.u.context_save();count=self.callback;self.callback=None;self.callback_bytes+=count
                self.call('tud_vendor_tx_cb',(0,count));self.u.context_restore(state);self.arm()
                assert self.get(self.transport+self.tl['completed_sequence'])==(1 if self.callback_bytes==len(self.frame) else 0)
            elif self.failed:break
            elif self.frame and self.callback_bytes==len(self.frame):break
            elif self.request and self.get(self.transport+self.tl['accepted_sequence'])==1:break
            elif self.case in ('idle','abandoned','stalled-counter') and len(self.ticks)>=4:break
            assert len(self.ticks)<100,(self.case,'tick bound')
            self.stop=None;self.u.emu_start(self.reg('pc'),0x43000004,count=500000)
            assert self.stop,(self.case,'instruction bound')
        if not self.failed and self.host_reads and self.frame:assert bytes(self.wire)==self.frame==bytes(self.queued)
        if self.request:
            assert self.get(self.transport+self.tl['accepted_sequence'])==1 and not self.get(self.transport+self.tl['completed_sequence'])
            assert self.get(self.channel+self.cl['request_size'],4)==632
        return dict(case=self.case,ticks=len(self.ticks),provider_yields=sum(t['kind']=='provider' for t in self.ticks),
                    passes=self.passes,packets=self.packets,bytes=self.callback_bytes,queued=len(self.queued),failed=self.failed,
                    max_burst_passes=max(b['passes'] for b in self.bursts),last_burst_passes=self.bursts[-1]['passes'],
                    max_burst_cpu_ns=max(b['cpu_ns'] for b in self.bursts),bursts=self.bursts,tick_detail=self.ticks)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--before',type=Path,required=True)
    p.add_argument('--after',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    a.output.mkdir(parents=True);results={}
    common=[('response-32k',dict(payload=32768)),('short-response',dict(payload=184)),('request',dict(request=True)),
            ('slow-host',dict(payload=4096,delay=400000)),('idle',{}),('abandoned',dict(payload=4096,reads=False))]
    for version,image in (('before',a.before),('after',a.after)):
        rows=[]
        cases=common+([] if version=='before' else [('stalled-counter',dict(payload=4096,reads=False)),
            ('counter-failure',dict(payload=4096,reads=False)),('backward-counter',dict(payload=4096,reads=False)),
            ('expiry',dict(payload=4096)),('provider-loss',dict(payload=4096)),('provider-wait',dict(payload=4096))])
        for name,kwargs in cases:
            row=Transport(image,name,**kwargs).drive();rows.append(row)
            (a.output/f'{version}-{name}.json').write_text(json.dumps(row,indent=2)+'\n')
            print(version,name,'ticks',row['ticks'],'passes',row['passes'],'max burst',row['max_burst_passes'],'failed',row['failed'],flush=True)
            if version=='after':
                assert row['max_burst_passes']<=64,row
                if name!='stalled-counter':assert row['max_burst_cpu_ns']<=215000,row
            assert row['failed']==(name in ('counter-failure','backward-counter','expiry','provider-loss')),row
        results[version]=dict(image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),cases=rows)
    old={r['case']:r for r in results['before']['cases']};new={r['case']:r for r in results['after']['cases']}
    assert new['response-32k']['ticks']<old['response-32k']['ticks']//2
    assert new['response-32k']['bytes']==old['response-32k']['bytes']==32840
    assert new['short-response']['ticks']==1 and old['short-response']['ticks']==2
    assert new['request']['ticks']<old['request']['ticks']
    assert new['idle']['max_burst_passes']==new['abandoned']['last_burst_passes']==1
    assert new['stalled-counter']['max_burst_passes']==64 and new['stalled-counter']['last_burst_passes']==1
    assert new['provider-wait']['provider_yields']==1
    results.update(result='pass',physical=False,limit='Identical modeled FIFO/endpoint/host costs; packet and scheduler counts are not physical throughput. Provider waits are separate from fairness yields.')
    (a.output/'result.json').write_text(json.dumps(results,indent=2)+'\n')

if __name__=='__main__':main()
