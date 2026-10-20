#!/usr/bin/env python3
"""Check the listener host path with the actual compiled USB result producer."""
import argparse,hashlib,importlib.util,json,shlex,subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_SP,UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--native-build',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
 unit=next(c for c in json.loads((a.native_build/'compile_commands.json').read_text()) if c['file'].endswith('/apps/board/client.c'))
 flags=shlex.split(unit['command']);flags=flags[:flags.index('-o')]
 linker=Path.home()/'.cache/sunstoneos/host/usr/bin/ld.lld-21';fixture=a.output/'usb-producer.elf'
 command=flags+[str(ROOT/n) for n in ['checks/board-application-output.c','drivers/storage/storage_digest.c','runtime/memory.c','runtime/memmove.c']]+['-nostdlib',f'-fuse-ld={linker}','-Wl,-Ttext=0x41000000','-Wl,-e,board_check_run','-Wl,--gc-sections','-o',str(fixture)]
 result=subprocess.run(command,capture_output=True,text=True);(a.output/'build.log').write_text(result.stdout+result.stderr);result.check_returncode()
 (a.output/'command.json').write_text(json.dumps(command,indent=2)+'\n')
 with fixture.open('rb') as f:
  elf=ELFFile(f);segments=[(s['p_vaddr'],bytes(s.data()),s['p_memsz']) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
  symbols={s.name:int(s['st_value']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
 host=module('net_listener_capture',ROOT/'tools/check-a733-board-supervised.py');prepare=module('net_listener_prepare',ROOT/'apps/board/prepare.py')
 cases={}
 for mode,name in [(3,'nominal'),(4,'pending'),(5,'failed')]:
  dest=a.output/name;dest.mkdir();cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);pages=set()
  for at,data,size in segments:
   for page in range(at&~4095,(at+size+4095)&~4095,4096):
    if page not in pages:cpu.mem_map(page,4096);pages.add(page)
   cpu.mem_write(at,data)
  cpu.mem_map(0x50000000,0x10000);cpu.reg_write(UC_ARM64_REG_SP,0x5000f000);cpu.reg_write(UC_ARM64_REG_X0,mode)
  cpu.hook_add(UC_HOOK_INTR,lambda m,*_:m.emu_stop());cpu.emu_start(symbols['board_check_run'],0,count=2000000)
  assert int.from_bytes(cpu.mem_read(symbols['board_check_result'],4),'little')==1
  frame=bytes(cpu.mem_read(symbols['board_check_frame'],72));session=bytes(cpu.mem_read(symbols['board'],592));profile=bytes(cpu.mem_read(symbols['profile'],1872))
  for n,b in [('producer.bin',frame),('session.bin',session),('profile.bin',profile)]: (dest/n).write_bytes(b)
  prepare.profile_spans(profile,session);assert prepare.session_check(session)==3
  clock=[0.]
  def now():clock[0]+=.001;return clock[0]
  def pause(n):clock[0]+=max(.001,n)
  record={}
  try:value=host.capture(lambda *_:frame,session,profile,dest,record,lambda:None,.1,now=now,pause=pause)
  except (ValueError,RuntimeError):assert name=='pending' and record.get('board_result')!='observed-success'
  else:assert record['board_result']==('observed-success' if name=='nominal' else 'observed-failure')
  if name=='nominal':assert value['radio_tcp_mode']=='listener' and value['verified_radio_bytes']==5 and not value['tcp_wire_drain_established']
  cases[name]=True
  if name=='nominal':
   for bad_name,offset,value in [('legacy-mode',8,4),('mode-zero',1338,0)]:
    raw=bytearray(profile);raw[offset:offset+(4 if offset==8 else 2)]=value.to_bytes(4 if offset==8 else 2,'little')
    bound=bytearray(session);bound[368:400]=hashlib.sha256(raw).digest();packet=bytearray(frame);packet[40:72]=bound[368:400]
    for fn,args in [(prepare.profile_spans,(bytes(raw),bytes(bound))),(host.decoder.decode,(bytes(packet),bytes(bound),bytes(raw)))]:
     try:fn(*args)
     except (ValueError,RuntimeError):pass
     else:raise AssertionError(bad_name)
    cases[bad_name]=True
 (a.output/'result.json').write_text(json.dumps({'passed':True,'cases':cases,'fixture_sha256':sha(fixture),'native_sha256':sha(a.native_build/'sunstone-a733-board.elf'),'scope':'Actual compiled USB result producer and existing host capture; radio/socket completion observations explicitly modeled. No hardware access.'},indent=2)+'\n')
 print('PASS inbound host: actual producer/profile binding, terminal success/failure, pending refusal and opt-in admission')
if __name__=='__main__':main()
