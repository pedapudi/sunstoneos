#!/usr/bin/env python3
"""Run the linked native board consumer and USB0 HID discriminator offline."""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
class Image:
    def __init__(self,path):
        self.symbols={};self.layouts={};self.sizes={};self.variables={}
        with path.open('rb') as f:
            e=ELFFile(f);self.parts=[(int(p['p_vaddr']),p.data()) for p in e.iter_segments() if p['p_type']=='PT_LOAD']
            for s in e.get_section_by_name('.symtab').iter_symbols():self.symbols.setdefault(s.name,set()).add(int(s['st_value']))
            for cu in e.get_dwarf_info().iter_CUs():
                unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
                for die in cu.iter_DIEs():
                    name=die.attributes.get('DW_AT_name')
                    if die.tag=='DW_TAG_variable' and name and 'DW_AT_location' in die.attributes:
                        loc=die.attributes['DW_AT_location'].value
                        if isinstance(loc,list) and len(loc)==9 and loc[0]==3:self.variables[(unit,name.value.decode())]=int.from_bytes(bytes(loc[1:]),'little')
                        elif isinstance(loc,list) and len(loc)==2 and loc[0]==0xa1:self.variables[(unit,name.value.decode())]=e.get_dwarf_info().get_addr(cu,loc[1])
                    if die.tag=='DW_TAG_structure_type' and name:
                        fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in die.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes and 'DW_AT_data_member_location' in c.attributes}
                        if fields:self.layouts[name.value.decode()]=fields
                        if 'DW_AT_byte_size' in die.attributes:self.sizes[name.value.decode()]=int(die.attributes['DW_AT_byte_size'].value)
    def address(self,name,unit=None):
        v=({a for (u,n),a in self.variables.items() if n==name and u.endswith(unit)} if unit else self.symbols.get(name,set())-{0})
        assert len(v)==1,(name,unit,v);return next(iter(v))
