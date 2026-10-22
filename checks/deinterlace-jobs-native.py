#!/usr/bin/env python3
"""Execute repeated DI301 jobs in the linked native task with a synthetic device.

The device model checks each job's submitted registers and reads its inputs
through the actual IOMMU table: both frames must be the job's uploaded frames
and the input flags must be the previous job's output flags. It writes
synthetic output and flags, keeps busy set for several status reads, and
returns per-job statistics. The TNR strength of every later job must equal
the gain recomputed here from the previous job's statistics. A memory hook
refuses any CPU write to a DMA input or the output while a job is in flight.
Failure variants cover statistics-read refusal, refused statistics, missing
or busy finish, guard and input corruption between jobs and an IOMMU fault.
The model supplies completion, bytes and statistics; it does not implement
DI arithmetic. Prepared authority, PMIC services and the scheduler are
modeled, and no physical device is accessed.
"""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import UC_HOOK_MEM_WRITE
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
motion=module('jobs_motion_native',ROOT/'checks/deinterlace-motion-native.py')
jobs_check=module('jobs_plan_check',ROOT/'checks/deinterlace-jobs.py')
prep=module('jobs_native_preparation',ROOT/'apps/deinterlace/prepare.py')
capture=module('jobs_native_capture',ROOT/'apps/deinterlace/capture.py')
producer=module('jobs_native_producer',ROOT/'apps/deinterlace/producer.py')
base=motion.base
SCRATCH=base.SCRATCH
DI=0x05400000
FRAME=460800
STATISTICS=[0x300c,0x3010,0x3014,0x3018,0x301c,0x3020,0x3024,0x3028,0x3048,0x304c,0x3054,0x3058,0x3064,0x3068,0x306c,
    0x402c,0x4030,0x4034,0x4038,0x403c,0x4040,0x4044,0x4048,0x404c]
BOUNDS=jobs_check.BOUNDS


def output_pattern(job):
    return bytes((i*13+7+job*29)&255 for i in range(FRAME))


