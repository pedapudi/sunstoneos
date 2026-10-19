#!/usr/bin/env python3
"""Compose bounded VGA streaming, healthy reuse, ACK ownership and retained failure.

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
    parser.add_argument('--authority-only',action='store_true',help='Only affected provider binding; no decoded case replay.')
    args=parser.parse_args();args.output.mkdir(exist_ok=False)
    cases=module('native_cases',ROOT/'checks/h264-application-cases.py')
    syms,segments,types,_,_=cases.setup(args.image,args.output,session_optional=True,fixture_optional=True)
    observe=module('stream_host',ROOT/'apps/h264/stream.py')
    app=types['h264_application'];sup=types['a733_h264_input'];run=types['a733_h264_run'];base=syms['h264'];upload=base+app['supplied'];rbase=base+app['run']
    reference=(args.inputs/'reference.yuv').read_bytes();assert len(reference)==6*460800
    streambase=syms['h264_stream'];streamtype=types['h264_stream'];outputtype=types['h264_stream_output']
    output_size=None
    from elftools.elf.elffile import ELFFile
    with args.image.open('rb') as file:
        elf=ELFFile(file)
        output_size=next(d.attributes['DW_AT_byte_size'].value for cu in elf.get_dwarf_info().iter_CUs() for d in cu.iter_DIEs() if d.tag=='DW_TAG_structure_type' and d.attributes.get('DW_AT_name') and d.attributes['DW_AT_name'].value==b'h264_stream_output' and 'DW_AT_byte_size' in d.attributes)
    reports=[]
    for variant in (('nominal',) if args.authority_only else ('nominal','normal')):
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x50000000,0x1000000)
        for address,data in segments:cpu.mem_write(address,data)
        mode='normal' if variant=='normal' else 'qualification'
        session=(args.inputs/(mode+'-session.bin')).read_bytes();manifest=(args.inputs/mode/'manifest.bin').read_bytes()
        frames=observe.validate_inputs(args.inputs/mode,manifest);wire=observe.manifest_upload(session,manifest)
        oracle=None if mode=='normal' else reference
        cpu.mem_write(base,session);cpu.mem_write(syms['board']+24,session[24:40]);cpu.mem_write(syms['board']+160,struct.pack('<Q',1000000))
        cpu.mem_write(syms['providers']+types['board_provider']['shared']+types['a733_native_usb']['pmic_ready'],struct.pack('<I',1))
        cpu.mem_write(base+app['iommu']+types['a733_iommu']['fault']+types['a733_iommu_fault']['scanned_masters'],struct.pack('<H',0xc0))
        def get(address,n=8):return int.from_bytes(cpu.mem_read(address,n),'little')
        def setfield(address,type_name,field,value,size=8):cpu.mem_write(address+types[type_name][field],value.to_bytes(size,'little'))
        def returned(value):cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
        fifo=bytearray();time=[1000];mmio={0:1,0x1c:0,0xb0:0,0xec:0,0x228:0,0x5c:0,0x804:0,0xf0:0,0xe0:0x33310,0xe4:0x21320}
        active=[False];status_reads=[0];fail_at=[None];scan_fail_at=[None];tx=bytearray();delivered=[0];sram=[0]*1024;sram_offset=[0];triggers=[];tables=[];calls=[];parked=[];guard_reads=[]
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
                    elif off==0x22c:value=1200
                    else:value=mmio.get(off,0)
                    m.mem_write(m.reg_read(REGS[3]),struct.pack('<I',value));returned(1)
                elif kind==4:
                    off=m.reg_read(REGS[2]);value=m.reg_read(REGS[3]);mmio[off]=value
                    if off==0x2e0:sram_offset[0]=value//4
                    if off==0x2e4:sram[sram_offset[0]]=value;sram_offset[0]+=1
                    if off==0x228:mmio[off]=0;active[0]=False
                    if off==0x224 and value==8:
                        p=get(rbase+run['picture_index'],4);slot=p&1;number=observe.descriptors(manifest)[p]['frame_num']
                        assert get(streambase+streamtype['au_state'],4)==2 and get(streambase+streamtype['active'],4)==p
                        b=buffers();assert mmio[0x24c]==slot
                        assert mmio[0xc4]==76800 and mmio[0xc8]==0x01400280
                        assert mmio[0x250]==0x10300000 and mmio[0x254]==0x10500000
                        assert mmio[0x234]==0 and mmio[0x23c]==0x1007ffff
                        assert sram[256+8*slot:256+8*slot+7]==[2*number,2*number,0,0x10100000+slot*307200,0x10200000+slot*155648,0x10400000+slot*40960,0x10400000+slot*40960+19456]
                        assert sram[400]==((slot^1)*2 if number else 0) and sram[401:408]==[0]*7
                        if number:
                            previous=observe.descriptors(manifest)[p-1]['frame_num'];refslot=slot^1
                            assert sram[256+8*refslot:256+8*refslot+7]==[2*previous,2*previous,0,0x10100000+refslot*307200,0x10200000+refslot*155648,0x10400000+refslot*40960,0x10400000+refslot*40960+19456]
                            assert bytes(m.mem_read(b[1]+refslot*307200,307200))==reference[(p-1)*460800:(p-1)*460800+307200]
                        table=get(base+app['table']);tables.append(hashlib.sha256(m.mem_read(table,32768)).hexdigest())
                        triggers.append(dict(picture=p,frame_num=number,registers=dict(mmio),sram=sram.copy()))
                        if fail_at[0]!=p:
                            pixels=reference[p*460800:(p+1)*460800]
                            m.mem_write(b[1]+slot*307200,pixels[:307200]);m.mem_write(b[2]+slot*155648,pixels[307200:])
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
            elif name=='a733_memory_uncached_init':returned(POOL)
            elif name=='a733_video_platform_h264_ops':returned(OPS)
            elif name=='h264_device':returned(int(m.reg_read(REGS[0]) in list(range(0x17000,0x22000,4096))+[0x01c0e000]))
            elif name=='h264_load':
                value=0x10007 if m.reg_read(REGS[0]) in (0x0200258c,0x020025b4) else mmio[0x228] if not active[0] else 0
                m.mem_write(m.reg_read(REGS[1]),struct.pack('<I',value));guard_reads.append(value);returned(1)
            elif name=='a733_iommu_scan':
                returned(-1 if scan_fail_at[0]==get(streambase+streamtype['active'],4) else 1)
            elif name=='a733_iommu_poll':
                setfield(base+app['iommu']+types['a733_iommu']['fault'],'a733_iommu_fault','complete',1,1);returned(1)
        hooks=['board_live','board_fail','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','a733_usb_prepare','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush','a733_counter_clock_now','a733_rail_acquire','a733_rail_poll','a733_pmic_submit','a733_pmic_result','a733_clock_acquire','a733_gpu_resources_acquire','a733_memory_uncached_init','a733_h264_platform_init','a733_video_platform_h264_ops','h264_load','h264_device','a733_iommu_scan','a733_iommu_poll']
        assert all(n in syms for n in hooks),[n for n in hooks if n not in syms]
        cpu.mem_write(OPS,struct.pack('<6Q',*[STUB+16*i for i in range(6)]))
        # Only modeled service boundaries need hooks; CPU parser/hash/copy loops
        # execute unchanged without a Python callback for every instruction.
        boundaries={STOP,syms['vTaskDelay']}|{STUB+16*i for i in range(6)}|{syms[n] for n in hooks}
        for address in boundaries:cpu.hook_add(UC_HOOK_CODE,hook,begin=address,end=address)
        def invoke(name,*values):
            for reg,value in zip(REGS,values):cpu.reg_write(reg,value)
            cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);cpu.reg_write(UC_ARM64_REG_LR,STOP);parked.clear()
            cpu.emu_start(syms[name],0,count=60000000);assert parked,(name,'instruction budget');return cpu.reg_read(REGS[0])
        assert invoke('device_memory_pool_init',POOL,0x50000000,0x50000000,0x200000)==1
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
                if len(tx)>=160:
                    needed=160+int.from_bytes(tx[12:16],'little')
                    if len(tx)==needed:return bytes(tx)
            raise AssertionError('serializer budget')
        def ack(index,frame):usb(observe.header(observe.A,index,session)+frame[96:128])
        def checkpoint():
            return [bytes(cpu.mem_read(0x41000000,0x800000)),bytes(cpu.mem_read(0x50000000,0x1000000)),time[0]]
        def restore(saved):
            cpu.mem_write(0x41000000,saved[0]);cpu.mem_write(0x50000000,saved[1]);time[0]=saved[2]
            fifo.clear();tx.clear();delivered[0]=0;active[0]=False;status_reads[0]=0;fail_at[0]=None;scan_fail_at[0]=None
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
                        assert query(0)[160:]==snapshots[0][160:] and query(1)[160:]==snapshots[1][160:]
                        branches.append(dict(case=name,retained_outputs=2));restore(saved)
                if ordinal==5:
                    saved=checkpoint();before=len(triggers);input_hash=hashlib.sha256(cpu.mem_read(buffers()[0],524288)).hexdigest()
                    usb(packet);scan_fail_at[0]=5;advance(5)
                    assert get(base+app['result'],4)==2 and len(triggers)==before
                    assert hashlib.sha256(cpu.mem_read(buffers()[0],524288)).hexdigest()==input_hash
                    assert query(4)[160:]==snapshots[4][160:]
                    branches.append(dict(case='late-IOMMU-fault-before-refill',retained_ordinal=4,no_next_trigger=True,input_unchanged=True));restore(saved)
                    usb(packet);fail_at[0]=5;advance(5)
                    assert get(base+app['result'],4)==2 and get(streambase+streamtype['next'],4)==5
                    retained=query(4);failed=query(5);assert retained[160:]==snapshots[4][160:] and observe.decode(failed,session,manifest,5,pixels)['status']==2
                    (args.output/'later-failure-frame4.bin').write_bytes(retained);(args.output/'later-failure-frame5.bin').write_bytes(failed)
                    branches.append(dict(case='later-engine-failure',retained_ordinal=4));restore(saved)
                usb(packet)
            if args.authority_only:
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
                (args.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(),scope='Actual native provider authority accepts committed manifest/AU and refuses changed per-frame length/ordinal; zero decode triggers; board/mapping/gate facts modeled.'),indent=2)+'\n')
                print('PASS native manifest/AU provider authority and changed length/ordinal refusal');return
            if ordinal==2:
                before=len(triggers)
                for _ in range(5):invoke('h264_task',0)
                assert len(triggers)==before and get(streambase+streamtype['au_state'],4)==1 and state(0)==state(1)==2
                ack(0,snapshots[0]);branches.append(dict(case='copied-output-backpressure',resumed_by_ACK=0))
            advance(ordinal);assert get(base+app['result'],4)!=2
            frame=query(ordinal);decoded=observe.decode(frame,session,manifest,ordinal,pixels);assert decoded['status']==1
            assert frame[160:]==reference[ordinal*460800:(ordinal+1)*460800]
            snapshots.append(frame);(args.output/f'{variant}-frame{ordinal}.bin').write_bytes(frame)
            if variant=='nominal' and ordinal not in (0,1,4,5):ack(ordinal,frame)
            if ordinal==2:ack(1,snapshots[1])
        assert calls.count('acquire')==1 and calls.count('prepare')==1
        assert len(set(tables))==1 # No remap/reset between healthy pictures.
        assert invoke('a733_iommu_validate',base+app['tables'])==1
        spans=base+app['tables']+types['a733_iommu_tables']['spans']
        declared=[struct.unpack('<3QI4x',cpu.mem_read(spans+32*i,32)) for i in range(6)]
        expected=[(0x10000000,524288,1),(0x10100000,614400,3),(0x10200000,311296,3),(0x10300000,139264,3),(0x10400000,81920,3),(0x10500000,49152,3)]
        for i,(va,n,access) in enumerate(expected):assert declared[i]==(va,buffers()[i],n,access)
        (args.output/f'{variant}-tables.bin').write_bytes(cpu.mem_read(get(base+app['table']),32768))
        reports.append(dict(case=variant,terminal=get(base+app['result'],4),completed=get(streambase+streamtype['next'],4),triggers=triggers,branches=branches,table_sha256=tables[0],spans=declared,provider_admission='modeled; real allocator/IOMMU/parser/controller/copy/USB execute'))
    (args.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(),cases=reports),indent=2)+'\n')
    print('PASS native VGA fixed pool, IDR-boundary reference reuse, copied ACK/backpressure, protocol refusal and retained later failure')

if __name__=='__main__':main()
