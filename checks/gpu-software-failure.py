#!/usr/bin/env python3
"""Actual GPU application control flow for retained software setup failures."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('gpu_transport_cases',ROOT/'checks/gpu-usb-ordering.py')
transport=importlib.util.module_from_spec(spec);spec.loader.exec_module(transport)
CONSTRUCTORS=['a733_gpu_reserved_prepare','a733_gpu_compute_reserved_prepare','a733_gpu_backend_bind','pvr_gpu_service_init','pvr_gpu_service_bind']
class Machine(transport.Machine):
    def __init__(self,image,fail=None):
        super().__init__(image);self.fail=fail;self.called=[];self.suspended=[];self.gpu_irqs=0;self.mmio=[];self.created=0
        session=0x43010000;self.cpu.mem_write(session,bytes(592))
        layout=image.layouts['a733_board_session']
        for key,value in [('workload',2),('epoch',123),('generation',124),('pmic_owner',10),('usb0_owner',11)]:self.write(session+layout[key],value,4 if key=='workload' else 8)
        runtime=image.address('a733_cached_runtime');fields=image.layouts['a733_cached_runtime']
        for key,value in [('epoch',123),('startup_deadline',24000000),('diagnostic_deadline',61000000*24)]:self.write(runtime+fields[key],value,8)
        self.sealed=session;self.worker=0x43008000
        names=CONSTRUCTORS+['a733_smp_device_preparation_admitted','a733_device_memory_ready','gpu_application_session','a733_board_session_valid','a733_native_usb_init','xTaskCreateStatic','a733_native_usb_bind','a733_gpu_platform_native_init','a733_gpu_platform_init','a733_pmic_identity','a733_gpu_platform_bind_pmic','vTaskSuspend','vTaskStartScheduler','platform_panic','pvr_gpu_service_interrupt']
        self.hooks.update({image.address(n):n for n in names})
        for n in ['device_memory_release','device_memory_reclaim','a733_gpu_memory_reclaim']:
            for address in image.symbols.get(n,set())-{0}:self.hooks[address]='forbidden-release'
        self.cpu.hook_add(UC_HOOK_MEM_READ,self.mmio_access,begin=0x01800000,end=0x080fffff)
        self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.mmio_access,begin=0x01800000,end=0x080fffff)
    def mmio_access(self,cpu,access,address,size,value,unused):
        self.mmio.append([address,access]);raise AssertionError(('Application touched device MMIO',hex(address)))
    def hook(self,cpu,address,size,unused):
        name=self.hooks.get(address)
        if name in CONSTRUCTORS:
            self.called.append(name)
            if name==self.fail:self.ret(0);return
            if name=='pvr_gpu_service_init':
                service=self.app+self.fields['service'];fields=self.image.layouts['pvr_gpu_service']
                self.write(service+fields['worker'],self.worker,8);self.write(service+fields['initialized'],1)
            self.ret(1);return
        if name in ['a733_smp_device_preparation_admitted','a733_device_memory_ready','a733_board_session_valid','a733_native_usb_init','a733_native_usb_bind','a733_gpu_platform_native_init','a733_gpu_platform_init','a733_pmic_identity','a733_gpu_platform_bind_pmic']:
            self.ret(1);return
        if name=='gpu_application_session':self.ret(self.sealed);return
        if name=='xTaskCreateStatic':self.created+=1;self.ret(0x43002000+0x100*self.created);return
        if name=='vTaskSuspend':self.suspended.append(cpu.reg_read(UC_ARM64_REG_X0));self.ret();return
        if name=='vTaskStartScheduler':self.stop='scheduler';cpu.emu_stop();return
        if name in ('platform_panic','forbidden-release'):raise AssertionError(name)
        if name=='pvr_gpu_service_interrupt':self.gpu_irqs+=1;self.ret(1);return
        super().hook(cpu,address,size,unused)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--image',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    image=transport.layout.Image(args.image);cases=[]
    def preparation(fail):
        m=Machine(image,fail);m.run('a733_smp_diagnostic_start');assert m.stop=='scheduler' and m.created==3
        if fail:
            assert m.called==CONSTRUCTORS[:CONSTRUCTORS.index(fail)+1]
            assert m.field('preparation_failed')==1 and m.field('result')==2 and not m.field('fatal')
            assert m.suspended==([m.worker] if fail=='pvr_gpu_service_bind' else [])
        return m
    for failed in CONSTRUCTORS:
        m=preparation(failed);m.client();assert m.stop=='delay' and not m.opened
        m.run('gpu_application_irq');assert not m.gpu_irqs
        m.queue();frame=b''.join(m.tx);assert len(frame)==40 and struct.unpack_from('<3I',frame)==(0x43555047,1,2)
        m.complete(40);assert not m.field('usb_observed') and not m.field('fatal')
        m.client();assert not m.opened and m.stop=='delay';m.run('gpu_application_irq');assert not m.gpu_irqs and not m.mmio
        cases.append({'name':failed+'-retained-terminal-report','passed':True,'constructor_prefix':m.called,'suspended':m.suspended,'response_hex':frame.hex(),'open':m.opened,'gpu_irq_calls':m.gpu_irqs})
    m=preparation(None);assert not m.field('preparation_failed') and not m.field('result') and not m.suspended
    m.client();assert not m.opened;m.queue();assert struct.unpack_from('<I',b''.join(m.tx),8)[0]==0
    m.complete(40);assert m.field('usb_observed');m.client();assert m.opened
    cases.append({'name':'successful-construction-pending-then-open','passed':True})
    for fault in ['shared-provider','deadline']:
        m=preparation('pvr_gpu_service_bind')
        if fault=='shared-provider':m.health=-1
        else:m.now=61000000
        m.queue();assert m.stop=='delay' and m.field('fatal') and not m.tx
        m.run('gpu_application_irq');assert not m.gpu_irqs
        m.client();assert not m.opened and not m.mmio
        cases.append({'name':fault+'-still-fatal-after-software-failure','passed':True})
    result={'passed':True,'image_sha256':hashlib.sha256(args.image.read_bytes()).hexdigest(),'cases':cases,'scope':'Actual application entry, client, USB task, completion and IRQ dispatcher. Constructor returns, shared providers, scheduler and transport callbacks modeled; no monitor/CPU/startup execution, hardware or GPU arithmetic replay.'}
    (args.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('Eight focused GPU software-failure cases passed')
if __name__=='__main__':main()
