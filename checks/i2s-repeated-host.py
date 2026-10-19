#!/usr/bin/env python3
"""Use actual compiled I2S USB frames for repeated host-copy and drain checks."""
import argparse,hashlib,importlib.util,json,struct,time
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_SP as SP,UC_ARM64_REG_X0 as X0
ROOT=Path(__file__).resolve().parents[1]
def module(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
p=argparse.ArgumentParser(description=__doc__)
for n in ('fixture','session','output'):p.add_argument('--'+n,type=Path,required=True)
p.add_argument("--case")
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
host=module('repeated_host',ROOT/'tools/check-a733-i2s-supervised.py');session=a.session.read_bytes()
with a.fixture.open('rb') as f:
 elf=ELFFile(f);segments=[(int(p['p_vaddr']),bytes(p.data()),int(p['p_memsz'])) for p in elf.iter_segments() if p['p_type']=='PT_LOAD']
 symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()};fields={}
 for cu in elf.get_dwarf_info().iter_CUs():
  for d in cu.iter_DIEs():
   if d.tag=='DW_TAG_structure_type' and d.attributes.get('DW_AT_name') and d.attributes['DW_AT_name'].value in (b'i2s_application',b'i2s_repeated'):
    fields[d.attributes['DW_AT_name'].value.decode()]={c.attributes['DW_AT_name'].value.decode():int(c.attributes['DW_AT_data_member_location'].value) for c in d.iter_children() if c.tag=='DW_TAG_member'}
cache={};checks={}
def produced(mode):
 if mode in cache:return cache[mode]
 cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);pages=set()
 for at,data,size in segments:
  for page in range(at&~4095,(at+size+4095)&~4095,4096):
   if page not in pages:cpu.mem_map(page,4096);pages.add(page)
  cpu.mem_write(at,data)
 cpu.mem_map(0x41f00000,4096);cpu.mem_write(0x41f00000,session);cpu.mem_map(0x50000000,0x10000)
 cpu.reg_write(SP,0x5000f000);cpu.reg_write(X0,mode);cpu.hook_add(UC_HOOK_INTR,lambda m,*_:m.emu_stop())
 cpu.emu_start(symbols['i2s_check_run'],0,count=3000000)
 result=int.from_bytes(cpu.mem_read(symbols['i2s_check_result'],4),'little')
 app=symbols['app'];repeated=app+fields['i2s_application']['repeated']
 if mode in (2,3,4):
  assert result==99 and int.from_bytes(cpu.mem_read(app+fields['i2s_application']['fatal'],4),'little')==1
  assert int.from_bytes(cpu.mem_read(repeated+fields['i2s_repeated']['tail'],8),'little')==(1 if mode==3 else 0)
  checks[{2:'early-partial-ack-refused',3:'duplicate-wire-ack-refused',4:'foreign-wire-ticket-refused'}[mode]]=True;return None
 assert result==1,(mode,result)
 extent=int.from_bytes(cpu.mem_read(symbols['wire_bytes'],4),'little');value=bytes(cpu.mem_read(symbols['wire'],extent))
 (a.output/f'producer-{mode}.bin').write_bytes(value);cache[mode]=value;return value
if not a.case:
 for mode in (2,3,4):produced(mode)
 high=produced(5);assert host.decoder.decode_repeated(high,session,5,ticket=0x100000001)['ticket']==0x100000001
 checks['ack-preserves-high-ticket-bits']=True
class Clock:
 value=100.
 def now(self):self.value+=.0001;return self.value
 def pause(self,n):self.value+=max(.001,n)