def run(image_path,reports_path,out,resume_batch=None):
    out.mkdir(parents=True,exist_ok=False);image=Image(image_path)
    response_address=image.address('response','/apps/board/usb0.c');response_bytes_address=image.address('response_bytes','/apps/board/usb0.c')
    (out/'preflight.json').write_text(json.dumps(dict(image_sha256=hashlib.sha256(image_path.read_bytes()).hexdigest(),response=response_address,response_bytes=response_bytes_address,defining_unit='apps/board/usb0.c'),indent=2)+'\n')
    cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    cpu.mem_map(0x40000000,0x04000000);cpu.mem_map(0x50000000,0x01000000)
    for a,data in image.parts:cpu.mem_write(a,data)
    layout=image.layouts['board_application'];session_layout=image.layouts['a733_board_session'];native=image.layouts['a733_usb2_native'];platform=image.layouts['a733_usb2_platform'];config=image.layouts['a733_usb2_platform_config'];board=image.address('board')
    baseline=Path.home()/'.cache/sunstoneos/evidence/board-supervised-20261017/batch1/session-fixture.bin';session=bytearray(baseline.read_bytes());assert len(session)==592
    def put(name,value,n=8):session[session_layout[name]:session_layout[name]+n]=int(value).to_bytes(n,'little')
    for name,value in (('latch_identity',20),('latch_owner',21),('typec_owner',70),('usb2_owner',99)):put(name,value)
    for name,value in (('workload',0),('latch_known_mask',0x3fff),('source_min_mv',5000),('source_max_mv',5100),('source_ma',500),('zero_threshold_max_mv',800),('fusb_device_id',0x80),('usb2_revision',0x3139302a),('pck_client',2)):put(name,value,4)
    session[session_layout['usb_supply_uv']:session_layout['usb_supply_uv']+20]=struct.pack('<5I',3300000,3300000,800000,800000,1800000)
    session[session_layout['fabric']:session_layout['fabric']+12]=struct.pack('<3I',0,0x83000002,0x84000001)
    session[session_layout['fabric_hz']:session_layout['fabric_hz']+12]=struct.pack('<3I',24000000,200000000,200000000)
    session[48:80]=bytes(cpu.mem_read(image.address('a733_smp_build_id'),32));cpu.mem_write(board,bytes(session))
    cpu.mem_write(board+layout['host']+native['platform']+platform['config']+config['lifetime_deadline'],struct.pack('<Q',10000000))
    cpu.mem_write(board+layout['host']+native['kind'],struct.pack('<I',2))
    report_bytes=reports_path.read_bytes();stride=image.sizes['a733_usb2_hid_report'];assert stride==96 and len(report_bytes)==4*stride
    calls=[];completed=[];requests=[];pending=[];done=[];response_available=[False]
    names=('board_live','a733_usb2_native_poll','a733_usb2_device_kind','a733_usb2_hid_receive','a733_usb2_hid_result','vTaskDelay','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read')
    hooks={image.address(n):n for n in names}
    def ret(value):cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
    def hook(machine,a,size,unused):
        if a==0x50fff000:done.append(True);machine.emu_stop();return
        name=hooks.get(a)
        if name is None:return
        args=[machine.reg_read(UC_ARM64_REG_X0+i) for i in range(8)];calls.append(name)
        if name=='a733_usb2_hid_receive':assert args[1]==10000000 and not pending;pending.append(True);requests.append(True);ret(1)
        elif name=='a733_usb2_hid_result':
            assert pending and len(completed)<4;at=len(completed)*stride;machine.mem_write(args[1],report_bytes[at:at+stride]);pending.clear();completed.append(True);ret(1)
        elif name=='vTaskDelay':
            assert cpu.mem_read(board+layout['result'],4)==struct.pack('<I',1) if len(completed)==4 else True
            if len(completed)==4 or resume_batch is not None:done.append(True);machine.emu_stop()
            else:ret(0)
        elif name=='a733_usb2_device_kind':ret(2)
        elif name=='a733_usb2_native_poll':ret(1)
        elif name=='tud_vendor_n_available':ret(1 if response_available[0] else 0)
        elif name=='tud_vendor_n_read':machine.mem_write(args[1],b'\0');response_available[0]=False;ret(1)
        else:ret(1)
    cpu.hook_add(UC_HOOK_CODE,hook)
    def call(name,*args):
        done.clear();cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
        for i,value in enumerate(args):cpu.reg_write(UC_ARM64_REG_X0+i,value)
        cpu.emu_start(image.address(name),0,count=2000000);assert done,(name,hex(cpu.reg_read(UC_ARM64_REG_PC)))
        return cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
    assert call('a733_board_session_valid',board,0)==1
    if resume_batch is None:
        call('board_host_task',0);assert len(completed)==len(requests)==4 and not pending
    else:
        batch=resume_batch.read_bytes();assert len(batch)==512
        cpu.mem_write(board+layout['sector'],batch);cpu.mem_write(board+layout['result'],struct.pack('<I',1))
    batch=bytes(cpu.mem_read(board+layout['sector'],512));assert struct.unpack_from('<3I',batch)==(0x52444948,1,4)
    (out/'batch.bin').write_bytes(batch);response_available[0]=True;call('board_usb0_task',0)
    count=int.from_bytes(cpu.mem_read(response_bytes_address,4),'little');assert count==584
    frame=bytes(cpu.mem_read(response_address,584));assert struct.unpack_from('<I',frame,4)[0]==2 and frame[72:]==batch
    (out/'frame.bin').write_bytes(frame);(out/'session.bin').write_bytes(session)
    decoder=module('hid_board_decoder',ROOT/'apps/board/observe.py');result=decoder.decode(frame,session)
    assert result['kind']=='hid-input' and result['sector'] is None and [r['bytes'] for r in result['reports']]==[8,3,0,8]
    # Same arbitrary contents remain a storage sector under version1.
    storage=bytearray(frame);struct.pack_into('<I',storage,4,1)
    decoded=decoder.decode(storage,session);assert decoded['kind']=='storage-sector' and decoded['sector']==batch
    refused=[]
    for name,offset,value in [('batch-count',80,3),('speed',96,4),('interface',97,1),('length',112,65),('reserved',100,1),('version',76,2)]:
        bad=bytearray(frame);bad[offset]=value
        try:decoder.decode(bad,session)
        except ValueError:refused.append(name)
        else:raise AssertionError(name)
    host=module('hid_board_host_capture',ROOT/'tools/check-a733-board-supervised.py');capture=out/'capture';capture.mkdir();record={}
    observed=host.capture(lambda packet,timeout:frame,bytes(session),None,capture,record,lambda:None,time.monotonic()+10)
    assert observed['kind']=='hid-input' and record['board_result']=='observed-success' and 'copied_hid_input' in record and 'copied_sector' not in record
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(image_path.read_bytes()).hexdigest(),reports_sha256=hashlib.sha256(report_bytes).hexdigest(),baseline=str(baseline),baseline_sha256=hashlib.sha256(baseline.read_bytes()).hexdigest(),calls=calls,refused=refused,reused_completed_batch=str(resume_batch) if resume_batch else None,scope='Actual native board consumer and unchanged USB0 transport with explicit HID discriminator, actual board session validator, real host decoder/capture. Input report objects produced by actual TinyUSB/xHCI composition; only RTOS/native host calls and USB0 provider observations modeled. No device or desktop events.'),indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','reports','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--resume-batch',type=Path)
    a=p.parse_args();run(a.image,a.reports,a.output,a.resume_batch)
