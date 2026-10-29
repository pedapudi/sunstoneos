#!/usr/bin/env python3
"""Execute streaming uploads and copied responses through the linked TinyUSB task.

Native construction and USB provider preparation are restored from hashed owner
checkpoints. DCD completion, scheduling, time and live provider observations are
modeled. The actual USB task, TinyUSB FIFO/endpoint stack, stream producer APIs
and output observation writer execute. This check does not run DMA or establish
physical USB throughput, cache visibility, I2S continuity or rendered sound.
"""
import argparse,copy,gzip,hashlib,importlib.util,json,pickle,struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from elftools.dwarf.dwarf_expr import DWARFExprParser
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import (UC_ARM64_REG_X0,UC_ARM64_REG_SP,UC_ARM64_REG_LR,
    UC_ARM64_REG_PC,UC_ARM64_REG_PSTATE,UC_ARM64_REG_Q0,UC_ARM64_REG_FPCR,
    UC_ARM64_REG_FPSR,UC_ARM64_REG_FP)
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
prior=module('hdmi_stream_usb_prior',ROOT/'checks/hdmi-audio-usb-native.py')
t=prior.t
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
REGIONS=((0x41000000,0x3000000),(0x48000000,0x1000000),(0x78000000,0x1000000),(0x90000000,0x1000000))
REGISTERS=[*[UC_ARM64_REG_X0+i for i in range(29)],UC_ARM64_REG_FP,UC_ARM64_REG_SP,
 UC_ARM64_REG_LR,UC_ARM64_REG_PC,UC_ARM64_REG_PSTATE,UC_ARM64_REG_FPCR,UC_ARM64_REG_FPSR,
 *[UC_ARM64_REG_Q0+i for i in range(32)]]
ATTRIBUTES=('seen','opened','pending','events','packets','transfers','started','now','stop','failure',
 'current','handles','reads','provider_busy')
def layouts(path):
 wanted={'hdmi_application','hdmi_stream','hdmi_audio_observation_mailbox','hdmi_audio_usb'};result={};variables={}
 with path.open('rb') as f:
  dwarf=ELFFile(f).get_dwarf_info()
  for cu in dwarf.iter_CUs():
   unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
   for d in cu.iter_DIEs():
    name=d.attributes.get('DW_AT_name')
    if not name:continue
    name=name.value.decode()
    if d.tag=='DW_TAG_structure_type' and name in wanted and 'DW_AT_byte_size' in d.attributes:
     row={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value)
          for c in d.iter_children() if c.tag=='DW_TAG_member' and 'DW_AT_name' in c.attributes}
     assert name not in result or result[name]==row,('ambiguous-layout',name)
     result[name]=row
    if d.tag=='DW_TAG_variable' and unit.endswith('/apps/hdmi-audio/usb.c') and 'DW_AT_location' in d.attributes:
     location=d.attributes['DW_AT_location']
     if location.form!='DW_FORM_exprloc':continue
     ops=DWARFExprParser(dwarf.structs).parse_expr(location.value)
     direct=len(ops)==1
     boolean=[op.op_name for op in ops[1:]]==['DW_OP_deref_size','DW_OP_lit1','DW_OP_mul','DW_OP_lit0','DW_OP_plus','DW_OP_stack_value'] and ops[1].args==[1]
     if (direct or boolean) and ops[0].op_name in ('DW_OP_addr','DW_OP_addrx'):
      variables[name]=ops[0].args[0] if ops[0].op_name=='DW_OP_addr' else dwarf.get_addr(cu,ops[0].args[0])
 assert wanted<=result.keys(),wanted-result.keys()
 assert {'received','payload_received','exchange_deadline','tx_active','tx_zlp'}<=variables.keys()
 return result,variables
class USB(prior.USB):
 def __init__(self,*args):
  self.reads=[];self.provider_busy=False;super().__init__(*args)
 def hook(self,cpu,at,size,unused):
  if getattr(self,'custom',{}).get(at)=='healthy-provider' and self.provider_busy:
   self.ret(0);return
  if getattr(self,'names',{}).get(at)=='tud_vendor_n_read':
   self.reads.append(dict(pointer=cpu.reg_read(UC_ARM64_REG_X0+1),bytes=cpu.reg_read(UC_ARM64_REG_X0+2)))
  return super().hook(cpu,at,size,unused)
 def response(self):
  first=len(self.transfers)
  for _ in range(10):
   self.poll();self.tick()
   if 0x81 in self.pending:break
  assert 0x81 in self.pending
  self.complete(0x81);self.tick()
  assert self.pending[0x81]['count']==0 and self.word(self.image.address('observation'))==3
  self.complete(0x81);self.tick()
  assert self.word(self.image.address('observation'))==0
  wire=b''.join(self.transfers[first:]);assert len(wire)==512;return wire
 def upload(self,header,payload,parts):
  self.request(header)
  pos=0
  for count in parts:
   self.request(payload[pos:pos+count]);pos+=count
  assert pos==len(payload)
  return self.response()
