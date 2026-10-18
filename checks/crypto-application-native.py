#!/usr/bin/env python3
"""Focused linked application publication/retention and USB framing; modeled CE returns."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def run(image,session_path,out):
    out.mkdir(parents=True,exist_ok=False)
    reference=module('crypto_native_vectors',ROOT/'apps/crypto/vectors.py').REFERENCE
    session=session_path.read_bytes()
    prep=module('crypto_native_prepare',ROOT/'apps/crypto/prepare.py');assert prep.embedded_reference(image.read_bytes(),session)==reference
    with image.open('rb') as f:
        elf=ELFFile(f);symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        segments=[(int(s['p_vaddr']),bytes(s.data())) for s in elf.iter_segments() if s['p_type']=='PT_LOAD'];layouts={}
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                name=die.attributes.get('DW_AT_name')
                if die.tag=='DW_TAG_structure_type' and name:
                    fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in die.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes and 'DW_AT_data_member_location' in c.attributes}
                    if fields:layouts[name.value.decode()]=fields
    required=('crypto','board','crypto_task','crypto_run_step','eligible','a733_ce_session_retain','xQueueSemaphoreTake','xQueueGenericSend','vTaskDelay','crypto_response','board_usb0_task','board_live','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read')
    assert all(symbols.get(n) for n in required),'Missing linked fixture dependency'
    layout=layouts['crypto_application'];runner=layouts['crypto_run'];results={}
    for variant in ('success','output-cutoff','output-fault','queued-revocation','partial-operation-fault','bad-final-bytes','final-revoked','stop-revokes-success','retain-busy'):
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x50000000,0x1000000)
        for address,data in segments:cpu.mem_write(address,data)
        base=symbols['crypto'];cpu.mem_write(base,session);cpu.mem_write(symbols['board'],session[:592]);cpu.mem_write(symbols['board']+160,struct.pack('<Q',1000000000000))
        cpu.mem_write(base+layout['mutex'],struct.pack('<Q',1))
        if variant=='stop-revokes-success':cpu.mem_write(base+layout['result'],struct.pack('<I',1))
        calls=[];held=[0];delays=[0];finished=[]
        hooks={symbols[n]:n for n in ('board_live','crypto_run_step','eligible','a733_ce_session_retain','xQueueSemaphoreTake','xQueueGenericSend','vTaskDelay')}
        def returned(value):cpu.reg_write(UC_ARM64_REG_X0,value&0xffffffffffffffff);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
        def hook(machine,address,size,unused):
            name=hooks.get(address)
            if name is None:return
            calls.append(name)
            if name=='board_live':returned(variant!='stop-revokes-success')
            elif name=='xQueueSemaphoreTake':assert not held[0];held[0]=1;returned(1)
            elif name=='xQueueGenericSend':assert held[0];held[0]=0;returned(1)
            elif name=='crypto_run_step':
                assert held[0];payload=bytearray(reference)
                if variant=='bad-final-bytes':payload[90]^=1
                machine.mem_write(base+layout['run']+runner['payload'],bytes(payload));machine.mem_write(base+layout['run']+runner['stage'],struct.pack('<I',8));returned(-1 if variant in ('partial-operation-fault','retain-busy') else 1)
            elif name=='eligible':assert held[0];returned(variant!='final-revoked')
            elif name=='a733_ce_session_retain':assert held[0];returned(1 if variant=='retain-busy' else 5)
            elif name=='vTaskDelay':
                assert not held[0];delays[0]+=1
                if delays[0]>=(2 if variant=='retain-busy' else 1):finished.append(True);machine.emu_stop()
                else:returned(0)
        cpu.hook_add(UC_HOOK_CODE,hook)
        cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_X0,0);cpu.emu_start(symbols['crypto_task'],0,count=1000000);assert finished
        status=struct.unpack('<I',cpu.mem_read(base+layout['result'],4))[0]
        assert status==(1 if variant in ('success','output-cutoff','output-fault','queued-revocation') else 2),(variant,status)
        assert calls.count('crypto_run_step')==(0 if variant=='stop-revokes-success' else 1)
        if variant=='retain-busy':assert calls.count('a733_ce_session_retain')==2
        if variant in ('success','output-cutoff','output-fault','queued-revocation'):
            assert bytes(cpu.mem_read(base+layout['payload'],160))==reference
            assert bytes(cpu.mem_read(base+layout['digest'],32))==hashlib.sha256(reference).digest()
            if variant=='output-cutoff':cpu.mem_write(symbols['board']+160,struct.pack('<Q',10000))
            if variant=='output-fault':cpu.mem_write(base+layout['engine']+layouts['a733_ce_session']['fault'],struct.pack('<I',1))
            writes=[]
            def usb_hook(machine,address,size,unused):
                if address==symbols['vTaskDelay']:finished.append(True);machine.emu_stop();return
                if address in {symbols[n] for n in ('a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext')}:returned(1)
                elif address==symbols['tud_vendor_n_available']:returned(1)
                elif address==symbols.get('tud_vendor_n_write'):writes.append(True);returned(0)
                elif address==symbols['tud_vendor_n_read']:machine.mem_write(machine.reg_read(UC_ARM64_REG_X1),b'\0');returned(1)
            usb=cpu.hook_add(UC_HOOK_CODE,usb_hook);finished.clear();cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.emu_start(symbols['board_usb0_task'],0,count=1000000);assert finished
            count=128 if variant in ('output-cutoff','output-fault') else 288
            frame=bytes(cpu.mem_read(symbols['crypto_response'],count));observer=module('crypto_native_observe',ROOT/'apps/crypto/observe.py');decoded=observer.decode(frame,session);assert decoded['status']==(2 if count==128 else 1)
            if variant=='success':(out/'native-frame.bin').write_bytes(frame)
            if variant=='queued-revocation':
                cpu.mem_write(symbols['board']+160,struct.pack('<Q',10000));finished.clear();cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.emu_start(symbols['board_usb0_task'],0,count=1000000);assert finished and not writes
            cpu.hook_del(usb)
        results[variant]={'exit':0,'status':status,'calls':calls}
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=results,scope='Actual linked task, private copy/hash/publication and USB frame. CE return, eligibility and RTOS scheduling/lock boundaries modeled; no hardware qualification.'),indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','session','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();run(a.image,a.session,a.output)
