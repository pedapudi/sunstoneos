#!/usr/bin/env python3
"""Compose High coded references, visible copied output and retained failure.

Power/admission, MMIO, USB and device pixel writes are explicit models. The
allocator, page-table builder, parser, register controller, guards, copying and
serializer execute from the linked image. No codec arithmetic is emulated.
"""
import argparse
import hashlib
import importlib.util
import json
import gzip,pickle
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
# High capacities of the six guarded spans in apps/h264/native.c.
CAPACITY=(2097152,8355840,4177920,0x35000,1048576,0xc000)

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('image','inputs','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--resume',type=Path)
    # Fixtures named after checks/h264-high-modes.py cases run the nominal path only.
    parser.add_argument('--variants',nargs='+',choices=('fullhd-qualification','hd-qualification','four-qualification','fullhd3-qualification','fullhd-normal',
        'long-term-qualification','mmco5-wrap-qualification','poc-wrap-b-qualification','baseline-profile-qualification','main-profile-b-qualification'))
    args=parser.parse_args();args.output.mkdir(exist_ok=False)
    cases=module('native_cases',ROOT/'checks/h264-application-cases.py')
    syms,segments,types,_,_=cases.setup(args.image,args.output,session_optional=True,fixture_optional=True,high=True)
    observe=module('stream_host',ROOT/'apps/h264/high.py')
    for lo,hi in [('__device_pool_start','__device_pool_end'),('__uncached_pool_start','__uncached_pool_end')]:
        assert 0x7b000000<=syms[lo] or syms[hi]<=0x7a000000, 'Harness scratch/stack overlaps linked pool'
    app=types['h264_application'];sup=types['a733_h264_input'];run=types['a733_h264_run'];base=syms['h264'];upload=base+app['supplied'];rbase=base+app['run']
    streambase=syms['h264_stream'];streamtype=types['h264_stream'];outputtype=types['h264_stream_output'];plan_type=types['h264_high_plan']
    from elftools.elf.elffile import ELFFile
    with args.image.open('rb') as file:
        elf=ELFFile(file);sizes={d.attributes['DW_AT_name'].value.decode():d.attributes['DW_AT_byte_size'].value for cu in elf.get_dwarf_info().iter_CUs() for d in cu.iter_DIEs() if d.tag=='DW_TAG_structure_type' and 'DW_AT_name' in d.attributes and 'DW_AT_byte_size' in d.attributes}
    output_size=sizes['h264_stream_output']
    variants=args.variants or ['fullhd-qualification','hd-qualification','fullhd3-qualification','fullhd-normal']
    fixture_identity={str(p.relative_to(args.inputs)):hashlib.sha256(p.read_bytes()).hexdigest() for geometry in sorted({v.rsplit('-',1)[0] for v in variants}) for p in (args.inputs/geometry).rglob('*') if p.is_file() and (p.suffix in ('.h264','.yu12','.yuv','.bin') or p.name=='syntax-plans.json')}
    resumed=pickle.loads(gzip.decompress(args.resume.read_bytes())) if args.resume else None
    if resumed:
        for item in resumed['artifacts']:
            path=Path(item['path']);data=path.read_bytes();assert hashlib.sha256(data).hexdigest()==item['sha256'];(args.output/path.name).write_bytes(data)
    if resumed:assert resumed['image_sha256']==hashlib.sha256(args.image.read_bytes()).hexdigest() and resumed['fixtures']==fixture_identity,'Resume image/fixture identities differ'
    reports=[] if resumed is None else resumed['reports']
    for variant in variants:
        if resumed and variant!=resumed['variant']:continue
        geometry,mode=variant.rsplit('-',1);directory=args.inputs/geometry
        manifest=(directory/mode/'manifest.bin').read_bytes();limits=observe.profile(manifest);descs=observe.descriptors(manifest);reference=(directory/'reference.yuv').read_bytes();frame_bytes=limits['frame_bytes'];w=limits['coded_width'];h=limits['coded_height'];vw=limits['width'];vh=limits['height'];crop=limits['crop']
        assert len(reference)==len(descs)*frame_bytes
        coded=[]
        for d in descs:
            i=d['ordinal'];pixels=reference[d['presentation']*frame_bytes:(d['presentation']+1)*frame_bytes];y=bytearray([0x61+i])*(w*h);c=bytearray([0x71+i])*(w*h//2)
            for row in range(vh):y[(crop[1]+row)*w+crop[0]:(crop[1]+row)*w+crop[0]+vw]=pixels[row*vw:(row+1)*vw]
            for plane in range(2):
                for row in range(vh//2):
                    dest=plane*w*h//4+(crop[1]//2+row)*(w//2)+crop[0]//2;src=vw*vh+plane*vw*vh//4+row*(vw//2);c[dest:dest+vw//2]=pixels[src:src+vw//2]
            coded.append((bytes(y),bytes(c)))
        cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cpu.mem_map(0x40000000,0x4000000);cpu.mem_map(0x7a000000,0x1000000);cpu.mem_map(0x48000000,0x2000000);cpu.mem_map(0x78000000,0x1000000)
        cpu.mem_map(0x01c0e000,4096);cpu.mem_map(0x02002000,4096)
        def device_read(m,access,address,size,value,unused):
            datum=0x10007 if address in (0x0200258c,0x020025b4) else mmio[0x228]
            if address==0x01c0e228 and copy_fail_at[0]==get(streambase+streamtype['active'],4) and get(base+app['copy_step'],4)==3 and get(base+app['copy_row'],4)>=64:datum=2
            if address==0x01c0e228:corrupt()
            m.mem_write(address,struct.pack('<I',datum));guard_reads.append(datum)
        cpu.hook_add(UC_HOOK_MEM_READ,device_read,begin=0x01c0e228,end=0x01c0e22b)
        for address,data in segments:
            if data:cpu.mem_write(address,data)
        session=(directory/(mode+'-session.bin')).read_bytes();frames=observe.validate_inputs(directory/mode,manifest);wire=observe.manifest_upload(session,manifest)
        oracle=None if mode=='normal' else reference
        cpu.mem_write(base,session);cpu.mem_write(syms['board']+24,session[24:40]);cpu.mem_write(syms['board']+160,struct.pack('<Q',1000000))
        cpu.mem_write(syms['providers']+types['board_provider']['shared']+types['a733_native_usb']['pmic_ready'],struct.pack('<I',1))
        cpu.mem_write(base+app['iommu']+types['a733_iommu']['fault']+types['a733_iommu_fault']['scanned_masters'],struct.pack('<H',0xc0))
        def get(address,n=8):return int.from_bytes(cpu.mem_read(address,n),'little')
        def corrupt():
            # One byte of device memory changes after the guard scan of the copy chunk that starts at row 64.
            if corrupt_at[0] and get(base+app['copy_step'],4)==3 and get(base+app['copy_row'],4)>=64:cpu.mem_write(corrupt_at[0],b'\x5a');corrupt_at[0]=None
        def setfield(address,type_name,field,value,size=8):cpu.mem_write(address+types[type_name][field],value.to_bytes(size,'little'))
        def returned(value):cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
        planbase=streambase+streamtype['plan'];slots_live={}
        def pf(name):return get(planbase+plan_type[name],4)
        fifo=bytearray();time=[1000];mmio={0:1,0x1c:0,0xb0:0,0xec:0,0x228:0,0x5c:0,0x804:0,0xf0:0,0xe0:0x33310,0xe4:0x21320}
        active=[False];status_reads=[0];fail_at=[None];scan_fail_at=[None];copy_fail_at=[None];corrupt_at=[None];tx=bytearray();delivered=[0];sram=[0]*1024;sram_offset=[0];triggers=[];tables=[];calls=[];parked=[];guard_reads=[];wakes=[0]
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
                    elif off==0x22c:value=w*h//256
                    else:value=mmio.get(off,0)
                    m.mem_write(m.reg_read(REGS[3]),struct.pack('<I',value));returned(1)
                elif kind==4:
                    off=m.reg_read(REGS[2]);value=m.reg_read(REGS[3]);mmio[off]=value
                    if off==0x2e0:sram_offset[0]=value//4
                    if off==0x2e4:sram[sram_offset[0]]=value;sram_offset[0]+=1
                    if off==0x228:mmio[off]=0;active[0]=False
                    if off==0x224 and value==8:
                        p=get(rbase+run['picture_index'],4);slot=pf('slot');number=descs[p]['frame_num'];b=buffers()
                        assert get(streambase+streamtype['au_state'],4)==2 and get(streambase+streamtype['active'],4)==p
                        assert mmio[0x24c]==slot and mmio[0xc4]==w*h//4 and mmio[0xc8]==((w//2)<<16)|w
                        assert mmio[0x250]==0x10df7000 and mmio[0x254]==0x10f34000 and mmio[0x234]==0 and mmio[0x23c]==0x101fffff
                        for field,off in [('sps',0x200),('pps',0x204),('shs',0x208),('shs2',0x20c),('qp',0x21c)]:assert mmio[off]==get(streambase+streamtype['picture']+types['a733_h264_picture'][field],4)
                        for k in range(5):
                            if not get(planbase+plan_type['valid']+4*k,4):continue
                            top,bottom=struct.unpack('<2I',m.mem_read(planbase+plan_type['pocs']+8*k,8));expected=[top,bottom,0x22 if k==slot and not pf('reference') else 0,0x10201000+k*pf('y_step'),0x109fa000+k*pf('c_step'),0x10e30000+k*pf('mv_step'),0x10e30000+k*pf('mv_step')+pf('mv_field')]
                            assert sram[256+8*k:256+8*k+7]==expected
                        for l in range(2):
                            count=get(planbase+plan_type['ref_count']+4*l,4);refs=struct.unpack('<8I',m.mem_read(planbase+plan_type['lists']+32*l,32));expected=bytes([2*refs[j] if j<count else 0 for j in range(32)])
                            assert struct.pack('<8I',*sram[400+9*l:408+9*l])==expected
                            for k in refs[:count]:
                                assert k!=slot and k in slots_live;prior=slots_live[k]
                                assert bytes(m.mem_read(b[1]+k*pf('y_step'),w*h))==coded[prior][0] and bytes(m.mem_read(b[2]+k*pf('c_step'),w*h//2))==coded[prior][1]
                        assert sram[:192]==list(struct.unpack('<192I',m.mem_read(planbase+plan_type['weights'],768)))
                        assert sram[512:568]==list(struct.unpack('<56I',m.mem_read(planbase+plan_type['scaling'],224)))
                        table=get(base+app['table']);tables.append(hashlib.sha256(m.mem_read(table,32768)).hexdigest())
                        triggers.append(dict(picture=p,frame_num=number,registers=dict(mmio),sram=sram.copy()))
                        if fail_at[0]!=p:
                            m.mem_write(b[1]+slot*pf('y_step'),coded[p][0]);m.mem_write(b[2]+slot*pf('c_step'),coded[p][1]);slots_live[slot]=p
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
            elif name=='a733_memory_uncached_init':returned(syms['uncached_pool'])
            elif name=='a733_memory_reservations_init':returned(syms['boot_pool'])
            elif name=='a733_video_platform_h264_ops':returned(OPS)
            elif name=='aarch64_device_translate':
                m.mem_write(m.reg_read(REGS[2]),struct.pack('<QII',m.reg_read(REGS[0]),0,2));returned(1)
            elif name=='h264_device':returned(int(m.reg_read(REGS[0]) in list(range(0x17000,0x22000,4096))+[0x01c0e000]))
            elif name=='h264_load':
                value=0x10007 if m.reg_read(REGS[0]) in (0x0200258c,0x020025b4) else mmio[0x228] if not active[0] else 0
                if copy_fail_at[0]==get(streambase+streamtype['active'],4) and get(base+app['copy_step'],4)==3 and get(base+app['copy_row'],4)>=64:value=2
                corrupt()
                m.mem_write(m.reg_read(REGS[1]),struct.pack('<I',value));guard_reads.append(value);returned(1)
            elif name=='a733_iommu_scan':
                returned(-1 if scan_fail_at[0]==get(streambase+streamtype['active'],4) else 1)
            elif name=='a733_iommu_poll':
                setfield(base+app['iommu']+types['a733_iommu']['fault'],'a733_iommu_fault','scanned_masters',0xc0,2);setfield(base+app['iommu']+types['a733_iommu']['fault'],'a733_iommu_fault','complete',1,1);returned(1)
        hooks=['board_live','board_fail','a733_native_usb_step','a733_native_usb_health','tusb_rhport_init','tud_task_ext','a733_usb_prepare','tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush','a733_counter_clock_now','a733_rail_acquire','a733_rail_poll','a733_pmic_submit','a733_pmic_result','a733_clock_acquire','a733_gpu_resources_acquire','a733_memory_uncached_init','a733_memory_reservations_init','a733_h264_platform_init','a733_video_platform_h264_ops','h264_load','h264_device','a733_iommu_scan','a733_iommu_poll','aarch64_device_translate']
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
        def usb(packet=b''):
            fifo.extend(packet)
            while fifo:invoke('board_usb0_task',0)
        def oslot(i):return streambase+streamtype['output']+output_size*i
        def state(i):return get(oslot(i)+outputtype['state'],4)
        def advance(ordinal):
            for _ in range(8000):
                invoke('h264_task',0)
                if get(streambase+streamtype['next'],4)>ordinal or get(base+app['result'],4)==2:return
            raise AssertionError(('no completion',ordinal,get(rbase+run['state'],4)))
        def query(index):
            tx.clear();usb(observe.header(observe.Q,index,session))
            # Actual serializer separates header/payload and pins the copied slot.
            for _ in range(10000):
                invoke('board_usb0_task',0)
                if delivered[0]:n=delivered[0];delivered[0]=0;invoke('tud_vendor_tx_cb',0,n)
                if len(tx)>=256:
                    needed=256+int.from_bytes(tx[12:16],'little')
                    if len(tx)==needed:return bytes(tx)
            raise AssertionError('serializer budget')
        def ack(index,frame):usb(observe.header(observe.A,index,session)+frame[96:128])
        def checkpoint():
            return [bytes(cpu.mem_read(0x41000000,0x800000)),bytes(cpu.mem_read(0x7a000000,0x1000000)),bytes(cpu.mem_read(0x48000000,0x2000000)),bytes(cpu.mem_read(0x78000000,0x1000000)),time[0],dict(slots_live)]
        def restore(saved):
            cpu.mem_write(0x41000000,saved[0]);cpu.mem_write(0x7a000000,saved[1]);cpu.mem_write(0x48000000,saved[2]);cpu.mem_write(0x78000000,saved[3]);time[0]=saved[4];slots_live.clear();slots_live.update(saved[5])
            fifo.clear();tx.clear();delivered[0]=0;active[0]=False;status_reads[0]=0;fail_at[0]=None;scan_fail_at[0]=None;copy_fail_at[0]=None;corrupt_at[0]=None
        if resumed:
            restore(resumed['memory']);mmio.clear();mmio.update(resumed['mmio']);sram[:]=resumed['sram'];calls[:]=resumed['calls'];triggers[:]=resumed['triggers'];tables[:]=resumed['tables'];snapshots=resumed['snapshots'];branches=resumed['branches'];backpressure=[resumed['backpressure']]
            for present,raw in snapshots.items():(args.output/f'{variant}-frame{present}.bin').write_bytes(raw)
        else:
            assert invoke('device_memory_pool_init',syms['uncached_pool'],0x78000000,0x78000000,0x1000000)==1
            assert invoke('device_memory_pool_init',syms['boot_pool'],0x48000000,0x48000000,0x30000000)==1
            assert invoke('h264_stream_storage_init',12)==1 and invoke('h264_stream_storage_valid')==1
            usb(wire[:24]);invoke('h264_task',0);assert not get(base+app['stage'],4) and not calls.count('a733_rail_acquire')
            usb(wire[24:]);assert get(streambase+streamtype['manifest_ready'],4)==1
            snapshots={};branches=[];backpressure=[False]
        def capture_published():
            for i in range(5):
                if state(i)!=2:continue
                present=get(oslot(i)+outputtype['ordinal'],4)
                if present in snapshots:continue
                raw=query(present);d=next(d for d in descs if d['presentation']==present);pixels=frames[d['ordinal']][1]
                value=observe.decode(raw,session,manifest,present,pixels);assert value['status']==1 and not value['reorder_pending']
                assert raw[256:]==reference[present*frame_bytes:(present+1)*frame_bytes]
                snapshots[present]=raw;(args.output/f'{variant}-frame{present}.bin').write_bytes(raw)
        limit=3 if mode=='normal' else len(frames)
        for ordinal,(data,pixels) in enumerate(frames[:limit]):
            if resumed and ordinal<resumed['ordinal']:continue
            packet=observe.frame_upload(session,manifest,ordinal,data,pixels)
            if not resumed:
                if ordinal==0:
                    usb(packet[:40]);invoke('h264_task',0);assert not calls.count('a733_rail_acquire');usb(packet[40:])
                else:
                    if ordinal==2 and variant=='fullhd-qualification':
                        saved=checkpoint();committed=get(streambase+streamtype['high'])+types['h264_high']['committed'];before=bytes(cpu.mem_read(committed,sizes['dpb']))
                        usb(packet);copy_fail_at[0]=2;advance(2)
                        assert get(base+app['result'],4)==2 and get(streambase+streamtype['next'],4)==2 and get(base+app['copy_row'],4)==64
                        assert bytes(cpu.mem_read(committed,sizes['dpb']))==before
                        for previous in (0,1):
                            present=descs[previous]['presentation'];expected_pending=next(state(i)==3 for i in range(5) if get(oslot(i)+outputtype['ordinal'],4)==present and state(i) in (2,3));raw=query(present);value=observe.decode(raw,session,manifest,present,frames[previous][1]);assert value['status']==1 and value['reorder_pending']==expected_pending and raw[256:]==reference[present*frame_bytes:(present+1)*frame_bytes]
                            (args.output/f'retained-present{present}.bin').write_bytes(raw)
                        failed=query(descs[2]['presentation']);assert observe.decode(failed,session,manifest,descs[2]['presentation'],pixels)['status']==2
                        (args.output/'failed-B.bin').write_bytes(failed);branches.append(dict(case='failed-B-copy-after64rows',prior_completed_results=2,reorder_private_results=1,committed_DPB_unchanged=True));restore(saved)
                    elif ordinal==2 and variant=='hd-qualification':
                        # A guard-page byte is caught by the next per-call scan; a byte
                        # in the unused capacity beyond the admitted reference slots
                        # is caught only by the full scan after the last copied row.
                        tail=pf('slots')*pf('y_step');assert tail+4097<CAPACITY[1],'HD geometry leaves no unused Y capacity'
                        lease_size=sizes['device_memory_lease'];lease=types['device_memory_lease'];allocation=types['device_memory_allocation']
                        for case,a,offset,row,plane,step in [('guard-page-corrupted-mid-copy',4,CAPACITY[4]+123,96,0,3),('unused-capacity-corrupted-mid-copy',1,tail+4097,0,3,2)]:
                            saved=checkpoint();address=buffers()[a]+offset;assert cpu.mem_read(address,1)==b'\xa5'
                            usb(packet);corrupt_at[0]=address;advance(2)
                            assert corrupt_at[0] is None and cpu.mem_read(address,1)==b'\x5a',(case,corrupt_at[0],get(base+app['result'],4),get(base+app['copy_row'],4),get(base+app['copy_plane'],4),get(base+app['copy_step'],4),get(streambase+streamtype['next'],4))
                            assert get(base+app['result'],4)==2 and get(streambase+streamtype['next'],4)==2
                            assert (get(base+app['copy_row'],4),get(base+app['copy_plane'],4),get(base+app['copy_step'],4))==(row,plane,step),case
                            pool=get(base+app['pool'])
                            for i in range(7):
                                l=base+app['leases']+i*lease_size;slot=get(l+lease['slot'],4)
                                assert get(pool+types['device_memory_pool']['allocations']+slot*sizes['device_memory_allocation']+allocation['state'],4)==3,(case,i)
                            failed=query(descs[2]['presentation']);assert observe.decode(failed,session,manifest,descs[2]['presentation'],pixels)['status']==2
                            branches.append(dict(case=case,buffer=a,offset=offset,copy_row=row,copy_plane=plane,copy_step=step,allocations_retained=7));restore(saved)
                    usb(packet)
                if ordinal==0:
                    for _ in range(20):
                        invoke('h264_task',0)
                        if get(base+app['stage'],4):break
                    assert get(base+app['stage'],4)==1 and not triggers
                    arguments=[base,base+app['config'],base+app['job'],990000,OPS+128]
                    assert invoke('h264_authority',*arguments)==1
                    old=get(rbase+run['frame_bytes']);cpu.mem_write(rbase+run['frame_bytes'],struct.pack('<Q',old+1));assert invoke('h264_authority',*arguments)&0xffffffff==0xffffffff;cpu.mem_write(rbase+run['frame_bytes'],struct.pack('<Q',old))
                    branches.append(dict(case='native-provider-length-refusal',no_decode_trigger=True))
            record=dict(variant=variant,ordinal=ordinal,memory=checkpoint(),mmio=dict(mmio),sram=sram.copy(),calls=calls.copy(),triggers=triggers.copy(),tables=tables.copy(),snapshots=dict(snapshots),branches=branches.copy(),backpressure=backpressure[0],artifacts=[dict(path=str(path.resolve()),sha256=hashlib.sha256(path.read_bytes()).hexdigest()) for path in args.output.glob('*.bin')],reports=reports.copy(),fixtures=fixture_identity,image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest())
            (args.output/f'{variant}-resume.pkl.gz').write_bytes(gzip.compress(pickle.dumps(record,protocol=4),compresslevel=1))
            resumed=None
            for attempt in range(8000):
                invoke('h264_task',0)
                if get(base+app['result'],4)==2:raise AssertionError(('native failure',variant,ordinal,get(rbase+run['state'],4)))
                if get(streambase+streamtype['next'],4)>ordinal:break
                if get(streambase+streamtype['au_state'],4)==1 and all(state(i) in (2,3) for i in range(5)):
                    before=len(triggers)
                    for _ in range(5):invoke('h264_task',0)
                    assert len(triggers)==before;capture_published();published=sorted(get(oslot(i)+outputtype['ordinal'],4) for i in range(5) if state(i)==2);assert published
                    ack(published[0],snapshots[published[0]]);backpressure[0]=True
            else:raise AssertionError(('no completion',ordinal))
            invoke('h264_task',0);capture_published()
            assert not get(get(streambase+streamtype['high'])+types['h264_high']['pending_valid'],4)
            if ordinal==3 and variant=='fullhd-qualification':
                saved=checkpoint()
                for case,bad in [('wrong-ACK-digest',observe.header(observe.A,0,session)+bytes(32)),('wrong-decode-ordinal',observe.header(observe.F,ordinal+2,session))]:
                    usb(bad);invoke('h264_task',0);assert get(base+app['result'],4)==2
                    assert observe.decode(query(0),session,manifest,0,frames[0][1])['status']==1
                    branches.append(dict(case=case,prior_output_retained=True));restore(saved)
        if mode!='normal':
            for _ in range(20):
                invoke('h264_task',0);capture_published();published=sorted(get(oslot(i)+outputtype['ordinal'],4) for i in range(5) if state(i)==2)
                for present in published:ack(present,snapshots[present])
                if len(snapshots)==len(frames):break
            assert len(snapshots)==len(frames) and get(base+app['result'],4)==1 and backpressure[0]
        else:assert 0 in snapshots and not observe.decode(snapshots[0],session,manifest,0,None)['oracle_verified']
        assert calls.count('acquire')==1 and calls.count('prepare')==1 and len(set(tables))==1 and invoke('a733_iommu_validate',base+app['tables'])==1
        spans=base+app['tables']+types['a733_iommu_tables']['spans'];declared=[struct.unpack('<3QI4x',cpu.mem_read(spans+32*i,32)) for i in range(6)]
        expected=[(0x10000000,2097152,1),(0x10201000,8355840,3),(0x109fa000,4177920,3),(0x10df7000,217088,3),(0x10e30000,1048576,3),(0x10f34000,49152,3)]
        for i,(va,n,access) in enumerate(expected):assert declared[i]==(va,buffers()[i],n,access)
        # Task wakes in this process, one per tick; resumed runs count only the remainder.
        reports.append(dict(case=variant,task_wakes=wakes[0],completed=get(streambase+streamtype['next'],4),copied_presentation_ordinals=sorted(snapshots),backpressure=backpressure[0],branches=branches,triggers=triggers,table_sha256=tables[0],provider_admission='Actual native parser/DPB, allocation/table, controller, copy/hash/ordering/serializer. Power, readiness/device translation, MMIO/pixels, fault scans and USB modeled; no startup/physical execution.'))
    (args.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(),cases=reports),indent=2)+'\n')
    print('PASS native High CABAC/B/multiple-reference ordering, guarded copied output, backpressure and retained failed B')

if __name__=='__main__':main()