def snapshot(m,path,identity):
 saved=dict(identity=identity,memory=[(at,bytes(m.cpu.mem_read(at,count))) for at,count in REGIONS],
  registers={r:m.cpu.reg_read(r) for r in REGISTERS},attributes={k:copy.deepcopy(getattr(m,k)) for k in ATTRIBUTES})
 path.write_bytes(gzip.compress(pickle.dumps(saved),mtime=0))
def restore(m,path,identity):
 saved=pickle.loads(gzip.decompress(path.read_bytes()));assert saved['identity']==identity
 for at,data in saved['memory']:m.cpu.mem_write(at,data)
 for reg,value in saved['registers'].items():m.cpu.reg_write(reg,value)
 for key,value in saved['attributes'].items():setattr(m,key,copy.deepcopy(value))
 m.context=m.cpu.context_save()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for key in ('image','session','pcm','checkpoint','output'):p.add_argument('--'+key,type=Path,required=True)
 p.add_argument('--terminal-checkpoint',type=Path)
 p.add_argument('--tail-checkpoint',type=Path)
 p.add_argument('--resume',action='store_true');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=a.resume)
 raw=a.session.read_bytes();pcm=a.pcm.read_bytes();assert len(raw)==848 and len(pcm)>192000
 image=t.base.Image(a.image,layout_names=());image.path=a.image;types,sizes,enums=t.metadata(a.image)
 ls,uv=layouts(a.image);sf=ls['hdmi_stream'];af=ls['hdmi_application'];of=ls['hdmi_audio_observation_mailbox']
 stream=image.address('stream');app=image.address('app');obs=image.address('observation')
 identity=dict(image=sha(a.image),session=sha(a.session),pcm=sha(a.pcm),checkpoint=sha(a.checkpoint))
 path=a.output/'result.json';record=json.loads(path.read_text()) if path.exists() else dict(identity=identity,cases={},physical_access=False,scope=__doc__)
 assert record['identity']==identity,'Refuse stale native inputs'
 epochs=struct.unpack_from('<2Q',raw,24);frames=len(pcm)//4;periods=(frames+1023)//1024
 def seed(checkpoint):
  meta=json.loads(checkpoint.read_text());assert meta.get('image_sha256',meta.get('image'))==identity['image'] and meta['session_sha256']==identity['session']
  memory=Path(meta.get('ram_file',meta.get('ram')));memory=memory if memory.is_absolute() else checkpoint.parent/memory
  assert sha(memory)==meta['ram_sha256'];data=memory.read_bytes()
  if data[:2]==b'\x1f\x8b':data=gzip.decompress(data)
  assert len(data)==0x3000000
  return data,meta.get('time_us',meta.get('time')),meta['handles']
 data,now,handles=seed(a.checkpoint)
 def fork(speed=2,terminal=False,tail=False):
  checkpoint=a.tail_checkpoint if tail else a.terminal_checkpoint if terminal else a.checkpoint
  assert checkpoint
  raw_state,time,owners=seed(checkpoint)
  m=USB(image,types,sizes,enums,speed,raw_state,time,owners)
  usb=image.address('usb');uf=ls['hdmi_audio_usb']
  assert m.word(usb+uf['rail_index'])==4 and m.word(usb+uf['step'])==5, 'USB providers were not prepared'
  key=dict(identity=identity,speed=speed,checkpoint=sha(checkpoint))
  setup=a.output/(('tail-'+sha(checkpoint)[:12]+'-' if tail else 'terminal-' if terminal else 'prepared-')+str(speed)+'.pkl.gz')
  if setup.exists():restore(m,setup,key)
  else:
   m.cpu.mem_write(0x90ff0000,b"\xa5"*0xd000)
   m.enumerate();assert m.word(usb+uf['ready'])==1;snapshot(m,setup,key)
  return m
 def field(m,name,n=8):return m.word(stream+sf[name],n)
 def header(op=0,payload=0,seq=0,rid=1):return struct.pack('<4I2Q2IQ',0x4f414748,2,rid,48,*epochs,op,payload,seq)
 def wire_ok(wire,rid,accepted,seq):
  assert struct.unpack_from('<4I',wire)==(0x52414748,2,512,rid)
  assert wire[64:96]==raw[48:80] and wire[96:128]==hashlib.sha256(raw).digest()
  credits,result,got,committed,consumed,echo=struct.unpack_from('<II4Q',wire,288)
  assert result==0 and got==accepted and echo==seq and consumed<=committed<=got
  assert not any(wire[328:]);return credits
 def pass_case(name,m,**details):
  stack=bytes(m.cpu.mem_read(0x90ff0000,0xd000));first=next((i for i,b in enumerate(stack) if b!=0xa5),len(stack))
  record['cases'][name]=dict(passed=True,check_source_sha256=sha(__file__),executed=dict(m.seen),usb_stack_written_bytes=len(stack)-first,**details)
  path.write_text(json.dumps(record,indent=2)+'\n');print('PASS',name,flush=True)
 def parked(m):
  try:m.tick()
  except AssertionError:assert m.stop=='park'
  else:raise AssertionError('Expected retained stop')
  assert m.word(app+af['fatal'])==1
 current=None
 try:
  for speed,label in ((1,'full'),(2,'high')):
   name='fragmented-upload-'+label
   if name in record['cases']:continue
   current=m=fork(speed);payload=pcm[:4096];req=header(1,4096)
   m.request(req[:17]);assert m.word(obs)==0
   m.request(req[17:]);assert m.word(obs)==4 and field(m,'head')==0
   pos=0
   parts=[31]+[min(m.mps,4096-31-i*m.mps) for i in range((4096-31+m.mps-1)//m.mps)]
   for count in parts:
    m.request(payload[pos:pos+count]);pos+=count
    assert bytes(m.cpu.mem_read(stream+sf['staging'],pos))==payload[:pos]
    assert field(m,'head')==int(pos==4096)
   assert m.word(obs)==1 and field(m,'tail')==0
   wire=m.response();assert wire_ok(wire,1,1024,0)==3
   staging_reads=[r for r in m.reads if stream+sf['staging']<=r['pointer']<stream+sf['staging']+16384]
   assert sum(r['bytes'] for r in staging_reads)==4096 and len(staging_reads)==len(parts)
   assert all(m.seen[k] for k in ('tud_vendor_n_read','tu_fifo_read_n_access_mode','vendord_xfer_cb','tud_vendor_tx_cb'))
   (a.output/(name+'.bin')).write_bytes(wire)
   pass_case(name,m,payload_read_bytes=4096,direct_staging_reads=len(staging_reads),terminal_zlp=True)
  if a.terminal_checkpoint:
   for speed,label in ((1,'full'),(2,'high')):
    name='terminal-response-'+label
    if name in record['cases']:continue
    current=m=fork(speed,True);assert field(m,'head')==periods
    m.request(header(seq=periods));wire=m.response();wire_ok(wire,1,frames,periods)
    assert struct.unpack_from('<2I',wire,40)==(1,7) and struct.unpack_from('<4Q',wire,128)==(0x4153373348415231,2,160,2)
    assert struct.unpack_from('<Q',wire,176)[0]==len(pcm)
    assert wire[224:256]==hashlib.sha256(pcm).digest()
    (a.output/(name+'.bin')).write_bytes(wire);pass_case(name,m,terminal_zlp=True,actual_native_completion=True)
  name='short-final-upload-and-explicit-end'
  if a.tail_checkpoint and name not in record['cases']:
   current=m=fork(tail=True);seq=field(m,'head');tail=field(m,'tail')
   assert seq==periods-1 and field(m,'eof',4)==0
   assert seq-tail<4, 'Short-upload checkpoint must include a returned staging credit'
   payload=pcm[seq*4096:];assert 0<len(payload)<4096
   parts=[min(3,len(payload))]
   while sum(parts)<len(payload):parts.append(min(512,len(payload)-sum(parts)))
   wire=m.upload(header(1,len(payload),seq),payload,parts);wire_ok(wire,1,frames,seq)
   assert field(m,'head')==periods and field(m,'tail')==tail and field(m,'eof',4)==0
   assert bytes(m.cpu.mem_read(stream+sf['staging']+(seq%4)*4096,len(payload)))==payload
   m.request(header(2,seq=periods,rid=2));wire=m.response()
   assert wire_ok(wire,2,frames,periods)==0 and field(m,'eof',4)==field(m,'closed',4)==1
   pass_case(name,m,final_payload_bytes=len(payload),actual_prior_uploaded_periods=seq,
             tail_checkpoint_sha256=sha(a.tail_checkpoint),end_required=True)
  name='partial-upload-timeout'
  if name not in record['cases']:
   current=m=fork();m.request(header(1,4096));m.request(pcm[:127]);before=bytes(m.cpu.mem_read(stream+sf['staging'],4096))
   assert m.word(obs)==4 and field(m,'head')==0 and m.word(uv['payload_received'])==127
   m.now=m.word(uv['exchange_deadline'],8);parked(m)
   assert m.word(obs)==4 and field(m,'head')==0 and m.word(uv['payload_received'])==127
   assert bytes(m.cpu.mem_read(stream+sf['staging'],4096))==before
   pass_case(name,m,retained_input_bytes=127,published_periods=0)
  name='provider-busy-keeps-exchange-deadline'
  if name not in record['cases']:
   current=m=fork();m.request(header());assert m.word(obs)==1
   deadline=m.word(uv['exchange_deadline'],8);m.provider_busy=True
   for _ in range(4):m.tick();assert m.word(uv['exchange_deadline'],8)==deadline
   m.now=deadline;parked(m)
   assert m.word(obs)==1 and m.word(uv['exchange_deadline'],8)==deadline
   pass_case(name,m,absolute_exchange_deadline=deadline,reply_pending=True)
  name='partial-upload-cancel-retains'
  if name not in record['cases']:
   current=m=fork();m.request(header(1,4096));m.request(pcm[:127]);before=bytes(m.cpu.mem_read(stream+sf['staging'],4096))
   assert m.call('hdmi_stream_cancel')==1 and field(m,'closed',4)==field(m,'cancel',4)==1
   m.tick();assert m.word(obs)==4 and m.word(uv['payload_received'])==127 and field(m,'head')==0
   assert bytes(m.cpu.mem_read(stream+sf['staging'],4096))==before
   pass_case(name,m,retained_input_bytes=127,cancel_api_owner='USB',interleaved_wire_cancel=False)
  name='corrupt-upload-preserved-for-authentication'
  if name not in record['cases']:
   current=m=fork();corrupt=bytearray(pcm[:4096]);corrupt[101]^=1
   wire=m.upload(header(1,4096),bytes(corrupt),[512]*8);wire_ok(wire,1,1024,0)
   assert bytes(m.cpu.mem_read(stream+sf['staging'],4096))==corrupt and field(m,'tail')==field(m,'committed')==0
   pass_case(name,m,accepted_frames=1024,committed_frames=0,authentication='Separate actual output/DMA task check')
  name='cancel-retains-outstanding-response'
  if name not in record['cases']:
   current=m=fork();m.request(header());m.poll();assert m.word(obs)==2
   before=bytes(m.cpu.mem_read(obs+of['reply'],512));assert m.call('hdmi_stream_cancel')==1
   m.tick();assert m.word(obs)==3 and bytes(m.cpu.mem_read(obs+of['reply'],512))==before
   m.complete(0x81);m.tick();assert m.word(obs)==3 and m.pending[0x81]['count']==0
   assert bytes(m.cpu.mem_read(obs+of['reply'],512))==before
   m.complete(0x81);m.tick();assert m.word(obs)==0 and field(m,'cancel',4)==1
   pass_case(name,m,reply_immutable_until_zlp=True)
  for zlp in (False,True):
   name='late-'+('zlp' if zlp else 'data')+'-retains-response'
   if name in record['cases']:continue
   current=m=fork();m.request(header());m.poll();m.tick();assert m.word(obs)==3
   if zlp:m.complete(0x81);m.tick();assert m.pending[0x81]['count']==0
   before=bytes(m.cpu.mem_read(obs+of['reply'],512));m.now=m.word(uv['exchange_deadline'],8)
   m.complete(0x81);parked(m)
   assert m.word(obs)==3 and m.word(uv['tx_active'],1)==1 and bytes(m.cpu.mem_read(obs+of['reply'],512))==before
   pass_case(name,m,reply_retained=True,terminal_zlp=zlp)
  name='early-end-refused'
  if name not in record['cases']:
   current=m=fork();m.complete(2,header(2));parked(m)
   assert field(m,'head')==field(m,'tail')==0 and field(m,'eof',4)==0
   pass_case(name,m,no_payload_publication=True)
 except BaseException:
  if current is not None:snapshot(current,a.output/'failure.pkl.gz',identity)
  raise
 record['passed']=bool(a.terminal_checkpoint);record['terminal_checkpoint']=sha(a.terminal_checkpoint) if a.terminal_checkpoint else None
 path.write_text(json.dumps(record,indent=2)+'\n')
if __name__=='__main__':main()
