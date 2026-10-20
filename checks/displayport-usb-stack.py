#!/usr/bin/env python3
"""One actual-linked DisplayPort TinyUSB high-speed reply ownership case.

The supplied owner's terminal checkpoint/reply is required; no DisplayPort
result is fabricated and no DisplayPort task is replayed. The existing G2D
fixture executes TinyUSB configuration, FIFO, endpoint and completion code.
Only DCD physical effects, board/provider readiness and scheduling are modeled.
"""
import argparse,hashlib,importlib.util,json
from pathlib import Path
from elftools.elf.elffile import ELFFile
from elftools.dwarf.dwarf_expr import DWARFExprParser
from unicorn.arm64_const import UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('displayport_usb_stack_base',ROOT/'checks/g2d-usb-stack.py');stack=importlib.util.module_from_spec(s);s.loader.exec_module(stack)
EXPECTED='916462470c39d569d6a573cffbf33aa9d1f01117c534877eb46e65333289ddac'
EXPECTED_REPLY='6026eb1670f39da4b99b6196650018b8d894ee2befd88768a6eb5a686357f364'
def output_variables(path):
 result={}
 with path.open('rb') as f:
  dwarf=ELFFile(f).get_dwarf_info()
  for cu in dwarf.iter_CUs():
   if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/displayport/output.c'):continue
   for d in cu.iter_DIEs():
    if d.tag!='DW_TAG_variable' or 'DW_AT_name' not in d.attributes or 'DW_AT_location' not in d.attributes:continue
    loc=d.attributes['DW_AT_location']
    if loc.form!='DW_FORM_exprloc':continue
    ops=DWARFExprParser(dwarf.structs).parse_expr(loc.value)
    if not ops or ops[0].op_name not in ('DW_OP_addr','DW_OP_addrx'):continue
    if len(ops)>1:
     # Clang represents optimized booleans/sent as one-byte storage followed
     # by a constant scale. Only its address is used for zero-state checks.
     assert len(ops)==7 and [x.op_name for x in ops[1:]] in (['DW_OP_deref_size','DW_OP_lit1','DW_OP_mul','DW_OP_lit0','DW_OP_plus','DW_OP_stack_value'],['DW_OP_deref_size','DW_OP_constu','DW_OP_mul','DW_OP_lit0','DW_OP_plus','DW_OP_stack_value']),d.attributes['DW_AT_name'].value
    result[d.attributes['DW_AT_name'].value.decode()]=ops[0].args[0] if ops[0].op_name=='DW_OP_addr' else dwarf.get_addr(cu,ops[0].args[0])
 return result