for variant in ('duplex-lag-and-backlog','overrun-drain-and-backlog','missing-tx-never-succeeds','journal-before-ack','interior-zlp','early-overrun-drains-backlog','terminal-cancel-last-publication','terminal-get-last-publication'):
 if a.case and variant!=a.case:continue
 dest=a.output/variant;dest.mkdir();record={};next_period=[0];cancelled=[False];raced=[False];requests=[];clock=Clock()
 def exchange(packet,timeout):
  op,low,high=struct.unpack('<III',packet);requests.append(op);n=next_period[0]
  if op==5:
   assert low|high<<32==n+1;next_period[0]+=1
   return produced((1600 if variant=='early-overrun-drains-backlog' and n==0 else 1000 if variant in ('overrun-drain-and-backlog','early-overrun-drains-backlog') else 600 if cancelled[0] else 400)+n)
  if op==2:
   assert variant!='missing-tx-never-succeeds';cancelled[0]=True
   assert variant in ('overrun-drain-and-backlog','early-overrun-drains-backlog') or n>=8
   return produced(1812 if variant.startswith('terminal-') else 1701 if variant=='early-overrun-drains-backlog' else 1204 if variant=='overrun-drain-and-backlog' else 808)
  assert op==4 and low==0 and high==4096
  if variant=='early-overrun-drains-backlog' and not n:return produced(1500)
  if variant in ('overrun-drain-and-backlog','early-overrun-drains-backlog'):return produced(900+n if n<4 else 1104)
  if cancelled[0]:return produced(300+n if n<12 else 512)
  if n<8:return produced(200+n)
  if variant=='terminal-get-last-publication' and not raced[0]:raced[0]=True;return produced(1912)
  return produced(1408 if variant=='missing-tx-never-succeeds' else 708)
 def save():
  if variant=='journal-before-ack' and record.get('copied_periods'):raise OSError('modeled durable period record failure')
 try:
  if variant=='interior-zlp':
   class Device:
    remaining=b'';empty=False
    def write(self,ep,packet,timeout):assert ep==2;self.remaining=exchange(bytes(packet),timeout);self.empty=False;return 12
    def read(self,ep,count,timeout):
     assert ep==0x81
     if len(self.remaining)<4000 and not self.empty:self.empty=True;return b''
     value=self.remaining[:min(count,257)];self.remaining=self.remaining[len(value):];return value
   host.capture_device(Device(),lambda:None,time.monotonic()+30,session,dest,record,save,pcm=bytes(16384),direction=5)
  else:host.capture_repeated(exchange,session,dest,record,save,100.2 if variant=='missing-tx-never-succeeds' else 130,8,now=clock.now,pause=clock.pause)
 except (ValueError,RuntimeError,OSError):assert variant in ('missing-tx-never-succeeds','journal-before-ack')
 else:
  expected='observed-failure' if variant in ('overrun-drain-and-backlog','early-overrun-drains-backlog') else 'observed-success'
  assert record['i2s_result']==expected and record['copied_i2s_output']['drained']==3
  assert record['copied_i2s_output']['copied_periods']==(4 if variant in ('overrun-drain-and-backlog','early-overrun-drains-backlog') else 12)
 if variant=='missing-tx-never-succeeds':assert 2 not in requests and record['i2s_result']!='observed-success'
 if variant=='journal-before-ack':assert 5 not in requests and next_period[0]==0
 checks[variant]=True
# Negative records derive from the actual producer frame, changing one field.
nominal=produced(200)
for name,offset in (() if a.case else (('wrong-ticket',72),('wrong-frame-count',88),('wrong-session',40),('reserved',120))):
 bad=bytearray(nominal);bad[offset]^=1
 try:
  value=host.decoder.decode_repeated(bytes(bad),session,4,count=4096)
  assert value['ticket']==1
 except (ValueError,AssertionError):checks[name]=True
 else:raise AssertionError(name)
(a.output/'result.json').write_text(json.dumps({'passed':True,'physical':False,'fixture_sha256':hashlib.sha256(a.fixture.read_bytes()).hexdigest(),'cases':checks},indent=2)+'\n')
print('PASS repeated I2S: actual USB completion/ACK refusal,64-bit tickets, both-direction work, failed/canceled backlog, durable copy before ACK and bounded ZLP reads')
