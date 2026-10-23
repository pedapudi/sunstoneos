#!/usr/bin/env python3
"""Compose full-HD coded references, visible copied output and retained failure.

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
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4,UC_ARM64_REG_X5,UC_ARM64_REG_X6,UC_ARM64_REG_X7,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
ROOT=Path(__file__).resolve().parents[1]
REGS=[UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4,UC_ARM64_REG_X5,UC_ARM64_REG_X6,UC_ARM64_REG_X7]
STOP=0x7afff000
OPS=0x7af00000
STUB=0x7af10000
POOL=0x7ae00000

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','inputs','output'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(exist_ok=False)
    cases=module('native_cases',ROOT/'checks/h264-application-cases.py')
    syms,segments,types,_,_=cases.setup(args.image,args.output,session_optional=True,fixture_optional=True)
    observe=module('stream_host',ROOT/'apps/h264/stream.py')
    for lo,hi in [('__device_pool_start','__device_pool_end'),('__uncached_pool_start','__uncached_pool_end')]:
        assert 0x7b000000<=syms[lo] or syms[hi]<=0x7a000000, 'Harness scratch/stack overlaps linked pool'
    app=types['h264_application'];sup=types['a733_h264_input'];run=types['a733_h264_run'];base=syms['h264'];upload=base+app['supplied'];rbase=base+app['run']
    reference=(args.inputs/'reference.yuv').read_bytes();assert len(reference)==6*3110400
    streambase=syms['h264_stream'];streamtype=types['h264_stream'];outputtype=types['h264_stream_output']
    output_size=None
    from elftools.elf.elffile import ELFFile
    with args.image.open('rb') as file:
        elf=ELFFile(file)
        output_size=next(d.attributes['DW_AT_byte_size'].value for cu in elf.get_dwarf_info().iter_CUs() for d in cu.iter_DIEs() if d.tag=='DW_TAG_structure_type' and d.attributes.get('DW_AT_name') and d.attributes['DW_AT_name'].value==b'h264_stream_output' and 'DW_AT_byte_size' in d.attributes)
    coded=[]
    for i in range(6):
        pixels=reference[i*3110400:(i+1)*3110400]
        y=pixels[:2073600]+bytes([0x61+i])*(1920*8)
        c=pixels[2073600:2592000]+bytes([0x71+i])*(960*4)+pixels[2592000:]+bytes([0x81+i])*(960*4)
        assert len(y)==2088960 and len(c)==1044480;coded.append((y,c))
    reports=[]
    for variant in ('nominal','normal'):
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x7a000000,0x1000000);cpu.mem_map(0x48000000,0x1000000);cpu.mem_map(0x78000000,0x1000000)
        cpu.mem_map(0x01c0e000,4096);cpu.mem_map(0x02002000,4096)
        def device_read(m,access,address,size,value,unused):
            datum=0x10007 if address in (0x0200258c,0x020025b4) else mmio[0x228]
            if address==0x01c0e228 and copy_fail_at[0]==get(streambase+streamtype['active'],4) and get(base+app['copy_step'],4)==3 and get(base+app['copy_row'],4)>=64:datum=2
            m.mem_write(address,struct.pack('<I',datum));guard_reads.append(datum)
        cpu.hook_add(UC_HOOK_MEM_READ,device_read,begin=0x01c0e228,end=0x01c0e22b)
        for address,data in segments:
            if data:cpu.mem_write(address,data)
        mode='normal' if variant=='normal' else 'qualification'
        session=(args.inputs/(mode+'-session.bin')).read_bytes();manifest=(args.inputs/mode/'manifest.bin').read_bytes()
        frames=observe.validate_inputs(args.inputs/mode,manifest,3);wire=observe.manifest_upload(session,manifest)
        oracle=None if mode=='normal' else reference
        cpu.mem_write(base,session);cpu.mem_write(syms['board']+24,session[24:40]);cpu.mem_write(syms['board']+160,struct.pack('<Q',1000000))
        cpu.mem_write(syms['providers']+types['board_provider']['shared']+types['a733_native_usb']['pmic_ready'],struct.pack('<I',1))
        cpu.mem_write(base+app['iommu']+types['a733_iommu']['fault']+types['a733_iommu_fault']['scanned_masters'],struct.pack('<H',0xc0))
        def get(address,n=8):return int.from_bytes(cpu.mem_read(address,n),'little')
        def setfield(address,type_name,field,value,size=8):cpu.mem_write(address+types[type_name][field],value.to_bytes(size,'little'))
        def returned(value):cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
        fifo=bytearray();time=[1000];mmio={0:1,0x1c:0,0xb0:0,0xec:0,0x228:0,0x5c:0,0x804:0,0xf0:0,0xe0:0x33310,0xe4:0x21320}
        active=[False];status_reads=[0];fail_at=[None];scan_fail_at=[None];copy_fail_at=[None];tx=bytearray();delivered=[0];sram=[0]*1024;sram_offset=[0];triggers=[];tables=[];calls=[];cleans=[];parked=[];guard_reads=[];wakes=[0]
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
                        value=3 if fail_at[0]==p else 1|(1<<23 if status_reads[0]<3 else 0)
                    elif off==0x22c:value=8160
                    else:value=mmio.get(off,0)
                    m.mem_write(m.reg_read(REGS[3]),struct.pack('<I',value));returned(1)
                elif kind==4:
                    off=m.reg_read(REGS[2]);value=m.reg_read(REGS[3]);mmio[off]=value
                    if off==0x2e0:sram_offset[0]=value//4
                    if off==0x2e4:sram[sram_offset[0]]=value;sram_offset[0]+=1
                    if off==0x228:mmio[off]=0;active[0]=False
                    if off==0x224 and value==8:
                        p=get(rbase+run['picture_index'],4);slot=p&1;number=observe.descriptors(manifest,3)[p]['frame_num']
                        assert get(streambase+streamtype['au_state'],4)==2 and get(streambase+streamtype['active'],4)==p
                        b=buffers();assert mmio[0x24c]==slot
                        assert mmio[0xc4]==522240 and mmio[0xc8]==0x03c00780
                        assert mmio[0x250]==0x10a00000 and mmio[0x254]==0x10abc000
                        assert mmio[0x234]==0 and mmio[0x23c]==0x103fffff
                        assert sram[256+8*slot:256+8*slot+7]==[2*number,2*number,0,0x10401000+slot*2088960,0x10801000+slot*1044480,0x10a36000+slot*262144,0x10a36000+slot*262144+131072]
                        assert sram[400]==((slot^1)*2 if number else 0) and sram[401:408]==[0]*7
                        if number:
                            previous=observe.descriptors(manifest,3)[p-1]['frame_num'];refslot=slot^1
                            assert sram[256+8*refslot:256+8*refslot+7]==[2*previous,2*previous,0,0x10401000+refslot*2088960,0x10801000+refslot*1044480,0x10a36000+refslot*262144,0x10a36000+refslot*262144+131072]
                            assert bytes(m.mem_read(b[1]+refslot*2088960,2088960))==coded[p-1][0] and bytes(m.mem_read(b[2]+refslot*1044480,1044480))==coded[p-1][1]
                        table=get(base+app['table']);tables.append(hashlib.sha256(m.mem_read(table,32768)).hexdigest())
                        triggers.append(dict(picture=p,frame_num=number,registers=dict(mmio),sram=sram.copy()))
                        if fail_at[0]!=p:
                            m.mem_write(b[1]+slot*2088960,coded[p][0]);m.mem_write(b[2]+slot*1044480,coded[p][1])
                        active[0]=True;status_reads[0]=0
                    returned(1)
                else:m.reg_write(REGS[0],base);m.reg_write(UC_ARM64_REG_PC,syms['h264_copied'])
                return
            name=next((n for n in hooks if syms.get(n)==address),None)
            if not name:return
            calls.append(name)
            if name in ('board_live','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','a733_usb_prepare','a733_h264_platform_init'):returned(1)
            elif name=='board_fail':raise AssertionError('Unexpected board revocation')
            elif name=='tud_vendor_n_available':returned(min(512,len(fifo)))
            elif name=='tud_vendor_n_read':
                n=m.reg_read(REGS[2]);assert 0<n<=min(512,len(fifo));m.mem_write(m.reg_read(REGS[1]),bytes(fifo[:n]));del fifo[:n];returned(n)
            elif name=='tud_vendor_n_write':
                n=min(m.reg_read(REGS[2]),512);tx.extend(m.mem_read(m.reg_read(REGS[1]),n));delivered[0]+=n;returned(n)
            elif name=='tud_vendor_n_write_flush':returned(0)
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
            elif name=='a733_device_memory_ready':returned(1) # Modeled READY: the actual getters return the prepared pools.
            elif name in ('vTaskEnterCritical','vTaskExitCritical'):returned(0) # One emulated core: the ledger's critical section has no effect.
            elif name=='aarch64_device_cache':cleans.append([m.reg_read(r) for r in REGS[:3]]);returned(1) # Modeled clean of a recycled write-back extent.
            elif name=='a733_video_platform_h264_ops':returned(OPS)
            elif name=='aarch64_device_translate':
                # The extended pools carry their MAIR attribute and inner
                # shareability; other addresses keep the attribute-free model.
                va=m.reg_read(REGS[0]);attributes=(0x44,3) if 0x78000000<=va<0x79000000 else (0xff,3) if 0x48000000<=va<0x78000000 else (0,2)
                m.mem_write(m.reg_read(REGS[2]),struct.pack('<QII',va,*attributes));returned(1)
            elif name=='h264_device':returned(int(m.reg_read(REGS[0]) in list(range(0x17000,0x22000,4096))+[0x01c0e000]))
            elif name=='h264_load':
                value=0x10007 if m.reg_read(REGS[0]) in (0x0200258c,0x020025b4) else mmio[0x228] if not active[0] else 0
                if copy_fail_at[0]==get(streambase+streamtype['active'],4) and get(base+app['copy_step'],4)==3 and get(base+app['copy_row'],4)>=64:value=2
                m.mem_write(m.reg_read(REGS[1]),struct.pack('<I',value));guard_reads.append(value);returned(1)
            elif name=='a733_iommu_scan':
                returned(-1 if scan_fail_at[0]==get(streambase+streamtype['active'],4) else 1)
            elif name=='a733_iommu_poll':
                setfield(base+app['iommu']+types['a733_iommu']['fault'],'a733_iommu_fault','scanned_masters',0xc0,2);setfield(base+app['iommu']+types['a733_iommu']['fault'],'a733_iommu_fault','complete',1,1);returned(1)
        hooks=['board_live','board_fail','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','a733_usb_prepare','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush','a733_counter_clock_now','a733_rail_acquire','a733_rail_poll','a733_pmic_submit','a733_pmic_result','a733_clock_acquire','a733_gpu_resources_acquire','a733_device_memory_ready','vTaskEnterCritical','vTaskExitCritical','aarch64_device_cache','a733_h264_platform_init','a733_video_platform_h264_ops','h264_load','h264_device','a733_iommu_scan','a733_iommu_poll','aarch64_device_translate']
        assert all(n in syms for n in hooks),[n for n in hooks if n not in syms]
        cpu.mem_write(OPS,struct.pack('<6Q',*[STUB+16*i for i in range(6)]))
        # Only modeled service boundaries need hooks; CPU parser/hash/copy loops
        # execute unchanged without a Python callback for every instruction.
        boundaries={STOP,syms['vTaskDelay']}|{STUB+16*i for i in range(6)}|{syms[n] for n in hooks}
        for address in boundaries:cpu.hook_add(UC_HOOK_CODE,hook,begin=address,end=address)
        def invoke(name,*values):
            for reg,value in zip(REGS,values):cpu.reg_write(reg,value)
            cpu.reg_write(UC_ARM64_REG_SP,0x7affeff0);cpu.reg_write(UC_ARM64_REG_LR,STOP);parked.clear();wakes[0]+=name=='h264_task'
            cpu.emu_start(syms[name],0,count=800000000);assert parked,(name,'instruction budget');return cpu.reg_read(REGS[0])
        # The actual extended preparations type the pools: dma32 write-back and Normal non-cacheable.
        assert invoke('a733_memory_reservations_prepare')&0xffffffff==1 and invoke('a733_memory_uncached_prepare')&0xffffffff==1
        assert invoke('h264_stream_storage_init',12)==1 and invoke('h264_stream_storage_valid')==1
        def usb(packet=b''):
            fifo.extend(packet)
            while fifo:invoke('board_usb0_task',0)
        def oslot(i):return streambase+streamtype['output']+output_size*i
        def state(i):return get(oslot(i)+outputtype['state'],4)
        def advance(ordinal):
            for _ in range(5000):
                invoke('h264_task',0)
                if get(streambase+streamtype['next'],4)>ordinal or get(base+app['result'],4)==2:return
            raise AssertionError(('no completion',ordinal,get(rbase+run['state'],4)))
        def query(index):
            tx.clear();usb(observe.header(observe.Q,index,session))
            # Actual serializer separates header/payload and pins the copied slot.
            for _ in range(10000):
                invoke('board_usb0_task',0)
                if delivered[0]:n=delivered[0];delivered[0]=0;invoke('tud_vendor_tx_cb',0,n)
                if len(tx)>=192:
                    needed=192+int.from_bytes(tx[12:16],'little')
                    if len(tx)==needed:return bytes(tx)
            raise AssertionError('serializer budget')
        def ack(index,frame):usb(observe.header(observe.A,index,session)+frame[96:128])
        def checkpoint():
            return [bytes(cpu.mem_read(0x41000000,0x800000)),bytes(cpu.mem_read(0x7a000000,0x1000000)),bytes(cpu.mem_read(0x48000000,0x1000000)),bytes(cpu.mem_read(0x78000000,0x1000000)),time[0]]
        def restore(saved):
            cpu.mem_write(0x41000000,saved[0]);cpu.mem_write(0x7a000000,saved[1]);cpu.mem_write(0x48000000,saved[2]);cpu.mem_write(0x78000000,saved[3]);time[0]=saved[4]
            fifo.clear();tx.clear();delivered[0]=0;active[0]=False;status_reads[0]=0;fail_at[0]=None;scan_fail_at[0]=None;copy_fail_at[0]=None
        usb(wire[:24]);invoke('h264_task',0);assert not get(base+app['stage'],4) and not calls.count('a733_rail_acquire')
        usb(wire[24:]);assert get(streambase+streamtype['manifest_ready'],4)==1
        snapshots=[];branches=[]
        for ordinal,(data,pixels) in enumerate(frames):
            if variant=='normal' and ordinal:break
            packet=observe.frame_upload(session,manifest,ordinal,data,pixels)
            if not ordinal:
                usb(packet[:40]);invoke('h264_task',0);assert not calls.count('a733_rail_acquire')
                usb(packet[40:])
            else:
                if ordinal==2:
                    saved=checkpoint()
                    for name,bad in [('wrong-ordinal',observe.header(observe.F,3,session)),('wrong-ACK-digest',observe.header(observe.A,0,session)+bytes(32))]:
                        usb(bad);invoke('h264_task',0);assert get(base+app['result'],4)==2
                        assert query(0)[192:]==snapshots[0][192:] and query(1)[192:]==snapshots[1][192:]
                        branches.append(dict(case=name,retained_outputs=2));restore(saved)
                if ordinal==5:
                    saved=checkpoint();usb(packet);copy_fail_at[0]=5;advance(5)
                    assert get(base+app['result'],4)==2 and get(streambase+streamtype['next'],4)==5
                    assert get(base+app['copy_row'],4)==64 and get(base+app['copy_plane'],4)==0
                    retained=query(4);failed=query(5);assert retained[192:]==snapshots[4][192:] and observe.decode(failed,session,manifest,5,pixels)['status']==2
                    (args.output/'later-failure-frame4.bin').write_bytes(retained);(args.output/'later-failure-frame5.bin').write_bytes(failed)
                    assert state(get(streambase+streamtype['copy_slot'],4))==1
                    branches.append(dict(case='partial-copy-status-fault',retained_ordinal=4,private_copy_rows=64,unpublished=True));restore(saved)
                usb(packet)
            if ordinal==0:
                saved=checkpoint();cpu.mem_write(syms['uncached_pool']+16,struct.pack('<Q',0xad4000));advance(0)
                assert get(base+app['result'],4)==2 and not triggers
                branches.append(dict(case='NC-pool-one-page-short',no_decode_trigger=True));restore(saved)
            if ordinal==0:
                for _ in range(20):
                    invoke('h264_task',0)
                    if get(base+app['stage'],4):break
                assert get(base+app['stage'],4)==1 and not triggers
                arguments=[base,base+app['config'],base+app['job'],990000,OPS+128]
                assert invoke('h264_authority',*arguments)==1
                old=get(rbase+run['frame_bytes']);cpu.mem_write(rbase+run['frame_bytes'],struct.pack('<Q',old+1))
                assert invoke('h264_authority',*arguments)&0xffffffff==0xffffffff
                cpu.mem_write(rbase+run['frame_bytes'],struct.pack('<Q',old))
                cpu.mem_write(rbase+run['picture_index'],struct.pack('<I',1))
                assert invoke('h264_authority',*arguments)&0xffffffff==0xffffffff
                cpu.mem_write(rbase+run['picture_index'],struct.pack('<I',0))
                assert invoke('h264_authority',*arguments)==1 and not triggers
                branches.append(dict(case='native-provider-frame-length-and-ordinal-refusal',no_decode_trigger=True))
            if ordinal==2:
                before=len(triggers)
                for _ in range(5):invoke('h264_task',0)
                assert len(triggers)==before and get(streambase+streamtype['au_state'],4)==1 and state(0)==state(1)==2
                ack(0,snapshots[0]);branches.append(dict(case='copied-output-backpressure',resumed_by_ACK=0))
            advance(ordinal)
            if get(base+app['result'],4)==2:
                (args.output/'failure.json').write_text(json.dumps(dict(ordinal=ordinal,app_fields={k:get(base+v,4) for k,v in app.items()},run_fields={k:get(rbase+v,4) for k,v in run.items()},calls=calls[-40:],mmio=mmio,triggers=len(triggers)),indent=2)+'\n')
                raise AssertionError(('native failure',ordinal))
            frame=query(ordinal);decoded=observe.decode(frame,session,manifest,ordinal,pixels);assert decoded['status']==1
            assert frame[192:]==reference[ordinal*3110400:(ordinal+1)*3110400]
            snapshots.append(frame);(args.output/f'{variant}-frame{ordinal}.bin').write_bytes(frame)
            if variant=='nominal' and ordinal not in (0,1,4,5):ack(ordinal,frame)
            if ordinal==2:ack(1,snapshots[1])
        assert calls.count('acquire')==1 and calls.count('prepare')==1
        assert len(set(tables))==1 # No remap/reset between healthy pictures.
        assert invoke('a733_iommu_validate',base+app['tables'])==1
        spans=base+app['tables']+types['a733_iommu_tables']['spans']
        declared=[struct.unpack('<3QI4x',cpu.mem_read(spans+32*i,32)) for i in range(6)]
        expected=[(0x10000000,4194304,1),(0x10401000,4177920,3),(0x10801000,2088960,3),(0x10a00000,217088,3),(0x10a36000,524288,3),(0x10abc000,49152,3)]
        for i,(va,n,access) in enumerate(expected):assert declared[i]==(va,buffers()[i],n,access)
        (args.output/f'{variant}-tables.bin').write_bytes(cpu.mem_read(get(base+app['table']),32768))
        # Task wakes in this process, one per tick; resumed runs count only the remainder.
        reports.append(dict(case=variant,task_wakes=wakes[0],terminal=get(base+app['result'],4),completed=get(streambase+streamtype['next'],4),triggers=triggers,branches=branches,table_sha256=tables[0],spans=declared,provider_admission='Extended-memory readiness/getters, mapping/power/USB/MMIO/pixels modeled; actual WB/NC allocator, IOMMU tables, parser/controller/copy/serializer execute. Shared startup/GIC/secondaries not executed.'))
    (args.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(),cases=reports),indent=2)+'\n')
    print('PASS full-HD coded references/cropped copy, WB/NC pool bounds, ACK/backpressure and retained partial-copy failure')

if __name__=='__main__':main()