class Machine(stack.Machine):
 def __init__(self,*args):self.fifo_writes=[];super().__init__(*args)
 def hook(self,cpu,at,size,unused):
  if self.names.get(at)=='tud_vendor_n_write':self.fifo_writes.append(cpu.reg_read(UC_ARM64_REG_X0+2))
  super().hook(cpu,at,size,unused)

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for key in ['image','checkpoint','reply','output']:p.add_argument('--'+key,type=Path,required=True)
 a=p.parse_args();assert hashlib.sha256(a.image.read_bytes()).hexdigest()==EXPECTED
 checkpoint_raw=a.checkpoint.read_bytes();checkpoint=json.loads(checkpoint_raw)
 assert checkpoint['image_sha256']==EXPECTED
 reply=a.reply.read_bytes();assert len(reply)==1024
 assert hashlib.sha256(reply).hexdigest()==checkpoint['reply_sha256']==EXPECTED_REPLY
 e=stack.base.Image(a.image,layout_names={'dp_application'});types,sizes,enums=stack.metadata(a.image)
 m=Machine(e,types,sizes,enums,2);uv=output_variables(a.image)
 for name in ['response','active','sent','transferred','zlp']:
  if name not in uv:uv[name]=e.address(name)
 app=e.address('dp_app');af=e.layouts['dp_application']
 with a.image.open('rb') as f:
  objects={int(x['st_value']):int(x['st_size']) for x in ELFFile(f).get_section_by_name('.symtab').iter_symbols() if x['st_info']['type']=='STT_OBJECT' and x['st_size']}
 widths={name:objects[uv[name]] for name in ['active','sent','transferred','zlp']}
 # Restore only the memory explicitly hashed by the supplied owner checkpoint;
 # never resume its CPU/task context. Scratch remains outside admitted pools.
 for row in checkpoint['memory']:
  path=Path(row['file']);path=path if path.is_absolute() else a.checkpoint.parent/path
  data=path.read_bytes();assert hashlib.sha256(data).hexdigest()==row['sha256']
  assert any(first<=row['address'] and row['address']+len(data)<=last for first,last in [(0x41000000,0x44000000),(0x48000000,0x49000000),(0x78000000,0x79000000)])
  m.cpu.mem_write(row['address'],data)
 with a.image.open('rb') as f:
  for segment in ELFFile(f).iter_segments():
   if segment['p_type']=='PT_LOAD' and segment['p_flags']&1:
    assert bytes(m.cpu.mem_read(int(segment['p_vaddr']),int(segment['p_filesz'])))==segment.data(),'Checkpoint changed executable bytes'
 assert m.word(app+af['terminal'])==1
 assert bytes(m.cpu.mem_read(app+af['reply'],1024))==reply
 assert int.from_bytes(reply[:8],'little')==0x4153373344505231
 assert int.from_bytes(reply[8:12],'little')==1 and int.from_bytes(reply[12:16],'little')==1024
 assert int.from_bytes(reply[16:20],'little') in (1,2),'Owner must supply an actual terminal observation'
 assert not m.word(uv['active'],1),'Owner checkpoint must precede USB capture'
 def field(name):return m.word(uv[name],widths[name])
 def immutable():
  assert bytes(m.cpu.mem_read(app+af['reply'],1024))==reply
  assert bytes(m.cpu.mem_read(uv['response'],1024))==reply
 a.output.mkdir(parents=True,exist_ok=False)
 m.enumerate();m.request(b'\0');assert field('active')==1
 m.tick();assert m.fifo_writes==[1024] and m.seen['tud_vendor_n_write']==1
 assert m.pending[0x81]['count']==512;immutable()
 # The second query can enter the RX FIFO, but cannot borrow the reply until
 # the first response's automatic terminal ZLP has actually completed.
 m.request(b'\0');assert m.seen['tud_vendor_n_read']==1 and field('transferred')==0
 m.complete(0x81);m.tick();immutable()
 assert field('active')==1 and field('transferred')==512 and m.pending[0x81]['count']==512
 assert m.seen['tud_vendor_n_read']==1
 m.complete(0x81);m.tick();immutable()
 assert field('active')==field('zlp')==1 and field('transferred')==1024
 assert m.pending[0x81]['count']==0 and m.seen['tud_vendor_n_read']==1
 assert m.seen['tud_vendor_n_write']==1
 m.complete(0x81);m.tick();immutable()
 assert list(map(len,m.transfers))==[512,512,0] and b''.join(m.transfers)==reply
 assert m.seen['tud_vendor_n_read']==2 and field('active')==1
 assert field('sent')==field('transferred')==field('zlp')==0
 assert m.seen['tud_vendor_n_write']==1 and not m.failure
 assert all(m.seen[name] for name in ['tusb_rhport_init','dcd_event_handler','tud_task_ext','vendord_open','vendord_xfer_cb','tu_fifo_read_n_access_mode','tu_fifo_write_n_access_mode','usbd_edpt_claim','usbd_edpt_xfer','tud_vendor_tx_cb'])
 (a.output/'reply.bin').write_bytes(b''.join(m.transfers))
 result=dict(passed=True,image_sha256=EXPECTED,owner_checkpoint_sha256=hashlib.sha256(checkpoint_raw).hexdigest(),owner_reply_sha256=hashlib.sha256(reply).hexdigest(),terminal_status=int.from_bytes(reply[16:20],'little'),dcd_transfer_lengths=[512,512,0],single_fifo_write=True,next_query_admitted_only_after_zlp=True,host_read_bytes=1025,executed=dict(m.seen),scope=__doc__,owner_scope=checkpoint['scope'],modeled=['DCD physical transfer and completion effects','Native board/provider readiness','Scheduling and counter','Inherited device effects already modeled by terminal-checkpoint owner'],displayport_replay=False,native_build=False,physical_access=False)
 (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
 print('PASS linked TinyUSB 512/512/ZLP ownership and next-query admission')
if __name__=='__main__':main()
