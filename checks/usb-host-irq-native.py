#!/usr/bin/env python3
"""Exercise the linked USB owner IRQ handler and GIC mask/rearm offline."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('hid_native',ROOT/'checks/usb-host-hid-native.py');image_module=importlib.util.module_from_spec(spec);spec.loader.exec_module(image_module)
def run(path,out):
    out.mkdir(parents=True,exist_ok=False);image=image_module.Image(path);cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    cpu.mem_map(0x40000000,0x04000000);cpu.mem_map(0x50000000,0x01000000);cpu.mem_map(0x03400000,0x10000)
    for address,data in image.parts:cpu.mem_write(address,data)
    board=image.address('board');layout=image.layouts['board_application'];native=image.layouts['a733_usb2_native'];core=image.layouts['a733_xhci'];host=board+layout['host'];controller=host+native['core']
    def put(address,n,size=4):cpu.mem_write(address,n.to_bytes(size,'little'))
    def get(address,size=4):return int.from_bytes(cpu.mem_read(address,size),'little')
    put(board+layout['handles']+4*8,0x50001000,8);put(host+native['core_started'],1);put(0x034004b8,0xb0000000)
    critical=[0];started=[1];alive=[1];begins=[];notifies=[];waits=[];done=[];calls=[]
    names=('xTaskGetSchedulerState','board_live','vTaskEnterCritical','vTaskExitCritical','vTaskEnterCriticalFromISR','vTaskExitCriticalFromISR','vTaskGenericNotifyGiveFromISR','ulTaskGenericNotifyTake','ucPortGetCoreID','a733_xhci_interrupt_start','a733_xhci_interrupt_begin','a733_xhci_interrupt_finish')
    hooks={image.address(n):n for n in names}
    def ret(value):cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
    def code(machine,address,size,unused):
        if address==0x50fff000:done.append(1);machine.emu_stop();return
        name=hooks.get(address)
        if name is None:return
        args=[machine.reg_read(UC_ARM64_REG_X0+i) for i in range(5)];calls.append(name)
        if name in ('vTaskEnterCritical','vTaskEnterCriticalFromISR'):
            assert not critical[0];critical[0]=1;ret(0x77 if name.endswith('FromISR') else 0)
        elif name in ('vTaskExitCritical','vTaskExitCriticalFromISR'):
            assert critical[0];critical[0]=0;ret(0)
        elif name=='xTaskGetSchedulerState':ret(started[0])
        elif name=='board_live':ret(alive[0])
        elif name=='vTaskGenericNotifyGiveFromISR':
            assert critical[0] and args[:2]==[0x50001000,0];put(args[2],1);notifies.append(1);ret(0)
        elif name=='ulTaskGenericNotifyTake':assert args[:3]==[0,1,1] and not critical[0];waits.append(1);ret(1)
        elif name=='a733_xhci_interrupt_start':assert args[0]==controller and not critical[0];put(controller+core['irq_mode'],1);ret(1)
        elif name=='a733_xhci_interrupt_begin':
            assert args[0]==controller and not critical[0] and not get(0x03400000)&(1<<31) and not (get(0x03400114)|get(0x03400314))&(1<<27)
            begins.append(1);put(controller+core['irq_service'],1);ret(1)
        elif name=='a733_xhci_interrupt_finish':assert args[0]==controller and not critical[0];put(controller+core['irq_service'],0);ret(1)
        else:ret(0)
    def write(machine,access,address,size,value,unused):
        if address==0x03400194:
            assert critical[0] and value==1<<27;put(0x03400114,get(0x03400114)&~value)
    cpu.hook_add(UC_HOOK_CODE,code);cpu.hook_add(UC_HOOK_MEM_WRITE,write)
    def call(name):
        done.clear();cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
        cpu.emu_start(image.address(name),0,count=100000);assert done,(name,hex(cpu.reg_read(UC_ARM64_REG_PC)))
        return cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
    assert call('board_host_irq_bind')==1;started[0]=2
    assert call('board_host_irq_after')==1 and get(0x03400114)&(1<<27)
    call('board_host_irq');assert len(notifies)==1 and not get(0x03400114)&(1<<27)
    put(0x03400000,1<<31);assert call('board_host_irq_before')==0 and not begins
    put(0x03400000,0);put(0x03400314,1<<27);assert call('board_host_irq_before')==0 and not begins
    put(0x03400314,0);assert call('board_host_irq_before')==1 and len(begins)==1
    assert call('board_host_irq_after')==1 and get(0x03400114)&(1<<27)
    call('board_host_irq_wait');assert waits
    call('board_host_irq_stop');assert not get(0x03400114)&(1<<27)
    call('board_host_irq');assert call('board_host_irq_before')==0xffffffff
    result=dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),calls=calls,scope='Actual linked application IRQ binding/handler/task claim/rearm/stop and gicv3_spi instructions. Kernel/FreeRTOS notification boundary, controller service and GIC effects modeled; actual xHCI service covered by composed checks. CPU dispatch/boot covered separately by root.')
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();run(a.image,a.output)
