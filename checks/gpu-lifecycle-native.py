#!/usr/bin/env python3
"""Actual retained depth lifecycle client/USB producer; GPU/service effects modeled.

The native application parser, mailbox owner, lifecycle caller and pinned TinyUSB
FIFO/callback code execute. Service completions and framebuffer bytes are model
inputs, bound to the separate actual child disposal/reconstruction records. No
GPU shader, firmware, physical shutdown, SMP schedule or pixel proof is claimed.
"""
import argparse,hashlib,importlib.util,json,struct,types,gzip,pickle
from pathlib import Path
from unicorn.arm64_const import *
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
runtime=module('lifecycle_runtime',ROOT/'checks/gpu-texture-runtime.py')
images=module('lifecycle_images',ROOT/'checks/npu-large-native.py')

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()

class Machine(runtime.Machine):
    def __init__(self,image):
        super().__init__(image,2,3);self.queue_epoch=1;self.disposed=False;self.control_budgets=[]
        self.hooks[image.address('pvr_gpu_service_send_epoch')]='pvr_gpu_service_send_epoch'
        self.cpu.mem_write(image.address('texture_session')+616,struct.pack('<2Q',64,64))
        self.cpu.mem_write(image.address('texture_session'),bytes(self.cpu.mem_read(self.app+self.fields['session'],592)))
        self.reply_phase=3
    def render(self):
        # Opaque modeled completed framebuffer, not a new pixel oracle.
        self.snapshot=bytes([len(self.inputs)&255])*16384
    def hook(self,cpu,address,size,unused):
        name=self.hooks.get(address);a=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(8)]
        if name in ('pvr_gpu_service_send','pvr_gpu_service_send_epoch'):
            explicit=name.endswith('_epoch');q=a[2] if explicit else a[1];op=int.from_bytes(cpu.mem_read(q,4),'little')
            timeout=int.from_bytes(cpu.mem_read(q+8,8),'little');slot=a[4] if explicit else a[3]
            if explicit:
                assert a[1]==self.queue_epoch
                self.control_budgets.append((op,timeout))
                assert timeout==(15000000 if op in (1,11) else 1000000)
            if op==2:self.closed=True;self.reply_phase=5
            elif op==11:assert self.closed;self.disposed=True;self.reply_phase=15
            elif op==1 and explicit:
                assert self.disposed;self.queue_epoch+=1;self.closed=self.disposed=False
                self.frame=self.generation=0;self.draft=bytearray();self.snapshot=b'';self.reply_phase=3
            else:assert op in (0,1)
            self.ticket+=1;self.write(slot,self.ticket,8);self.ret(1);return
        if name=='pvr_gpu_service_texture':
            assert a[1]==self.queue_epoch and not self.closed
            # Existing fixture models only the service boundary. Preserve its
            # original independent session fixture while checking new epoch.
            cpu.reg_write(UC_ARM64_REG_X1,123)
            return super().hook(cpu,address,size,unused)
        if name=='pvr_gpu_service_collect':
            fields=self.image.layouts['pvr_gpu_service_reply'];p=a[2];cpu.mem_write(p,bytes(self.reply_size))
            for k,v in dict(epoch=self.queue_epoch,ticket=a[1],phase=self.reply_phase,
                render_sequence=self.frame,render_phase=9 if self.frame else 2,
                render_checked=len(self.snapshot)+128 if self.frame else 0,
                texture_generation=self.generation,texture_received=len(self.draft)).items():
                self.write(p+fields[k],v,8 if k in ('epoch','ticket','texture_generation') else 4)
            self.ret(1);return
        super().hook(cpu,address,size,unused)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('image','leaf-proof','client','output'):p.add_argument('--'+n,type=Path,required=True)
    p.add_argument('--resume',type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    leaf=json.loads(a.leaf_proof.read_text());assert leaf['passed'] and leaf['new_open']['epoch']==2
    image=images.Image(a.image);image.path=a.image
    # All new called/observed symbols and retained layouts resolve before tasks.
    for name in ('client','gpu_application_lifecycle','gpu_application_texture_client','gpu_application_texture_usb',
        'pvr_gpu_service_send_epoch','pvr_gpu_service_collect','vendord_xfer_cb'):
        image.address(name,'/apps/gpu-render/main.c' if name=='client' else None)
    m=Machine(image);client=module('new_lifecycle_host',a.client)
    clock=types.SimpleNamespace(monotonic=lambda:m.now/1000000,sleep=lambda seconds:m.pump())
    client.time=clock;client.common.time=clock
    records=[];current=None
    if a.resume:
        with gzip.open(a.resume,'rb') as f:restored=pickle.load(f)
        if 'image_sha256' in restored:assert restored['image_sha256']==sha(a.image)
        for at,data in restored['memory']:m.cpu.mem_write(at,data)
        m.__dict__.update(restored['attrs']);m.contexts={}
        for name,registers in restored['contexts'].items():
            for reg,value in registers.items():m.cpu.reg_write(int(reg),value)
            m.contexts[name]=m.cpu.context_save()
        for reg,value in restored['regs'].items():m.cpu.reg_write(int(reg),value)
        records=restored['records'];current=records[-1]
        assert restored['observed']==dict(state=1,sequence=1,queue_epoch=1)

    def save(name,data):
        path=a.output/name;path.write_bytes(data);return dict(path=str(path.resolve()),sha256=sha(path))
    def write(data,remaining):
        nonlocal current
        current=dict(request=save(f'{len(records):03d}-request.bin',data));records.append(current)
        return m.write_request(data,remaining)
    def read(n,remaining):
        raw=m.read(n,remaining)
        if 'response' not in current:
            if not raw:return raw
            assert len(raw)==72;current['response']=save(f'{len(records)-1:03d}-response.bin',raw)
            if struct.unpack_from('<I',raw,40)[0]:current['_body']=bytearray()
        elif '_body' in current:current['_body'].extend(raw)
        else:assert not raw
        return raw
    inputs=[]
    for scene in range(2):
        vertices=[[-1,-1,.25,1,0,0],[1,-1,.25,1,1,0],[-1,1,.25,1,0,1],[1,1,.25,1,1,1]]
        item=runtime.host.input_bytes(vertices,[0,1,3,0,3,2],2,2,bytes([scene,0,0,255])*4,profile=3,state=5)
        inputs.append(item)
    c=client.Texture(write,read,123,64,64,deadline=60,retain=lambda kind,data,identity:None,profile=3)
    if a.resume:
        c.state=1;c.sequence=1;c.queue_epoch=1;c.generation=m.generation;c.copied=(1,1)

    def checkpoint(label):
        registers=[UC_ARM64_REG_X0+i for i in range(31)]+[UC_ARM64_REG_SP,UC_ARM64_REG_PC,UC_ARM64_REG_PSTATE]+[UC_ARM64_REG_Q0+i for i in range(32)]+[UC_ARM64_REG_FPCR,UC_ARM64_REG_FPSR]
        original=m.cpu.context_save();contexts={}
        for name,context in m.contexts.items():
            m.cpu.context_restore(context);contexts[name]={str(reg):m.cpu.reg_read(reg) for reg in registers}
        m.cpu.context_restore(original)
        state=dict(image_sha256=sha(a.image),memory=[(at,bytes(m.cpu.mem_read(at,n))) for at,n in ((0x41000000,0x1200000),(0x43000000,0x1000000),(0x48000000,65536))],
            regs={str(reg):m.cpu.reg_read(reg) for reg in registers},contexts=contexts,
            attrs={k:v for k,v in vars(m).items() if k not in ('image','cpu','contexts','hooks')},records=records,
            observed=dict(state=c.state,sequence=c.sequence,queue_epoch=c.queue_epoch))
        with gzip.open(a.output/(label+'.pkl.gz'),'wb') as f:pickle.dump(state,f,protocol=5)
    try:
        if not a.resume:c.submit(inputs[0]);checkpoint('first-output')
        c.close();c.dispose();c.open();checkpoint('reopened')
        c.submit(inputs[1]);c.close();checkpoint('terminal')
    except Exception:
        checkpoint('failure');raise
    for record in records:
        if '_body' in record:
            data=bytes(record.pop('_body'));assert len(data)==16384
            record['body']=save(record['response']['path'].split('/')[-1].replace('response','body'),data)
    input_rows=[]
    for i,item in enumerate(inputs):
        row=save(f'input-{i}.bin',item[1]);row.update(shape=item[0],state=item[2]);input_rows.append(row)
    # This session fixture models prior authentication and retains the exact
    # native getter geometry; it conveys no physical admission authority.
    session=bytes(m.cpu.mem_read(image.address('texture_session'),632));session_row=save('session.bin',session)
    producer=dict(schema='pvr-gpu-lifecycle-native-producer-v1',modeled=True,physical_authority=False,
        image_sha256=sha(a.image),session_epoch=123,profile='depth',exchanges=records,
        session=session_row,leaf_proof=dict(path=str(a.leaf_proof.resolve()),sha256=sha(a.leaf_proof)),
        control_budgets=m.control_budgets,endpoint_completions=m.packets,zlps=m.zlps,scope=__doc__)
    producer_path=a.output/'producer.json';producer_path.write_text(json.dumps(producer,indent=2)+'\n')
    fixture=dict(schema='pvr-gpu-lifecycle-host-fixture-v1',modeled=True,physical_authority=False,
        image=dict(path=str(a.image.resolve()),sha256=sha(a.image)),session_epoch=123,profile='depth',width=64,height=64,
        inputs=input_rows,exchanges=records,producer=dict(path=str(producer_path.resolve()),sha256=sha(producer_path)))
    (a.output/'fixture.json').write_text(json.dumps(fixture,indent=2)+'\n')
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,image_sha256=sha(a.image),exchanges=len(records),scope=__doc__),indent=2)+'\n')
    print('PASS actual depth lifecycle application and USB producer',len(records))
if __name__=='__main__':main()
