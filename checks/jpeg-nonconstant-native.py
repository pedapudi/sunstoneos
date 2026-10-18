#!/usr/bin/env python3
"""Execute only nonconstant copy/layout cases in the actual linked JPEG image."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def run(image,oracle,out):
    out.mkdir(parents=True,exist_ok=False)
    reference=(oracle/'reference.yuv').read_bytes();fixture=(oracle/'fixture.jpg').read_bytes()
    profiles=module('nonconstant_profiles',ROOT/'apps/jpeg/fixture.py');profile=profiles.profile_for(hashlib.sha256(fixture).hexdigest(),hashlib.sha256(reference).hexdigest())
    assert profile['name']=='nonconstant'
    with image.open('rb') as f:
        elf=ELFFile(f);symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        segments=[(int(p['p_vaddr']),bytes(p.data())) for p in elf.iter_segments() if p['p_type']=='PT_LOAD'];layouts={}
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                name=die.attributes.get('DW_AT_name')
                if die.tag=='DW_TAG_structure_type' and name:
                    fields={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in die.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes and 'DW_AT_data_member_location' in c.attributes}
                    if fields:layouts[name.value.decode()]=fields
    required=['jpeg','board','jpeg_copied','jpeg_response','board_usb0_task','board_live','a733_iommu_scan','a733_iommu_poll','device_memory_describe','jpeg_load','vTaskDelay','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','tud_vendor_n_available','tud_vendor_n_read']
    assert all(symbols.get(n) for n in required),'Missing public entry/helper symbols before instruction cases'
    layout=layouts['jpeg_application'];iommu=layouts['a733_iommu'];fault=layouts['a733_iommu_fault']
    assert {'luma','chroma','iommu','payload','digest','copy_step','result'}<=layout.keys()
    session=bytearray(856);struct.pack_into('<6Q',session,0,0x4153373342535332,2,592,1,2,3);struct.pack_into('<I',session,400,2)
    struct.pack_into('<3Q',session,592,0x415337334a504132,1,856);struct.pack_into('<3Q',session,616,12,13,0)
    for start in (640,672,704,736):session[start]=1
    session[768:800]=hashlib.sha256(fixture).digest();session[800:832]=hashlib.sha256(reference).digest()
    struct.pack_into('<6I',session,832,1,234000000,0x10007,0x10007,800000,1)
    (out/'session-fixture.bin').write_bytes(session)
    prep=module('nonconstant_prepare',ROOT/'apps/jpeg/prepare.py');assert prep.embedded_reference(image.read_bytes(),session)==reference
    variants=('verified-padding','swapped-chroma','luma-stride','chroma-stride','chroma-sampling','table-sample-error');results={}
    for variant in variants:
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x50000000,0x1000000)
        for address,data in segments:cpu.mem_write(address,data)
        base=symbols['jpeg'];cpu.mem_write(base,bytes(session));cpu.mem_write(symbols['board']+160,struct.pack('<Q',1000000))
        for field,address in [('input',0x50001000),('luma',0x50005000),('chroma',0x50009000)]:cpu.mem_write(base+layout[field],struct.pack('<Q',address))
        for address in (0x50000000,0x50004000,0x50008000):cpu.mem_write(address,bytes([0xa5])*12288)
        cpu.mem_write(0x50001000,fixture+bytes(4096-len(fixture)))
        # Decoder-owned padded allocations may change; guards and unused tails may not.
        cpu.mem_write(0x50005000,bytes([0xcc])*2048);cpu.mem_write(0x50009000,bytes([0xdd])*1024)
        for row in range(32):cpu.mem_write(0x50005000+row*(48 if variant=='luma-stride' else 64),reference[row*48:row*48+48])
        for plane in range(2):
            source=1-plane if variant=='swapped-chroma' else plane
            for row in range(16):
                sampled=row//2 if variant=='chroma-sampling' else row
                offset=1536+source*384+sampled*24
                cpu.mem_write(0x50009000+plane*512+row*(24 if variant=='chroma-stride' else 32),reference[offset:offset+24])
        if variant=='table-sample-error':cpu.mem_write(0x50005000,bytes([reference[0]^1]))
        cpu.mem_write(base+layout['iommu']+iommu['fault']+fault['scanned_masters'],struct.pack('<H',0xc0))
        calls=[];describes=[0];done=[]
        hooks={symbols[n]:n for n in ('board_live','a733_iommu_scan','a733_iommu_poll','device_memory_describe','jpeg_load')}
        def returned(value):cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
        def hook(machine,address,size,unused):
            if address==0x50fff000:done.append(True);machine.emu_stop();return
            name=hooks.get(address)
            if name is None:return
            calls.append(name)
            if name=='device_memory_describe':
                extent=(0x50000000,0x50004000,0x50008000)[describes[0]%3];describes[0]+=1
                machine.mem_write(machine.reg_read(UC_ARM64_REG_X2),struct.pack('<3Q',extent,extent,12288));machine.mem_write(machine.reg_read(UC_ARM64_REG_X3),struct.pack('<I',2))
            elif name=='jpeg_load':machine.mem_write(machine.reg_read(UC_ARM64_REG_X1),bytes(4))
            returned(1)
        cpu.hook_add(UC_HOOK_CODE,hook);returns=[]
        for unused in range(2 if variant=='verified-padding' else 1):
            for reg,value in ((UC_ARM64_REG_X0,base),(UC_ARM64_REG_X1,13),(UC_ARM64_REG_X2,990000),(UC_ARM64_REG_SP,0x50ffeff0),(UC_ARM64_REG_LR,0x50fff000)):cpu.reg_write(reg,value)
            done.clear();cpu.emu_start(symbols['jpeg_copied'],0,count=1000000);assert done,'Copy instruction allowance exceeded'
            returns.append(cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff)
        assert returns==([0,1] if variant=='verified-padding' else [0xffffffff]),(variant,returns)
        if variant=='verified-padding':
            assert bytes(cpu.mem_read(base+layout['payload'],2304))==reference
            assert bytes(cpu.mem_read(base+layout['digest'],32))==hashlib.sha256(reference).digest()
            cpu.mem_write(base+layout['result'],struct.pack('<I',1));cpu.mem_write(symbols['board']+24,struct.pack('<2Q',1,2))
            def usb_hook(machine,address,size,unused):
                if address==symbols['vTaskDelay']:done.append(True);machine.emu_stop();return
                if address in {symbols[n] for n in ('a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext')}:returned(1)
                elif address==symbols['tud_vendor_n_available']:returned(1)
                elif address==symbols['tud_vendor_n_read']:machine.mem_write(machine.reg_read(UC_ARM64_REG_X1),b'\x00');returned(1)
            usb=cpu.hook_add(UC_HOOK_CODE,usb_hook);cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);done.clear();cpu.emu_start(symbols['board_usb0_task'],0,count=1000000);assert done
            frame=bytes(cpu.mem_read(symbols['jpeg_response'],2432));observe=module('nonconstant_observe',ROOT/'apps/jpeg/observe.py')
            assert observe.decode(frame,session,reference)['copied_verified'];(out/'native-frame.bin').write_bytes(frame);cpu.hook_del(usb)
        results[variant]={'exit':0,'returns':returns,'modeled':'Provider admission, MMIO and IOMMU observations; actual native private-copy comparison and USB frame code.'}
    (out/'results.json').write_text(json.dumps({'passed':True,'hardware_access':False,'cases':results},indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','oracle','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();run(a.image,a.oracle,a.output)
