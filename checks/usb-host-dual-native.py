#!/usr/bin/env python3
"""Exercise the linked combined board consumer, serializer and host capture offline."""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
base=module('existing_hid_native',ROOT/'checks/usb-host-hid-native.py')
def run(image_path,reports_path,sector_path,out,order,fault=None):
    out.mkdir(parents=True,exist_ok=False);image=base.Image(image_path);cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    cpu.mem_map(0x40000000,0x04000000);cpu.mem_map(0x50000000,0x01000000)
    for a,data in image.parts:cpu.mem_write(a,data)
    b=image.address('board');fields=image.layouts['board_application'];native=image.layouts['a733_usb2_native'];host=b+fields['host']
    session=bytearray((Path.home()/'.cache/sunstoneos/evidence/board-supervised-20261017/batch1/session-fixture.bin').read_bytes());layout=image.layouts['a733_board_session']
    for name,value,size in [('workload',4,4),('latch_identity',20,8),('latch_owner',21,8),('typec_owner',70,8),('usb2_owner',99,8),('latch_known_mask',0x3fff,4),('source_min_mv',5000,4),('source_max_mv',5100,4),('source_ma',500,4),('zero_threshold_max_mv',800,4),('fusb_device_id',0x80,4),('usb2_revision',0x3139302a,4),('pck_client',2,4)]:session[layout[name]:layout[name]+size]=value.to_bytes(size,'little')
    for name,fmt,values in [('usb_supply_uv','<5I',(3300000,3300000,800000,800000,1800000)),('fabric','<3I',(0,0x83000002,0x84000001)),('fabric_hz','<3I',(24000000,200000000,200000000))]:session[layout[name]:layout[name]+struct.calcsize(fmt)]=struct.pack(fmt,*values)
    session[48:80]=bytes(cpu.mem_read(image.address('a733_smp_build_id'),32));cpu.mem_write(b,bytes(session))
    def put(addr,v,size=4):cpu.mem_write(addr,v.to_bytes(size,'little'))
    report_input=reports_path.read_bytes();hid_address=report_input[image.layouts['a733_usb2_hid_report']['address']];msc_address=3-hid_address
    for field,v in [('kind',4),('address',msc_address),('hid_address',hid_address),('msc_in',0x81),('msc_out',2)]:put(host+native[field],v)
    core=host+native['core'];cl=image.layouts['a733_xhci'];put(core+cl['generation'],71,8)
    put(core+cl['ops']+image.layouts['a733_xhci_ops']['live'],image.address('board_live'),8)
    put(host+native['platform']+image.layouts['a733_usb2_platform']['config']+image.layouts['a733_usb2_platform_config']['lifetime_deadline'],10000000,8)
    reports=reports_path.read_bytes();sector=sector_path.read_bytes();stride=image.sizes['a733_usb2_hid_report'];assert len(reports)==4*stride and len(sector)==512
    revoked=[];completed=[];requests=[];pending=[];calls=[];polled=[0];read_issued=[];read_copied=[];done=[];available=[False]
    names=('board_live','a733_usb2_native_poll','a733_usb2_device_kind','a733_usb2_hid_receive','a733_usb2_hid_result','a733_usb2_read_sector','a733_usb2_read_result','a733_xhci_device_speed','vTaskDelay','board_host_irq_before','board_host_irq_after','board_host_irq_wait','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','board_host_irq_stop','a733_usb2_native_lost','a733_typec_revoke')
    hooks={image.address(n):n for n in names if n in image.symbols}
    def ret(v):cpu.reg_write(UC_ARM64_REG_X0,v);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
    def hook(machine,a,size,unused):
        if a==0x50fff000:done.append(True);machine.emu_stop();return
        name=hooks.get(a)
        if name is None:return
        args=[machine.reg_read(UC_ARM64_REG_X0+i) for i in range(8)];calls.append(name)
        if name=='board_live' and fault and ((order=='hid-first' and len(completed)==4) or (order=='msc-first' and read_copied)):
            revoked.append(True);ret(0)
        elif name=='a733_usb2_native_poll':polled[0]+=1;ret(1)
        elif name=='a733_usb2_device_kind':ret(4)
        elif name=='a733_xhci_device_speed':assert args[1]==msc_address;ret(3)
        elif name=='a733_usb2_read_sector':assert args[1]==0 and not read_issued;read_issued.append(True);ret(1)
        elif name=='a733_usb2_read_result':
            assert read_issued and not read_copied
            if order=='hid-first' and len(completed)<4:ret(0)
            else:machine.mem_write(args[1],sector);read_copied.append(True);ret(1)
        elif name=='a733_usb2_hid_receive':assert not pending and len(requests)<4;pending.append(True);requests.append(True);ret(1)
        elif name=='a733_usb2_hid_result':
            assert pending and len(completed)<4
            if order=='msc-first' and polled[0]<3:ret(0)
            else:at=len(completed)*stride;machine.mem_write(args[1],reports[at:at+stride]);pending.clear();completed.append(True);ret(1)
        elif name in ('vTaskDelay','board_host_irq_wait'):
            published=int.from_bytes(machine.mem_read(b+fields['result'],4),'little')
            assert published==(2 if revoked else 1 if len(completed)==4 and read_copied else 0)
            if published or available[0] is None:done.append(True);machine.emu_stop()
            else:ret(0)
        elif name=='tud_vendor_n_available':ret(1 if available[0] else 0)
        elif name=='tud_vendor_n_read':machine.mem_write(args[1],b'\0');available[0]=None;ret(1)
        else:ret(1)
    cpu.hook_add(UC_HOOK_CODE,hook)
    def call(n,*args):
        done.clear();cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
        for i,v in enumerate(args):cpu.reg_write(UC_ARM64_REG_X0+i,v)
        cpu.emu_start(image.address(n),0,count=2000000);assert done,(n,hex(cpu.reg_read(UC_ARM64_REG_PC)))
        return cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
    assert call('a733_board_session_valid',b,0)==1
    call('board_host_task',0)
    if fault:
        assert int.from_bytes(cpu.mem_read(b+fields['result'],4),'little')==2
        fault=None;available[0]=True;call('board_usb0_task',0)
        count=int.from_bytes(cpu.mem_read(image.address('response_bytes','/apps/board/usb0.c'),4),'little');assert count==72
        frame=bytes(cpu.mem_read(image.address('response','/apps/board/usb0.c'),72));assert struct.unpack_from('<3I',frame,4)==(1,2,0)
        decoder=module('fault_decoder',ROOT/'apps/board/observe.py');assert decoder.decode(frame,session)['kind']=='pending-or-failed'
        (out/'frame.bin').write_bytes(frame);(out/'session.bin').write_bytes(session)
        (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,order=order,fault='authority-lost-after-one-result',image_sha256=hashlib.sha256(image_path.read_bytes()).hexdigest(),completed_hid=len(completed),copied_msc=bool(read_copied),calls=calls,scope='Actual native joint client and failure serializer: a single completed component never publishes a payload when authority is revoked. Provider/kernel boundaries modeled.'),indent=2)+'\n');return
    assert len(completed)==len(requests)==4 and len(read_copied)==1
    backing=bytes(cpu.mem_read(b+fields['dual_result'],1088));available[0]=True;call('board_usb0_task',0)
    assert bytes(cpu.mem_read(b+fields['dual_result'],1088))==backing
    response=image.address('response','/apps/board/usb0.c');count=int.from_bytes(cpu.mem_read(image.address('response_bytes','/apps/board/usb0.c'),4),'little');assert count==1160
    frame=bytes(cpu.mem_read(response,count));assert struct.unpack_from('<I',frame,4)[0]==3 and frame[72:]==backing
    for name,data in [('frame.bin',frame),('session.bin',session),('combined.bin',backing)]: (out/name).write_bytes(data)
    finish(image_path,reports_path,sector_path,out,order,frame,session,calls)