class Machine(motion.Machine):
    """Sequence-aware device model over the shared native harness."""
    def setup(self,session,frames,statistics):
        self.mode,self.strength,self.repeats=struct.unpack_from('<I',session,876)[0],*struct.unpack_from('<2H',session,884)
        self.frames=frames;self.statistics=statistics;self.in_flight=False;self.busy_reads=0;self.watched=False
        self.stat_reads=[];self.cpu_writes=[];self.gains=[];self.flag_marks={}
    def rows(self):
        result={};pf=self.im.layouts['device_memory_pool'];af=self.im.layouts['device_memory_allocation'];size=self.im.sizes['device_memory_allocation']
        for name,count in [('boot_pool',2),('uncached_pool',6 if self.mode!=1 else 3)]:
            at=self.im.address(name);back=at+pf['backing'];rows=[]
            for i in range(count):
                p=at+pf['allocations']+i*size;first=self.get(p+af['first'],4);pages=self.get(p+af['pages'],4)
                if pages:rows.append(dict(slot=i,cpu=self.get(back)+first*4096,physical=self.get(back+8)+first*4096,bytes=pages*4096,state=self.get(p+af['state'],4)))
            result[name]=rows
        return result
    def field(self,name,n=8):return self.get(self.base+self.fields[name],n)
    def watch(self):
        """Refuse CPU writes to DMA inputs and the output during a job."""
        if self.watched:return
        self.watched=True
        spans=[(self.field('input'),FRAME),(self.field('output'),FRAME)]
        if self.mode!=1:spans.append((self.field('current'),FRAME))
        def write(u,access,at,size,value,unused):
            self.cpu_writes.append((self.started,self.in_flight))
            assert not self.in_flight,('CPU wrote a DMA buffer during job',self.started,hex(at))
        for at,n in spans:self.u.hook_add(UC_HOOK_MEM_WRITE,write,begin=at,end=at+n-1)
    def expected_gain(self,job):
        if self.mode!=3:return None
        initial=jobs_check.INITIAL[self.strength]
        if not job or not self.strength:return initial
        total,_=jobs_check.gain(self.gains[job-1],self.statistics[job-1])
        low,high=BOUNDS[self.strength];return min(high,max(low,total//32))
    def effect(self):
        job=self.started-1;r=lambda off:self.regs.get(DI+off,0)
        assert job<=self.repeats,'Start after the last job'
        if self.mode==1:
            assert r(0xb0)==0x10000000
            assert bytes(self.u.mem_read(self.mapped(0x10000000,FRAME,1),FRAME))==self.frames[job],('WEAVE input',job)
            base.Machine.weave_effect(self);return
        previous,current=(0x10000000,0x10800000) if not job&1 else (0x10800000,0x10000000)
        out_flag=0x10c50000 if self.mode==3 else 0x10c20000
        flag_in,flag_out=(0x10c00000,out_flag) if not job&1 else (out_flag,0x10c00000)
        assert r(0xb0)==previous and r(0xd0)==current and r(0x120)==flag_in and r(0x124)==flag_out,('addresses',job)
        assert r(0x24)==(0x91001011 if self.mode==3 else 0x90101011) and r(0x38)==0
        for iova,frame in ((previous,job),(current,job+1)):
            assert bytes(self.u.mem_read(self.mapped(iova,FRAME,1),FRAME))==self.frames[frame],('input frame',job,frame)
        md=307200 if self.mode==3 else 76800
        flags=bytes(self.u.mem_read(self.mapped(flag_in,md,1),md))
        assert flags==(bytes(md) if not job else bytes([0x30+job-1])*md),('flag history',job)
        self.u.mem_write(self.mapped(flag_out,md,2),bytes([0x30+job])*md)
        if self.mode==3:
            word=r(0x4000);gain=self.expected_gain(job);assert word==gain*0x010101,('gain',job,hex(word),gain)
            self.gains.append(gain);dest=r(0xf0)
        else:dest=r(0x110)
        self.u.mem_write(self.mapped(dest,FRAME,2),output_pattern(job))
        if self.variant=='input-mutated' and job==1:self.put(self.mapped(current,1,1),self.frames[job+1][0]^1,1)
        self.events.append(('synthetic-job',job))
    def read_mmio(self,u,access,at,size,value,unused):
        if 0x03900000<=at<0x03920000 and self.variant=='iommu-fault' and self.started==3 and at-0x03900000==0x10088:
            self.regs[at]=1
        if at==DI+0x18:
            if self.in_flight:
                self.busy_reads-=1
                stuck=(self.variant=='nonfinish' and self.started==3)
                if self.busy_reads<=0 and not stuck:
                    self.status=0x101 if self.variant=='busy-finish' and self.started==2 else 1;self.in_flight=False
                    # A guard byte changes after the first job finished.
                    if self.variant=='guard' and self.started==1:self.put(self.field('output')-1,0,1)
            self.put(at,self.status);return
        # Between a finish and its acknowledgment the counters hold that job's
        # statistics; register publication reads the TNR accumulators too.
        if at-DI in STATISTICS and self.started and not self.in_flight and self.status&1:
            job=self.started-1;self.stat_reads.append((job,at-DI))
            self.put(at,self.statistics[job].get(at-DI,0) if job<len(self.statistics) else 0);return
        return base.Machine.read_mmio(self,u,access,at,size,value,unused)
    def hook(self,u,pc,size,unused):
        if self.hooks.get(pc)=='aarch64_device_translate' and self.variant=='statistics-translation':
            # Only statistics() touches the FMD counters.
            if DI+0x3000<=self.args()[0]<DI+0x3070:self.ret(0);return
        return base.Machine.hook(self,u,pc,size,unused)
    def write_mmio(self,u,access,at,size,value,unused):
        assert size==4,('Unexpected MMIO width',hex(at),size)
        self.events.append(('write',at,value))
        assert not (DI+0x3000<=at<DI+0x4000) and at-DI not in STATISTICS[:15],'Statistics are read only'
        if at==DI+0x18:
            assert value==1 and not self.in_flight;self.status&=~1
        elif at==DI+0x10:
            assert value==1 and not self.status&0x101 and not self.regs.get(DI+0x14,0)&1 and not self.in_flight
            self.watch();self.started+=1;self.effect()
            self.status=0x100;self.in_flight=True;self.busy_reads=3
        else:self.regs[at]=value
    def run(self):
        for _ in range(4000):
            self.tick()
            if self.variant=='nonfinish' and self.started==3:self.now+=10000
            if self.result():return
        raise AssertionError('Native bounded task made no terminal progress')


def statistics_for(kind,repeats):
    rows=[];state=0x1234567
    for job in range(repeats):
        row={}
        for offset in STATISTICS:
            state=(state*1664525+1013904223)&0xffffffff
            row[offset]=0 if kind=='zero' or (kind=='fall-rise' and not job) else state&(0x07ffffff if offset<0x4000 else 0xffffffff)
        rows.append(row)
    if kind=='divisor':rows[0].update({0x4038:1,0x404c:8192,0x4044:1})
    if kind=='field-order':rows[0][0x301c]=(2**31-1)//11+1
    return rows


def session_for(template,mode,strength,repeats):
    frames=producer.field_pattern(mode,repeats);raw=bytearray(template)
    struct.pack_into('<I',raw,876,mode);struct.pack_into('<2H',raw,884,strength,repeats)
    raw[768:800]=hashlib.sha256(frames).digest();prep.session_check(bytes(raw))
    return bytes(raw),[frames[i*FRAME:(i+1)*FRAME] for i in range(len(frames)//FRAME)],frames


def fresh(image,session,frames,statistics,variant='nominal'):
    m=Machine(image,session);m.setup(session,frames,statistics);m.variant=variant
    m.u.mem_write(SCRATCH,session);assert m.call('a733_deinterlace_session_valid',SCRATCH)==1
    m.pools()
    supplied=m.base+m.fields['supplied'];epoch,generation=struct.unpack_from('<2Q',session,24)
    packet=struct.pack('<4I2Q',0x55494453,1,len(b''.join(frames)),0,epoch,generation)+b''.join(frames)
    at=0;result=0
    while at<len(packet):
        n=min(4096,m.call('deinterlace_input_remaining',supplied),len(packet)-at);assert n
        m.u.mem_write(SCRATCH,packet[at:at+n])
        result=m.call('deinterlace_input_feed',supplied,m.base,m.im.address('a733_counter_clock_now'),0,990000,SCRATCH,n);at+=n
    assert result==1
    m.configure();return m


def allocation(m,session):
    f=m.fields;b=m.base;table=m.get(b+f['table']);t=b+f['tables'];tf=m.im.layouts['a733_iommu_tables'];sf=m.im.layouts['a733_iommu_span'];size=m.im.sizes['a733_iommu_span']
    count=m.get(t+tf['count'],4);assert m.call('a733_iommu_validate',t)==1
    spans=[{key:m.get(t+tf['spans']+i*size+sf[key],4 if key=='access' else 8) for key in ['iova','physical','bytes','access']} for i in range(count)]
    md=307200 if m.mode==3 else 76800;input_bytes=FRAME*(m.repeats+(1 if m.mode==1 else 2))
    proof=dict(physical_authority=False,modeled=True,session=hashlib.sha256(session).hexdigest(),rounded=462848,
        input_bytes=input_bytes,input_rounded=(input_bytes+4095)&~4095,md_bytes=md,md_rounded=(md+4095)&~4095,table_bytes=32768,
        allocations=m.rows(),spans=spans,table_physical=table,source_physical=m.get(b+f['input']),destination_physical=m.get(b+f['output']),
        table_sha256=hashlib.sha256(bytes(m.u.mem_read(table,32768))).hexdigest())
    if m.mode!=1:proof.update(current_physical=m.get(b+f['current']),flag_input_physical=m.get(b+f['flag_input']),
        flag_output_physical=m.get(b+f['flag_output']))
    return proof,bytes(m.u.mem_read(table,32768))


def serializer(m):
    """Run the native USB serializer; TinyUSB FIFO and completions are modeled."""
    functions={}
    for symbol in ('tud_vendor_n_available','tud_vendor_n_read','tud_vendor_n_write','tud_vendor_n_write_flush','tud_speed_get','tud_task_ext','tusb_rhport_init','a733_native_usb_step','a733_native_usb_health'):
        if symbol in m.im.symbols:functions[m.im.address(symbol)]=symbol
    incoming=bytearray();wire=bytearray();pending=[]
    def hook(u,pc,size,unused):
        a=m.args();symbol=functions[pc]
        if symbol in ('tud_speed_get','tusb_rhport_init','a733_native_usb_step','a733_native_usb_health'):m.ret(1)
        elif symbol=='tud_vendor_n_available':m.ret(len(incoming))
        elif symbol=='tud_vendor_n_read':
            n=min(a[2],len(incoming));u.mem_write(a[1],bytes(incoming[:n]));del incoming[:n];m.ret(n)
        elif symbol=='tud_vendor_n_write':
            n=a[2];wire.extend(u.mem_read(a[1],n));pending.append(n);m.ret(n)
        else:m.ret(0)
    for pc in functions:m.u.hook_add(base.UC_HOOK_CODE,hook,begin=pc,end=pc)
    def exchange(data):
        incoming.extend(data);start=len(wire)
        m.call('board_usb0_task',0);m.call('board_usb0_task',0)
        while pending:
            n=pending.pop(0);m.call('tud_vendor_tx_cb',0,n);m.call('board_usb0_task',0)
        if len(wire)>start and (len(wire)-start)%64==0:m.call('tud_vendor_tx_cb',0,0)
        return bytes(wire[start:])
    return exchange


def replies(m,out,session,name):
    """Query the full sequence response, acknowledge it and query the receipt."""
    exchange=serializer(m)
    response=exchange(b'\0');(out/(name+'-response.bin')).write_bytes(response)
    assert not exchange(b'\1'+session[32:40]+response[96:128])
    receipt=exchange(b'\0');(out/(name+'-receipt.bin')).write_bytes(receipt)
    return response,receipt


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True,help='The revision5 deinterlace ELF')
    parser.add_argument('--template',type=Path,required=True,help='An earlier prepared session.bin; read only')
    parser.add_argument('--output',type=Path,required=True)
    a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    im=base.helpers.Image(a.image);template=a.template.read_bytes();assert len(template)==888
    report=dict(image_sha256=base.digest(a.image),template_sha256=hashlib.sha256(template).hexdigest(),cases={},physical_access=False,scope=__doc__)
    def passed(name,**values):
        report['cases'][name]=dict(passed=True,**values);(a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS',name,flush=True)
    def nominal(name,mode,strength,repeats,kind,checkpoint=False):
        session,frames,upload_bytes=session_for(template,mode,strength,repeats);stats=statistics_for(kind,repeats)
        m=fresh(im,session,frames,stats);m.image_hash=report['image_sha256']
        m.run();assert m.result()==1,m.events[-12:]
        jobs=repeats+1;assert m.started==jobs and m.field('job',4)==repeats and m.field('copied')==FRAME
        assert sorted(set(m.stat_reads))==[(job,offset) for job in range(repeats) for offset in STATISTICS] and len(m.stat_reads)==repeats*len(STATISTICS)
        payload=bytes(m.u.mem_read(m.field('payload'),FRAME*jobs))
        if mode==1:assert payload==upload_bytes
        else:assert payload==b''.join(output_pattern(job) for job in range(jobs))
        assert all(row['state']==2 for row in m.rows()['uncached_pool'])
        assert any(not flight for _,flight in m.cpu_writes) or not repeats
        proof,table=allocation(m,session);prep.allocation_check(proof,session,table)
        # The TinyUSB check resumes from this copied state.
        if checkpoint:m.checkpoint(a.output/(name+'-copied.json'))
        response,receipt=replies(m,a.output,session,name)
        decoded=capture.decode(response,session);assert decoded['status']==1 and decoded['bytes']==FRAME*jobs and response[192:]==payload
        if mode==1:assert capture.expected_output(upload_bytes,session)==payload
        if repeats:assert decoded['finished_jobs']==jobs
        if mode==3 and repeats:assert list(response[180:180+jobs])==m.gains,(list(response[180:184]),m.gains)
        else:assert not any(response[180:192])
        if not repeats:assert not any(response[176:192]) and len(response)==192+FRAME
        terminal=capture.receipt_check(receipt,session,response[:192]);assert terminal['status']==3
        # Host capture journals the whole sequence before acknowledging it.
        folder=a.output/(name+'-capture');folder.mkdir();record={};writes=[];observations=iter((response,receipt))
        upload=struct.pack('<4I2Q',0x55494453,1,len(upload_bytes),0,*struct.unpack_from('<2Q',session,24))+upload_bytes
        def save():capture.supervisor.save_json(folder/'session.json',record)
        def acknowledge(data):
            copied=json.loads((folder/'session.json').read_text())['deinterlace_output']
            assert copied['ack_write_intent'] and (folder/copied['path']).read_bytes()==payload
            assert copied.get('jobs',1)==jobs and data==capture.ack(response,session);writes.append('ack')
        captured=capture.capture(lambda request,timeout:next(observations),session,folder,record,save,100,upload=upload,
            send_upload=lambda data:writes.append('upload'),send_ack=acknowledge,now=lambda:1,pause=lambda _:None)
        assert captured['ack_observed'] and writes==['upload','ack'] and record['deinterlace_result']=='observed-success'
        assert captured['matches_cpu_reference'] is (True if mode==1 else None)
        (a.output/(name+'-allocation.json')).write_text(json.dumps(proof,indent=2)+'\n')
        passed(name,mode=mode,strength=strength,repeats=repeats,statistics=kind,submissions=m.started,gains=m.gains,
            refills=sum(1 for _,flight in m.cpu_writes if not flight)>0,response_bytes=len(response),
            response_sha256=hashlib.sha256(response).hexdigest(),allocation_check=True,pixel_arithmetic_checked=mode==1)
        return m
    nominal('tnr-high-fall-rise',3,3,3,'fall-rise',checkpoint=True)
    nominal('tnr-low-random',3,1,2,'random')
    nominal('tnr-fixed',3,0,1,'random')
    nominal('motion',2,0,2,'random')
    nominal('weave',1,0,3,'random')
    # A single-job session keeps the earlier one-frame response.
    m=nominal('tnr-single',3,2,0,'random');assert not m.stat_reads
    def failure(name,mode,strength,repeats,kind,variant,started,**checks):
        session,frames,_=session_for(template,mode,strength,repeats);stats=statistics_for(kind,repeats)
        m=fresh(im,session,frames,stats,variant);m.image_hash=report['image_sha256'];m.run()
        assert m.result()==2 and m.started==started,(name,m.result(),m.started,m.events[-8:])
        assert all(row['state']==2 for row in m.rows()['uncached_pool']),('retained',name)
        refilled=sum(1 for _,flight in m.cpu_writes if not flight)
        if 'refills' in checks:assert refilled==checks['refills'],(name,refilled)
        response=serializer(m)(b'\0');assert len(response)==192
        decoded=capture.decode(response,session);assert decoded['status']==2 and decoded['finished_jobs']==checks['finished']
        passed(name,variant=variant,submissions=m.started,retained=True,refilled_writes=refilled,finished_jobs=decoded['finished_jobs'])
    # A statistics read the translation check refuses stops after job 0.
    failure('statistics-translation',3,3,2,'random','statistics-translation',1,refills=0,finished=1)
    # The port refuses statistics the source would divide by zero or overflow.
    failure('statistics-divisor',3,3,2,'divisor','nominal',1,refills=0,finished=1)
    failure('statistics-field-order',2,0,2,'field-order','nominal',1,refills=0,finished=1)
    # A third job without finish, or a second with busy set, retains everything.
    failure('nonfinish',3,2,3,'random','nonfinish',3,finished=2)
    failure('busy-finish',2,0,2,'random','busy-finish',2,finished=1)
    failure('input-mutated',3,1,2,'random','input-mutated',2,finished=2)
    failure('guard',2,0,1,'random','guard',1,refills=0,finished=1)
    failure('iommu-fault',3,1,3,'random','iommu-fault',3,finished=3)
    report['passed']=True;(a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS',len(report['cases']),'native sequence cases')


if __name__=='__main__':main()
