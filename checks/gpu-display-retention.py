#!/usr/bin/env python3
"""Isolate display DEVICE ownership from ordinary copied-output bounds.

The completed client checkpoint is reused. Both requests have identical valid
in-range borrowed-slot metadata; only parent ownership changes. Cooperative
private metadata is arranged by this fixture, not an untrusted native caller.
"""
import argparse,hashlib,importlib.util,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('native_display',ROOT/'checks/gpu-display-native.py');n=importlib.util.module_from_spec(s);s.loader.exec_module(n)
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','checkpoint','client','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);e=n.base.Image(a.image);state=json.loads((a.checkpoint/'constructor.json').read_text());record=json.loads((a.client/'client.json').read_text());raw=(a.checkpoint/'session-first.bin').read_bytes();contract=(a.checkpoint/'contract-first.bin').read_bytes()
 assert state['image_sha256']==record['image_sha256']==hashlib.sha256(a.image.read_bytes()).hexdigest()
 assert state['session_sha256']==hashlib.sha256(raw).hexdigest() and state['contract_sha256']==hashlib.sha256(contract).hexdigest()
 m=n.Machine(e,a.image,raw,contract);m.u.mem_write(0x41000000,(a.client/'main-after-client.bin').read_bytes());bank=record['bank_address'];m.u.mem_write(bank,(a.client/'bank-after-client.bin').read_bytes());m.scheduler=2;m.task=m.get(m.app+m.af['handles']+16)
 service=m.app+m.af['gpu_service'];sl=e.layouts['pvr_gpu_service'];graphics=m.app+m.af['gpu_graphics'];gl=e.layouts['a733_gpu_graphics_memory'];source=graphics+gl['submission'];rl=e.layouts['pvr_render_service'];scratch=0x43e00000
 assert m.call('pvr_render_texture_layout_profile',1280,720,2,scratch)==1
 output=m.get(graphics+gl['payload_extent'])+m.get(graphics+gl['render']+e.layouts['pvr_render_requirements']['payload_bytes'],4)+m.get(scratch+e.layouts['pvr_render_texture_layout']['download'],4)
 fb=e.address('framebuffer');fl=e.layouts['hdmi_framebuffer'];pool=m.get(fb+fl['pool']);slot=m.get(fb+fl['lease']+e.layouts['device_memory_lease']['slot'],4);allocation=pool+e.layouts['device_memory_pool']['allocations']+slot*32;ownership=allocation+e.layouts['device_memory_allocation']['state']
 assert m.get(ownership,4)==2
 m.put(m.app+m.af['frames_ready'],0,4);m.put(m.app+m.af['copied'],0,4);m.put(service+sl['owner'],0,4);m.put(service+sl['phase'],3,4)
 for key,value,width in [('phase',9,4),('sequence',1,4),('texture_output_state',1,4),('texture_output_ticket',999,8),('texture_output_client',0,4),('texture_output_frame',1,4),('texture_output_offset',0,4),('texture_output_bytes',4,4)]:m.put(source+rl[key],value,width)
 m.u.mem_write(output,b'\xff\x00\x00\xff');before=bytes(m.u.mem_read(bank,4194304))
 assert m.call('gpu_display_bank_copy',0,1,0,output,4)==0
 assert bytes(m.u.mem_read(bank,4194304))==before and m.get(m.app+m.af['copied'],4)==0
 # Same valid request succeeds when the fixture restores pre-publication CPU
 # ownership, proving the earlier rejection came from the ownership branch.
 m.put(ownership,1,4)
 # The copy also extends frame 0's copied-output evidence, which the client
 # run closed; reopen it as the render task does before its first copy.
 evidence=n.variables(a.image,'/apps/gpu-display/observation.c')['digest'][0]
 with a.image.open('rb') as f:size=next(x['st_size'] for x in n.ELFFile(f).get_section_by_name('.symtab').iter_symbols() if x.name=='digest' and x['st_value']==evidence)
 m.u.mem_write(evidence,bytes(size//2));assert m.call('gpu_display_evidence_begin',0,m.call('gpu_display_inputs_get')+16+32)==1
 assert m.call('gpu_display_bank_copy',0,1,0,output,4)==1
 assert bytes(m.u.mem_read(bank+36864,2))==b'\x00\xf8' and m.get(m.app+m.af['copied'],4)==4
 (a.output/'retention.json').write_text(json.dumps(dict(passed=True,image_sha256=record['image_sha256'],scope=__doc__,physical_access=False,cases=['valid-borrow-refused-under-device-ownership','identical-valid-borrow-accepted-under-cpu-ownership']),indent=2)+'\n');print('PASS exact ownership rejection with valid CPU control')
if __name__=='__main__':main()
