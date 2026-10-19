#!/usr/bin/env python3
"""Execute finite GPU client/stop adapters with explicitly modeled service and scheduler."""
import argparse, hashlib, importlib.util, json
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('gpu_close',ROOT/'checks/gpu-close-ordering.py');close=importlib.util.module_from_spec(spec);spec.loader.exec_module(close)
class Machine(close.Machine):
    def __init__(self,image,variant):
        self.image=image;self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.cpu.mem_map(0x41000000,0x3000000);self.cpu.reg_write(UC_ARM64_REG_PSTATE,5)
        for at,data in image.parts:
            if data:self.cpu.mem_write(at,data)
        self.app=image.address('app');self.fields=image.layouts['gpu_application'];self.shared=self.app+self.fields['shared']
        self.write(self.shared+image.layouts['a733_native_usb']['pmic_ready'],1)
        self.write(self.app+self.fields['session']+image.layouts['a733_board_session']['deadline_us'],61000000,8)
        self.now=1000000;self.stop=None;self.outcome='stopped';self.sequence=0;self.closed=False;self.statuses=0;self.operations=[]
        self.hooks={image.address(n):n for n in ['a733_smp_counter','pvr_gpu_service_send','pvr_gpu_service_collect','vTaskDelay']}
        self.hooks[image.address('pvr_gpu_service_collect')]='collect';self.cpu.hook_add(UC_HOOK_CODE,self.hook)
        self.variant=variant;self.suspended=[];self.exit=None;self.critical=False;self.report=[];self.pending_reads=0
        self.cpu.mem_map(0x03400000,0x10000)
        self.cpu.hook_add(UC_HOOK_MEM_READ,self.mmio_read,begin=0x03400000,end=0x0340ffff)
        for n in ['platform_exit','platform_puts','platform_hex','vTaskSuspend','xTaskGetCurrentTaskHandle','xQueueGetMutexHolder','aarch64_device_translate','vPortEnterCritical']:
            self.hooks[image.address(n)]=n
        self.write(self.app+self.fields['usb_observed'],0)
        pmic=self.shared+image.layouts['a733_native_usb']['pmic'];self.write(pmic+image.layouts['a733_pmic']['active'],8)
    def mmio_read(self,cpu,access,address,size,value,unused):
        if address==0x03400208:self.pending_reads+=1
    def field_at(self,base,typ,key,value,size=4):self.write(base+self.image.layouts[typ][key],value,size)
    def stop_state(self):
        s=self.app+self.fields['service'];f=self.image.layouts['pvr_gpu_service'];self.field_at(s,'pvr_gpu_service','phase',5);self.field_at(s,'pvr_gpu_service','owner',8);self.field_at(s,'pvr_gpu_service','irq_masked',1);self.field_at(s,'pvr_gpu_service','worker',0x43008000,8)
        self.field_at(s+f['stop'],'pvr_gpu_stop','phase',10);self.field_at(s+f['stop'],'pvr_gpu_stop','isolation_observed',1)
        self.field_at(self.app+self.fields['backend'],'a733_gpu_backend','phase',5)
        self.field_at(self.app+self.fields['power'],'a733_gpu_platform','phase',6)
        self.field_at(self.app+self.fields['firmware']+self.image.layouts['a733_gpu_reserved']['memory'],'a733_gpu_memory','phase',1)
        if self.variant=='retained-token':self.field_at(self.app+self.fields['power'],'a733_gpu_platform','token',1,8)
        if self.variant=='pending-irq':self.write(0x03400208,0x80000000)
        if self.variant=='mapping-fault':self.write(0x03400108,0)
        if self.variant=='pmic-borrow':
            pmic=self.shared+self.image.layouts['a733_native_usb']['pmic'];slots=pmic+self.image.layouts['a733_pmic']['slots'];borrow=slots+self.image.layouts['a733_pmic_slot']['borrow'];self.field_at(borrow,'a733_rail_borrow','id',1,8)
    def hook(self,cpu,address,size,unused):
        name=self.hooks.get(address);args=[cpu.reg_read(UC_ARM64_REG_X0+n) for n in range(4)]
        if name=='collect':
            super().hook(cpu,address,size,unused)
            if self.closed and self.statuses>=2:
                if self.variant=='close-fault':
                    self.write(args[2]+self.image.layouts['pvr_gpu_service_reply']['result'],4)
                else:self.stop_state()
            return
        if name=='vTaskDelay':
            if self.field('fatal'):
                self.stop='retained';cpu.emu_stop();return
            if self.closed and self.statuses>=2:
                if self.variant=='pmic-timeout' or (self.variant=='pending-irq' and self.pending_reads):self.now=61000000
                else:self.write(self.image.address('pmic_parked'),1)
            self.ret();return
        if name=='vTaskSuspend':
            if not args[0]:self.stop='retained';cpu.emu_stop();return
            assert args[0]==0x43008000;self.suspended.append(args[0]);self.ret();return
        if name=='xTaskGetCurrentTaskHandle':self.ret(0x43009000);return
        if name=='xQueueGetMutexHolder':self.ret(int(self.variant=='held-lock'));return
        if name=='aarch64_device_translate':
            if self.variant=='mapping-fault':self.ret(0);return
            self.write(args[2],args[0],8);self.write(args[2]+8,0);self.write(args[2]+12,2);self.ret(1);return
        if name=='vPortEnterCritical':self.critical=True;self.ret();return
        if name=='platform_puts':
            data=bytes(cpu.mem_read(args[0],256)).split(b'\0',1)[0].decode();self.report.append(data);self.ret();return
        if name=='platform_hex':self.report.append(hex(args[0]));self.ret();return
        if name=='platform_exit':
            assert self.critical and self.suspended==[0x43008000] and self.field('result')==1
            self.exit=args[0];self.stop='exited';cpu.emu_stop();return
        super().hook(cpu,address,size,unused)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--cases',nargs='+');a=p.parse_args();imagepath=a.evidence/'build/adapter.elf';image=close.transport.layout.Image(imagepath);cases=[]
    if a.cases and (a.evidence/'client-result.json').exists():
        prior=json.loads((a.evidence/'client-result.json').read_text());assert prior['elf_sha256']==hashlib.sha256(imagepath.read_bytes()).hexdigest();cases=[x for x in prior['cases'] if x['name'] not in a.cases]
    for variant in ['stopped','close-fault','retained-token','pmic-timeout','pmic-borrow','held-lock','pending-irq','mapping-fault']:
        if a.cases and variant not in a.cases:continue
        m=Machine(image,variant);m.client()
        if variant=='stopped':assert m.exit==0 and m.stop=='exited' and m.operations==[1,3,6,3,6,2,0]
        else:assert m.exit is None and m.stop=='retained' and m.field('result')!=1,(variant,m.stop,m.exit)
        if variant=='pending-irq':assert m.pending_reads and '0x6' in m.report,(m.pending_reads,m.report)
        assert not m.field('usb_observed')
        cases.append({'name':variant,'passed':True,'exit':m.exit,'suspended_workers':m.suspended,'operations':m.operations,'log':m.report,'pending_reads':m.pending_reads})
    result={'passed':True,'elf_sha256':hashlib.sha256(imagepath.read_bytes()).hexdigest(),'cases':cases,'physical_access':False,'scope':'Actual linked GPU client and finite completion guard/report. Service replies, PMIC checkpoint scheduling, locks, GIC state and mappings modeled. No firmware execution, arithmetic or ROM-return evidence.'}
    (a.evidence/'client-result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS finite GPU client:',len(cases),'selected modeled cases')
if __name__=='__main__':main()
