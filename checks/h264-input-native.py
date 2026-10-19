#!/usr/bin/env python3
"""Run changed native upload/copy instructions over explicit USB/DMA models.

This does not emulate USB hardware or H.264 entropy decoding. Device output
samples are injected; copied output and its metadata must match those bytes.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','inputs','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--serializer-only',action='store_true',help='Reuse accepted upload and refusal evidence; check actual copied-output serializer')
    a=p.parse_args();a.output.mkdir(exist_ok=False)
    cases=module('h264_cases',ROOT/'checks/h264-application-cases.py')
    symbols,segments,layouts,_,_=cases.setup(a.image,a.output,session_optional=True,fixture_optional=True)
    layout=layouts['h264_application'];supplied=layouts['a733_h264_input'];base=symbols['h264'];upload=base+layout['supplied']
    observe=module('h264_observe',ROOT/'apps/h264/observe.py');results=[]
    def machine(session):
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x50000000,0x1000000)
        for address,data in segments:cpu.mem_write(address,data)
        cpu.mem_write(base,session);cpu.mem_write(symbols['board']+24,session[24:40]);cpu.mem_write(symbols['board']+160,struct.pack('<Q',1000000))
        cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_LR,0x50fff000)
        return cpu
    def word(cpu,address,size=4):return int.from_bytes(cpu.mem_read(address,size),'little')
    upload_cases=[('normal-coalesced','normal',10000),('oracle-fragments','qualification',17),
                             ('partial','normal',17),('duplicate','normal',512),('late-commit','normal',512)]
    for name,mode,limit in ([] if a.serializer_only else upload_cases):
        session=(a.inputs/(mode+'-session.bin')).read_bytes();wire=(a.inputs/(mode+'-upload.bin')).read_bytes()
        fifo=bytearray(wire if name not in ('partial','duplicate') else wire[:-1] if name=='partial' else wire+wire[:32])
        cpu=machine(session);delays=[0];failed=[];clock_reads=[0]
        def returned(value):cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
        def hook(m,address,size,unused):
            if address==symbols['vTaskDelay']:
                delays[0]+=1
                if not fifo or failed:m.emu_stop()
                else:returned(0)
            elif address==symbols['board_fail']:failed.append(True);returned(0)
            elif address==symbols['a733_counter_clock_now']:
                clock_reads[0]+=1
                complete=word(m,upload+supplied['received'],8)==len(wire)-32
                now=990000 if name=='late-commit' and complete else 1000+clock_reads[0]
                m.mem_write(m.reg_read(UC_ARM64_REG_X1),struct.pack('<Q',now));returned(1)
            elif address==symbols.get('tud_vendor_n_available'):returned(min(limit,len(fifo)))
            elif address==symbols.get('tud_vendor_n_read'):
                n=m.reg_read(UC_ARM64_REG_X2);assert 0<n<=min(limit,len(fifo))
                m.mem_write(m.reg_read(UC_ARM64_REG_X1),bytes(fifo[:n]));del fifo[:n];returned(n)
            elif address in {symbols.get(n) for n in ('board_live','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','a733_usb_prepare')}:returned(1)
        usb_hook=cpu.hook_add(UC_HOOK_CODE,hook);cpu.emu_start(symbols['board_usb0_task'],0,count=4000000)
        assert delays[0] and (not fifo or failed)
        committed=word(cpu,upload+supplied['committed'])
        expected=name not in ('partial','late-commit')
        assert bool(committed)==expected,(name,committed)
        assert bool(failed)==(name in ('duplicate','late-commit')),(name,failed)
        assert not word(cpu,base+layout['stage']) # USB upload alone does not acquire VE.
        if committed:assert bytes(cpu.mem_read(upload+supplied['stream'],len(wire)-32-(2304 if mode=='qualification' else 0)))==wire[32:len(wire)-(2304 if mode=='qualification' else 0)]
        if name=='partial':
            # Actual decoder task remains before setup/provider acquisition.
            cpu.hook_del(usb_hook)
            seen=[]
            def wait(m,address,size,unused):
                if address==symbols['vTaskDelay']:seen.append(True);m.emu_stop()
                elif address==symbols.get('a733_h264_begin'):raise AssertionError('Provider path reached before publication')
                elif address==symbols['board_live']:returned(1)
            cpu.hook_add(UC_HOOK_CODE,wait);cpu.emu_start(symbols['h264_task'],0,count=100000);assert seen
        results.append(dict(case=name,committed=bool(committed),retained_failure=bool(failed),usb_delays=delays[0],clock_reads=clock_reads[0]))
    # Exercise actual copied-output code with the same injected pixels in normal
    # and qualification modes. Normal mode checks ownership/integrity only.
    reference=(a.inputs/'qualification-upload.bin').read_bytes()[-2304:]
    for mode in (('normal','qualification') if a.serializer_only else ('normal','qualification','oracle-mismatch')):
        session=(a.inputs/('normal-session.bin' if mode=='normal' else 'qualification-session.bin')).read_bytes();cpu=machine(session)
        cpu.mem_write(upload+supplied['committed'],struct.pack('<I',1))
        if mode!='normal':
            cpu.mem_write(upload+supplied['oracle_bytes'],struct.pack('<Q',2304));cpu.mem_write(upload+supplied['oracle'],reference)
        addresses=[0x50001000,0x50005000,0x50009000,0x5000d000,0x50031000,0x50038000]
        allocation=[0x50000000,0x50004000,0x50008000,0x5000c000,0x50030000,0x50034000]
        capacity=[4096,4096,4096,0x21000,4096,0xc000];prefix=[4096,4096,4096,4096,4096,16384]
        cpu.mem_write(base+layout['buffers'],struct.pack('<6Q',*addresses))
        for field,address in zip(('input','luma','chroma'),addresses):cpu.mem_write(base+layout[field],struct.pack('<Q',address))
        for address,n,before in zip(allocation,capacity,prefix):cpu.mem_write(address,bytes([0xa5])*(n+before+4096))
        pixels=bytes([reference[0]^1])+reference[1:] if mode=='oracle-mismatch' else reference
        for row in range(32):cpu.mem_write(addresses[1]+row*64,pixels[row*48:row*48+48])
        for plane in range(2):
            for row in range(16):cpu.mem_write(addresses[2]+plane*512+row*32,pixels[1536+plane*384+row*24:1536+plane*384+row*24+24])
        cpu.mem_write(base+layout['iommu']+layouts['a733_iommu']['fault']+layouts['a733_iommu_fault']['scanned_masters'],struct.pack('<H',0xc0))
        descriptions=[0];finished=[]
        def returned(value):cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
        def copy_hook(m,address,size,unused):
            if address==0x50fff000:finished.append(True);m.emu_stop()
            elif address==symbols['device_memory_describe']:
                i=descriptions[0]%6;descriptions[0]+=1
                m.mem_write(m.reg_read(UC_ARM64_REG_X2),struct.pack('<3Q',allocation[i],allocation[i],capacity[i]+prefix[i]+4096))
                m.mem_write(m.reg_read(UC_ARM64_REG_X3),struct.pack('<I',2));returned(1)
            elif address==symbols['h264_load']:m.mem_write(m.reg_read(UC_ARM64_REG_X1),bytes(4));returned(1)
            elif address in {symbols[n] for n in ('board_live','a733_iommu_scan','a733_iommu_poll')}:returned(1)
        cpu.hook_add(UC_HOOK_CODE,copy_hook);returns=[]
        for _ in range(1 if mode=='oracle-mismatch' else 2):
            cpu.reg_write(UC_ARM64_REG_X0,base);cpu.reg_write(UC_ARM64_REG_X1,13);cpu.reg_write(UC_ARM64_REG_X2,990000)
            cpu.reg_write(UC_ARM64_REG_LR,0x50fff000);cpu.emu_start(symbols['h264_copied'],0,count=4000000);assert finished;finished.clear()
            returns.append(cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff)
        assert returns==([0xffffffff] if mode=='oracle-mismatch' else [0,1]),(mode,returns)
        if mode!='oracle-mismatch':
            payload=bytes(cpu.mem_read(base+layout['payload'],2304));digest=bytes(cpu.mem_read(base+layout['digest'],32))
            assert payload==pixels and digest==hashlib.sha256(payload).digest()
            cpu.mem_write(base+layout['result'],struct.pack('<I',1))
            queried=[]
            def query_hook(m,address,size,unused):
                if address==symbols['vTaskDelay']:queried.append(True);m.emu_stop()
                elif address==symbols.get('tud_vendor_n_available'):returned(1)
                elif address==symbols.get('tud_vendor_n_read'):
                    assert m.reg_read(UC_ARM64_REG_X2)==1
                    m.mem_write(m.reg_read(UC_ARM64_REG_X1),b'\0');returned(1)
                elif address in {symbols.get(n) for n in ('a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','a733_usb_prepare')}:returned(1)
            cpu.hook_add(UC_HOOK_CODE,query_hook)
            cpu.emu_start(symbols['board_usb0_task'],0,count=4000000);assert queried
            frame=bytes(cpu.mem_read(symbols['h264_response'],2432))
            decoded=observe.decode(frame,session,None if mode=='normal' else reference)
            assert decoded['copied_verified'] and decoded['oracle_verified']==(mode=='qualification')
            (a.output/(mode+'-frame.bin')).write_bytes(frame)
        results.append(dict(case='copy-'+mode,returns=returns,injected_device_pixels=True))
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),cases=results),indent=2)+'\n')
    print('PASS native fragmented upload/publication, partial/late/duplicate refusal, normal and qualification copied outputs')

if __name__=='__main__':main()