def finish(image_path,reports_path,sector_path,out,order,frame,session,calls):
    reports=reports_path.read_bytes();sector=sector_path.read_bytes()
    decoder=module('dual_decoder',ROOT/'apps/board/observe.py');result=decoder.decode(frame,session);assert result['kind']=='storage-and-hid' and result['sector']==sector and [r['bytes'] for r in result['reports']]==[8,3,0,8]
    refused=[];hid_address=struct.unpack_from('<I',frame,660)[0]
    for name,offset,value in [('header-size',80,0),('generation',88,72),('same-address',96,hid_address),('msc-speed',97,1),('msc-in',99,1),('padding',101,1),('lba',104,1),('sector-offset',108,65),('sector-length',113,1),('hid-offset',116,0),('hid-length',121,1),('reserved',124,1),('hid-count',656,3),('kind-version',4,2)]:
        bad=bytearray(frame);bad[offset]=value
        try:decoder.decode(bad,session)
        except ValueError:refused.append(name)
        else:raise AssertionError(name)
    host_capture=module('dual_capture',ROOT/'tools/check-a733-board-supervised.py');capture=out/'capture';capture.mkdir();record={}
    result=host_capture.capture(lambda packet,timeout:frame,bytes(session),None,capture,record,lambda:None,time.monotonic()+10)
    assert result['kind']=='storage-and-hid' and record['board_result']=='observed-success' and 'copied_sector' in record and 'copied_hid_input' in record
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,order=order,image_sha256=hashlib.sha256(image_path.read_bytes()).hexdigest(),reports_sha256=hashlib.sha256(reports).hexdigest(),sector_sha256=hashlib.sha256(sector).hexdigest(),refused=refused,calls=calls,scope='Actual native dual client, joint immutable publication, session validator and USB0 serializer; actual host decoder/capture. Provider/RTOS/IRQ boundaries modeled, input bytes produced by actual xHCI/TinyUSB composition. No hardware or desktop input.'),indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('image','reports','sector','output'):p.add_argument('--'+n,type=Path,required=True)
    p.add_argument('--order',choices=('hid-first','msc-first'),required=True);p.add_argument('--resume-capture',type=Path);p.add_argument('--fault',action='store_true');a=p.parse_args()
    if a.resume_capture:
        a.output.mkdir(parents=True,exist_ok=False)
        frame=(a.resume_capture/'frame.bin').read_bytes();session=(a.resume_capture/'session.bin').read_bytes()
        for name,data in [('frame.bin',frame),('session.bin',session),('combined.bin',frame[72:])]: (a.output/name).write_bytes(data)
        finish(a.image,a.reports,a.sector,a.output,a.order,frame,session,['native-client-and-serializer-reused:'+str(a.resume_capture)])
    else:run(a.image,a.reports,a.sector,a.output,a.order,a.fault)
