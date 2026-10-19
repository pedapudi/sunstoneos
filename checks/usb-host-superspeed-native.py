#!/usr/bin/env python3
"""Run linked SS board consumer/serializer and actual host capture without hardware."""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
base=module('existing_hid_native',ROOT/'checks/usb-host-hid-native.py')
def run(image_path,sector_path,out,fault):
    out.mkdir(parents=True,exist_ok=False);image=base.Image(image_path);cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    cpu.mem_map(0x40000000,0x04000000);cpu.mem_map(0x50000000,0x01000000)
    for address,data in image.parts:cpu.mem_write(address,data)
    b=image.address('board');fields=image.layouts['board_application'];native=image.layouts['a733_usb2_native'];host=b+fields['host']
    session=bytearray((Path.home()/'.cache/sunstoneos/evidence/board-supervised-20261017/batch1/session-fixture.bin').read_bytes());layout=image.layouts['a733_board_session']
    for name,value,size in [('workload',6,4),('latch_identity',20,8),('latch_owner',21,8),('typec_owner',70,8),('usb2_owner',99,8),('latch_known_mask',0x3fff,4),('source_min_mv',5000,4),('source_max_mv',5100,4),('source_ma',900,4),('zero_threshold_max_mv',800,4),('fusb_device_id',0x80,4),('usb2_revision',0x3139302a,4),('pck_client',2,4)]:session[layout[name]:layout[name]+size]=value.to_bytes(size,'little')
    for name,fmt,values in [('usb_supply_uv','<5I',(3300000,3300000,800000,800000,1800000)),('fabric','<3I',(0,0x83000002,0x84000001)),('fabric_hz','<3I',(24000000,200000000,200000000))]:session[layout[name]:layout[name]+struct.calcsize(fmt)]=struct.pack(fmt,*values)
    session[48:80]=bytes(cpu.mem_read(image.address('a733_smp_build_id'),32));cpu.mem_write(b,bytes(session))
    def put(address,value,size=4):cpu.mem_write(address,value.to_bytes(size,'little'))
    for field,value in [('kind',1),('address',1),('msc_in',0x81),('msc_out',2)]:put(host+native[field],value)
    core=host+native['core'];cl=image.layouts['a733_xhci'];put(core+cl['generation'],71,8);put(core+cl['speed'],3 if fault=='hs-fallback' else 4)
    put(core+cl['ops']+image.layouts['a733_xhci_ops']['live'],image.address('board_live'),8)
    put(host+native['platform']+image.layouts['a733_usb2_platform']['config']+image.layouts['a733_usb2_platform_config']['lifetime_deadline'],10000000,8)
    sector=sector_path.read_bytes();assert len(sector)==512
    calls=[];issued=[];copied=[];done=[];available=[False];revoked=[]
    names=('board_live','a733_usb2_native_poll','a733_usb2_device_kind','a733_usb2_read_sector','a733_usb2_read_result','vTaskDelay','board_host_irq_before','board_host_irq_after','board_host_irq_wait','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','board_host_irq_stop','a733_usb2_native_lost','a733_typec_revoke')
    hooks={image.address(name):name for name in names if name in image.symbols}
    def ret(value):cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
    def hook(machine,address,size,unused):
        if address==0x50fff000:done.append(True);machine.emu_stop();return
        name=hooks.get(address)
        if name is None:return
        args=[machine.reg_read(UC_ARM64_REG_X0+i) for i in range(8)];calls.append(name)
        if name=='board_live' and fault=='lost-after-copy' and copied and not available[0]:revoked.append(True);ret(0)
        elif name=='a733_usb2_native_poll':ret(1)
        elif name=='a733_usb2_device_kind':ret(1)
        elif name=='a733_usb2_read_sector':assert args[1]==0 and not issued;issued.append(True);ret(1)
        elif name=='a733_usb2_read_result':assert issued and not copied;machine.mem_write(args[1],sector);copied.append(True);ret(1)
        elif name in ('vTaskDelay','board_host_irq_wait'):
            result=int.from_bytes(machine.mem_read(b+fields['result'],4),'little')
            assert result==(2 if fault else 1)
            done.append(True);machine.emu_stop()
        elif name=='tud_vendor_n_available':ret(1 if available[0] else 0)
        elif name=='tud_vendor_n_read':machine.mem_write(args[1],b'\0');available[0]=False;ret(1)
        else:ret(1)
    cpu.hook_add(UC_HOOK_CODE,hook)
    def call(name,*args):
        done.clear();cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
        for i,value in enumerate(args):cpu.reg_write(UC_ARM64_REG_X0+i,value)
        cpu.emu_start(image.address(name),0,count=2000000);assert done,(name,hex(cpu.reg_read(UC_ARM64_REG_PC)))
        return cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
    assert call('a733_board_session_valid',b,0)==1
    call('board_host_task',0);assert len(issued)==len(copied)==1
    backing=bytes(cpu.mem_read(b+fields['ss_result'],544));available[0]=True;call('board_usb0_task',0)
    assert backing==bytes(cpu.mem_read(b+fields['ss_result'],544))
    count=int.from_bytes(cpu.mem_read(image.address('response_bytes','/apps/board/usb0.c'),4),'little')
    frame=bytes(cpu.mem_read(image.address('response','/apps/board/usb0.c'),count));assert count==(72 if fault else 616)
    for name,data in [('frame.bin',frame),('session.bin',session)]: (out/name).write_bytes(data)
    decoder=module('ss_decoder',ROOT/'apps/board/observe.py');result=decoder.decode(frame,session)
    refused=[]
    if fault:assert result['usb_state']==2 and result['sector'] is None
    else:
        assert result['kind']=='superspeed-sector' and result['sector']==sector and result['attachment_generation']==71 and result['device_speed']=='super-gen1'
        for name,offset,value in [('version',4,1),('magic',72,0),('extent',80,0),('lba',84,1),('address',96,0),('speed',97,3),('interface',98,1),('in',99,1),('out',100,0),('reserved',101,1)]:
            bad=bytearray(frame);bad[offset]=value
            try:decoder.decode(bad,session)
            except ValueError:refused.append(name)
            else:raise AssertionError(name)
        capture=module('ss_capture',ROOT/'tools/check-a733-board-supervised.py');destination=out/'capture';destination.mkdir();record={}
        observed=capture.capture(lambda packet,timeout:frame,bytes(session),None,destination,record,lambda:None,time.monotonic()+10)
        assert observed['kind']=='superspeed-sector' and record['board_result']=='observed-success' and (destination/'sector.bin').read_bytes()==sector
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,fault=fault,image_sha256=hashlib.sha256(image_path.read_bytes()).hexdigest(),sector_sha256=hashlib.sha256(sector).hexdigest(),refused=refused,calls=calls,scope='Actual linked client, session validator, immutable SS result and USB0 serializer; actual host decoder/capture. Provider/RTOS/IRQ boundaries modeled. Sector supplied by actual xHCI/TinyUSB composition. No electrical or DMA-confinement qualification.'),indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','sector','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--fault',choices=('hs-fallback','lost-after-copy'));a=p.parse_args();run(a.image,a.sector,a.output,a.fault)
