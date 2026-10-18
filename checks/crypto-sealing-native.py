#!/usr/bin/env python3
"""Execute changed Crypto session-consumption and USB revocation paths only."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR

def run(image,session_path,out):
    out.mkdir(parents=True,exist_ok=False)
    with image.open('rb') as f:
        elf=ELFFile(f);symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        segments=[(int(s['p_vaddr']),bytes(s.data())) for s in elf.iter_segments() if s['p_type']=='PT_LOAD'];layouts={}
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                name=die.attributes.get('DW_AT_name')
                if die.tag=='DW_TAG_structure_type' and name:
                    fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in die.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes and 'DW_AT_data_member_location' in c.attributes}
                    if fields:layouts[name.value.decode()]=fields
    required=('crypto_response_bytes','a733_smp_build_id','crypto_session_prepare','crypto_admitted_session','board_application_pins','board_application_start','board_usb0_task','board_live','board_fail','crypto_task','crypto','board','crypto_construct','crypto_output_ready','xQueueSemaphoreTake','xTaskCreateStatic','a733_cpu_coherence_publish','vTaskDelay','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write')
    assert all(symbols.get(n) for n in required),'Missing changed-path fixture symbol'
    board=layouts['board_application'];crypto=layouts['crypto_application'];results={}
    for variant in ('unsealed-refusal','sealed-extension-survives','prefix-mismatch-pins','prefix-mismatch-start','invalid-request-revokes','health-failure-queued'):
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x50000000,0x1000000)
        for address,data in segments:cpu.mem_write(address,data)
        raw=bytearray(session_path.read_bytes());assert len(raw)==824
        raw[48:80]=bytes(cpu.mem_read(symbols['a733_smp_build_id'],32));cpu.mem_write(0x41f00000,bytes(raw))
        cpu.mem_write(symbols['board']+board['admitted_session'],bytes(raw[:592]));cpu.mem_write(symbols['board'],bytes(raw[:592]));cpu.mem_write(symbols['board']+160,struct.pack('<Q',1000000000000));cpu.mem_write(symbols['board']+board['live_until'],struct.pack('<Q',1000000000000))
        contract=bytearray(224);struct.pack_into('<Q',contract,0,0x4153373343524331);struct.pack_into('<Q',contract,88,24000024);struct.pack_into('<2Q',contract,176,0x41f00000,824);contract[192:224]=hashlib.sha256(raw).digest();cpu.mem_write(0x50001000,bytes(contract))
        calls=[];finished=[];task_creates=[];writes=[]
        hook_names=('crypto_construct','xQueueSemaphoreTake','a733_cpu_coherence_publish','xTaskCreateStatic','vTaskDelay','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write')
        hooks={symbols[n]:n for n in hook_names}
        def returned(value):cpu.reg_write(UC_ARM64_REG_X0,value&0xffffffffffffffff);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
        def hook(machine,address,size,unused):
            if address==0x50fff000:finished.append(True);machine.emu_stop();return
            name=hooks.get(address)
            if name is None:return
            calls.append(name)
            if name=='a733_cpu_coherence_publish':assert machine.reg_read(UC_ARM64_REG_X1)>=824 and machine.reg_read(UC_ARM64_REG_X2)==24000024
            if name=='vTaskDelay':finished.append(True);machine.emu_stop();return
            if name=='crypto_construct':returned(1)
            elif name=='xQueueSemaphoreTake':returned(0)
            elif name=='xTaskCreateStatic':task_creates.append(True);returned(0x50010000)
            elif name=='a733_native_usb_health':returned(-1 if variant=='health-failure-queued' else 1)
            elif name=='tud_vendor_n_read':machine.mem_write(machine.reg_read(UC_ARM64_REG_X1),b'\x01');returned(1)
            elif name=='tud_vendor_n_write':writes.append(True);returned(0)
            else:returned(1)
        cpu.hook_add(UC_HOOK_CODE,hook)
        def call(name,arg=0):
            finished.clear();cpu.reg_write(UC_ARM64_REG_X0,arg);cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_LR,0x50fff000);cpu.emu_start(symbols[name],0,count=1000000);assert finished,(variant,name);return cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
        if variant=='unsealed-refusal':
            assert call('board_application_pins',0x50002000)==0 and call('board_application_start')==0 and not task_creates
        else:
            assert call('crypto_session_prepare',0x50001000)==1
            if variant in ('sealed-extension-survives','prefix-mismatch-pins','prefix-mismatch-start'):
                changed=bytearray(raw);changed[616]^=1;changed[664]^=1;changed[696]^=1;cpu.mem_write(0x41f00000,bytes(changed))
                if variant=='prefix-mismatch-pins':cpu.mem_write(symbols['board']+board['admitted_session']+456,struct.pack('<Q',99))
                expected=0 if variant=='prefix-mismatch-pins' else 1;assert call('board_application_pins',0x50002000)==expected
                if variant=='prefix-mismatch-start':cpu.mem_write(symbols['board']+board['admitted_session']+456,struct.pack('<Q',99))
                start=call('board_application_start');assert start==(1 if variant=='sealed-extension-survives' else 0)
                assert len(task_creates)==(variant=='sealed-extension-survives')
                if start:assert bytes(cpu.mem_read(symbols['crypto'],824))==bytes(raw)
            else:
                assert call('board_live')==1
                cpu.mem_write(symbols['crypto']+crypto['result'],struct.pack('<I',1))
                if variant=='health-failure-queued':cpu.mem_write(symbols['crypto_response_bytes'],struct.pack('<I',288))
                call('board_usb0_task')
                fatal=struct.unpack('<I',cpu.mem_read(symbols['board']+board['fatal'],4))[0];watchdog=struct.unpack('<I',cpu.mem_read(symbols['board']+board['watchdog_state'],4))[0]
                assert fatal==1 and watchdog&4 and call('board_live')==0 and not writes
                assert call('crypto_output_ready')==0
                call('crypto_task');assert struct.unpack('<I',cpu.mem_read(symbols['crypto']+crypto['result'],4))[0]==2
        if variant in ('unsealed-refusal','prefix-mismatch-pins','prefix-mismatch-start'):assert 'crypto_construct' not in calls
        if variant=='sealed-extension-survives':assert calls.count('crypto_construct')==1
        results[variant]=dict(exit=0,calls=calls,task_creates=len(task_creates),vendor_writes=len(writes))
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),cases=results,scope='Actual linked seal/consumer, USB stop, board_fail/board_live and Crypto revocation instructions. Coherence publication, USB provider observations and RTOS boundaries modeled. No SHA/AES arithmetic or device execution.'),indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','session','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();run(a.image,a.session,a.output)
