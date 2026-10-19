#!/usr/bin/env python3
"""Compose actual native upload, allocation, IOMMU, controller and copied output.

Power/admission, MMIO, USB and device pixel writes are explicit models. The
allocator, page-table builder, parser, register controller, guards, copying and
serializer execute from the linked image. No codec arithmetic is emulated.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4,UC_ARM64_REG_X5,UC_ARM64_REG_X6,UC_ARM64_REG_X7,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
REGS=[UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4,UC_ARM64_REG_X5,UC_ARM64_REG_X6,UC_ARM64_REG_X7]
STOP=0x50fff000
OPS=0x50f00000
STUB=0x50f10000
POOL=0x50e00000

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','inputs','output'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(exist_ok=False)
    cases=module('native_cases',ROOT/'checks/h264-application-cases.py')
    syms,segments,types,_,_=cases.setup(args.image,args.output,session_optional=True,fixture_optional=True)
    observe=module('observe',ROOT/'apps/h264/observe.py')
    app=types['h264_application'];sup=types['a733_h264_input'];run=types['a733_h264_run'];base=syms['h264'];upload=base+app['supplied'];rbase=base+app['run']
    reference=(args.inputs/'reference.yuv').read_bytes();assert len(reference)==4608
    reports=[]
    for variant in ('nominal','normal','P-error'):
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x50000000,0x1000000)
        for address,data in segments:cpu.mem_write(address,data)
        mode='normal' if variant=='normal' else 'qualification'
        session=(args.inputs/(mode+'-session.bin')).read_bytes();wire=(args.inputs/(mode+'-upload.bin')).read_bytes()
        oracle=None if mode=='normal' else reference
        cpu.mem_write(base,session);cpu.mem_write(syms['board']+24,session[24:40]);cpu.mem_write(syms['board']+160,struct.pack('<Q',1000000))
        cpu.mem_write(syms['providers']+types['board_provider']['shared']+types['a733_native_usb']['pmic_ready'],struct.pack('<I',1))
        cpu.mem_write(base+app['iommu']+types['a733_iommu']['fault']+types['a733_iommu_fault']['scanned_masters'],struct.pack('<H',0xc0))
        def get(address,n=8):return int.from_bytes(cpu.mem_read(address,n),'little')
        def setfield(address,type_name,field,value,size=8):cpu.mem_write(address+types[type_name][field],value.to_bytes(size,'little'))
        def returned(value):cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
        fifo=bytearray();time=[1000];mmio={0:1,0x1c:0,0xb0:0,0xec:0,0x228:0,0x5c:0,0x804:0,0xf0:0,0xe0:0x33310,0xe4:0x21320}
        active=[False];status_reads=[0];sram=[0]*1024;sram_offset=[0];triggers=[];tables=[];calls=[];parked=[];guard_reads=[]
        def buffers():return list(struct.unpack('<6Q',cpu.mem_read(base+app['buffers'],48)))
        def hook(m,address,size,unused):
            if address==STOP:parked.append('return');m.emu_stop();return
            if address==syms['vTaskDelay']:parked.append('delay');m.emu_stop();return
            if STUB<=address<STUB+6*16:
                kind=(address-STUB)//16
                if kind==0:time[0]+=1;m.mem_write(m.reg_read(REGS[1]),struct.pack('<Q',time[0]));returned(1)
                elif kind==1:m.mem_write(m.reg_read(REGS[3]),struct.pack('<Q',13));calls.append('acquire');returned(1)
                elif kind==2:calls.append('prepare');returned(1)
                elif kind==3:
                    off=m.reg_read(REGS[2]);p=get(rbase+run['picture_index'],4)
                    if off==0x228 and active[0]:
                        status_reads[0]+=1
                        value=3 if variant=='P-error' and p==1 else 1|(1<<23 if status_reads[0]<3 else 0)
                    elif off==0x22c:value=6
                    else:value=mmio.get(off,0)
                    m.mem_write(m.reg_read(REGS[3]),struct.pack('<I',value));returned(1)
                elif kind==4:
                    off=m.reg_read(REGS[2]);value=m.reg_read(REGS[3]);mmio[off]=value
                    if off==0x2e0:sram_offset[0]=value//4
                    if off==0x2e4:sram[sram_offset[0]]=value;sram_offset[0]+=1
                    if off==0x228:mmio[off]=0;active[0]=False
                    if off==0x224 and value==8:
                        p=get(rbase+run['picture_index'],4);assert p==get(base+app['completed'],4)
                        b=buffers();assert mmio[0x24c]==p
                        assert mmio[0x250]==0x10020000 and mmio[0x254]==0x10070000
                        assert mmio[0x234]==0 and mmio[0x23c]==0x10001000+p*4096+4095
                        for i in range(p+1):
                            assert sram[256+8*i:256+8*i+7]==[2*i,2*i,0,0x10010000+i*4096,0x10014000+i*4096,0x10068000+i*4096,0x10068400+i*4096]
                        assert sram[400]==0
                        if p:
                            assert bytes(m.mem_read(base+app['payload'],2304))==reference[:2304]
                            for row in range(32):assert bytes(m.mem_read(b[1]+row*64,48))==reference[row*48:row*48+48]
                        table=get(base+app['table']);tables.append(hashlib.sha256(m.mem_read(table,32768)).hexdigest())
                        triggers.append(dict(picture=p,registers=dict(mmio),sram=sram.copy()))
                        if not(variant=='P-error' and p==1):
                            pixels=reference[p*2304:(p+1)*2304]
                            for row in range(32):m.mem_write(b[1]+p*4096+row*64,pixels[row*48:row*48+48])
                            for plane in range(2):
                                for row in range(16):m.mem_write(b[2]+p*4096+plane*512+row*32,pixels[1536+plane*384+row*24:1536+plane*384+row*24+24])
                        active[0]=True;status_reads[0]=0
                    returned(1)
                else:m.reg_write(REGS[0],base);m.reg_write(UC_ARM64_REG_PC,syms['h264_copied'])
                return
            name=next((n for n in hooks if syms.get(n)==address),None)
            if not name:return
            calls.append(name)
            if name in ('board_live','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','a733_usb_prepare','a733_h264_platform_init'):returned(1)
            elif name=='board_fail':raise AssertionError('Unexpected board revocation')
            elif name=='tud_vendor_n_available':returned(min(17,len(fifo)))
            elif name=='tud_vendor_n_read':
                n=m.reg_read(REGS[2]);assert 0<n<=min(17,len(fifo));m.mem_write(m.reg_read(REGS[1]),bytes(fifo[:n]));del fifo[:n];returned(n)
            elif name=='a733_counter_clock_now':time[0]+=1;m.mem_write(m.reg_read(REGS[1]),struct.pack('<Q',time[0]));returned(1)
            elif name=='a733_rail_acquire':setfield(base+app['rail'],'a733_rail_token','id',1);returned(0)
            elif name=='a733_rail_poll':returned(0)
            elif name=='a733_pmic_submit':setfield(base+app['request'],'a733_pmic_request','id',1);returned(0)
            elif name=='a733_pmic_result':
                pointer=m.reg_read(REGS[2])
                for field,value in [('rail',1),('chip',3),('enabled',1),('decoded',1),('programmed_uv',800000)]:setfield(pointer,'a733_pmic_snapshot',field,value,1 if field in ('chip','enabled','decoded') else 4)
                returned(2)
            elif name=='a733_clock_acquire':
                # Both clock outputs are fields of the actual application.
                pointer=m.reg_read(REGS[7]);setfield(pointer,'a733_clock_token','id',1);returned(0)
            elif name=='a733_gpu_resources_acquire':m.mem_write(m.reg_read(REGS[4]),struct.pack('<Q',1));returned(0)
            elif name=='a733_memory_uncached_init':returned(POOL)
            elif name=='a733_video_platform_h264_ops':returned(OPS)
            elif name=='h264_load':m.mem_write(m.reg_read(REGS[1]),struct.pack('<I',mmio[0x228] if not active[0] else 0));guard_reads.append(mmio[0x228]);returned(1)
            elif name in ('a733_iommu_scan','a733_iommu_poll'):returned(1)
        hooks=['board_live','board_fail','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','a733_usb_prepare','tud_vendor_n_available','tud_vendor_n_read','a733_counter_clock_now','a733_rail_acquire','a733_rail_poll','a733_pmic_submit','a733_pmic_result','a733_clock_acquire','a733_gpu_resources_acquire','a733_memory_uncached_init','a733_h264_platform_init','a733_video_platform_h264_ops','h264_load','a733_iommu_scan','a733_iommu_poll']
        assert all(n in syms for n in hooks),[n for n in hooks if n not in syms]
        cpu.mem_write(OPS,struct.pack('<6Q',*[STUB+16*i for i in range(6)]));cpu.hook_add(UC_HOOK_CODE,hook)
        def invoke(name,*values):
            for reg,value in zip(REGS,values):cpu.reg_write(reg,value)
            cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_LR,STOP);parked.clear()
            cpu.emu_start(syms[name],0,count=8000000);assert parked,(name,'instruction budget');return cpu.reg_read(REGS[0])
        assert invoke('device_memory_pool_init',POOL,0x50000000,0x50000000,0x200000)==1
        # Run the real USB receiver in fragments, then real task before commit.
        fifo.extend(wire[:40])
        while fifo:invoke('board_usb0_task',0)
        invoke('h264_task',0);assert not calls.count('a733_rail_acquire') and not get(base+app['stage'],4)
        fifo.extend(wire[40:])
        while fifo:invoke('board_usb0_task',0)
        assert get(upload+sup['committed'],4)==1
        snapshots=[]
        def query(index):
            fifo.extend(b'\0'+struct.pack('<I',index))
            while fifo:invoke('board_usb0_task',0)
            frame=bytes(cpu.mem_read(syms['h264_response'],2464));status=int.from_bytes(frame[8:12],'little')
            if status!=1:frame=frame[:160]
            # USB transport completion is modeled; execute actual TX ownership callback.
            cpu.mem_write(syms['h264_response_sent'],struct.pack('<I',len(frame)))
            invoke('tud_vendor_tx_cb',0,len(frame))
            return frame
        queried=False
        for iteration in range(5000):
            invoke('h264_task',0)
            if get(base+app['completed'],4)==1 and not queried:
                frame=query(0);assert observe.decode(frame,session,oracle,0)['copied_verified'];snapshots.append(frame);queried=True
            if get(base+app['result'],4):break
        if not(queried and len(triggers)==2 and calls.count('acquire')==1 and calls.count('prepare')==1):
            (args.output/'failure.json').write_text(json.dumps(dict(variant=variant,queried=queried,triggers=len(triggers),result=get(base+app['result'],4),stage=get(base+app['stage'],4),state=get(rbase+run['state'],4),index=get(rbase+run['picture_index'],4),calls=calls[-50:]),indent=2))
        assert queried and len(triggers)==2 and calls.count('acquire')==1 and calls.count('prepare')==1
        terminal=get(base+app['result'],4);assert terminal==(2 if variant=='P-error' else 1)
        assert tables[0]==tables[1] # No live page-table mutation or slot remapping.
        frame0=query(0);decoded=observe.decode(frame0,session,oracle,0);assert decoded['copied_verified']
        assert frame0[160:]==snapshots[0][160:] and frame0[96:128]==snapshots[0][96:128]
        frame1=query(1);decoded=observe.decode(frame1,session,oracle,1)
        assert decoded['status']==(2 if variant=='P-error' else 1)
        assert get(base+app['completed'],4)==(1 if variant=='P-error' else 2)
        for i,frame in enumerate([frame0,frame1]):(args.output/f'{variant}-frame{i}.bin').write_bytes(frame)
        # Actual table validation checks every emitted descriptor against the
        # declared spans. Reference permissions remain RW without live remaps.
        assert invoke('a733_iommu_validate',base+app['tables'])==1
        spans=base+app['tables']+types['a733_iommu_tables']['spans']
        declared=[struct.unpack('<3QI4x',cpu.mem_read(spans+32*i,32)) for i in range(6)]
        assert declared[1]==(0x10010000,buffers()[1],8192,3)
        assert declared[2]==(0x10014000,buffers()[2],8192,3)
        assert declared[0][3]==1
        assert not any(va<=0x10003000<va+n for va,pa,n,access in declared)
        (args.output/f'{variant}-tables.bin').write_bytes(cpu.mem_read(get(base+app['table']),32768))
        reports.append(dict(case=variant,terminal=terminal,completed=get(base+app['completed'],4),triggers=triggers,table_sha256=tables[0],spans=declared,calls=calls))
    (args.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(),cases=reports),indent=2)+'\n')
    print('PASS composed native IDR+P upload, owned DMA/IOMMU, reference slots, two copies, indexed USB and retained partial failure')

if __name__=='__main__':main()
