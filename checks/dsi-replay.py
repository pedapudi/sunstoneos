#!/usr/bin/env python3
"""Check the affected two-frame native upload/replay path with model authority."""
import argparse,hashlib,importlib.util,json,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','output'):p.add_argument('--'+name,type=Path,required=True)
 p.add_argument('--resume',action='store_true');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=a.resume);out=a.output;commands=[]
 if (out/'commands.json').exists():commands=json.loads((out/'commands.json').read_text())
 def run(args):
  r=subprocess.run(list(map(str,args)),cwd=ROOT,capture_output=True,text=True);commands.append(dict(command=list(map(str,args)),returncode=r.returncode,stdout=r.stdout,stderr=r.stderr));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');assert not r.returncode,commands[-1];return r.stdout
 image=module('replay_image',ROOT/'checks/usb-host-hid-native.py').Image(a.image);profile=module('replay_values',ROOT/'apps/dsi-scanout/profile.py');build_id=next(data[image.address('a733_smp_build_id')-start:image.address('a733_smp_build_id')-start+32] for start,data in image.parts if start<=image.address('a733_smp_build_id')<start+len(data));(out/'build-id.c').write_text('const unsigned char a733_smp_build_id[32]={'+','.join(map(str,build_id))+'};\n')
 old=Path.home()/'.cache/sunstoneos/evidence/dsi-scanout-application-20261019'
 for mode in ('720p','1080p'):
  path=out/mode
  if path.exists():continue
  path.mkdir();raw=bytearray((old/'linked-continued'/(mode+'-session.bin')).read_bytes());raw[48:80]=build_id;v=struct.unpack_from('<32I',raw,832);w,h,pitch=v[3],v[4],v[15];frames=[]
  for slot in range(2):
   row=b''.join(struct.pack('<I',((x*17+slot*73)&255)|(((x*7+slot*11)&255)<<8)|(((x+slot*97)&255)<<16)) for x in range(w))+bytes(pitch-4*w)
   frames.append(row*h+bytes(profile.frame_bytes(raw)-pitch*h))
  raw=profile.replay_session(raw,frames);(path/'session.bin').write_bytes(raw)
  for slot,data in enumerate(frames):(path/f'upload{slot}.bin').write_bytes(profile.upload_bytes(raw,slot,data))
  for sequence,slot in enumerate((0,1,0),1):(path/f'present{sequence}.bin').write_bytes(profile.input_header(raw,2,slot,sequence))
  (path/'producer.json').write_text(json.dumps(dict(physical_access=False,launch_authority=False,source='Actual profile.replay_session/upload_bytes/input_header producer; explicit MODEL_ONLY initial/electrical/panel facts from accepted native composition',bank_bytes=2*profile.frame_bytes(raw)+8192,table_bytes=32768,frame_sha256=[hashlib.sha256(v).hexdigest() for v in frames]),indent=2)+'\n')
 oldcommands=json.loads((old/'application/commands.json').read_text());cmd=next(r['command'] for r in oldcommands if not r['returncode'] and any(str(x).endswith('/native-check') for x in r['command']) and '-o' in r['command']);cmd=[str(out/'build-id.c') if x.endswith('/application/build-id.c') else 'checks/dsi-replay-native.c' if x=='checks/dsi-scanout-native.c' else x.replace('/home/sunil/git/sunstoneos-worktrees/dsi-scanout-app',str(ROOT)) for x in cmd];cmd[cmd.index('-o')+1]=str(out/'native-check');cmd+=['apps/dsi-scanout/input.c','-Wno-missing-field-initializers']
 if not (out/'native-check').exists():
  deps=cmd[:];i=deps.index('-o');del deps[i:i+2];deps+=['-M'];data=run(deps);(out/'host-inputs.d').write_text(data)
  run(cmd)
 for mode,fault in [('720p','success'),('1080p','success'),('720p','readback')]:
  case=out/(mode+'-'+fault)
  if (case/'result.json').exists():continue
  case.mkdir(exist_ok=True);path=out/mode;args=[out/'native-check',path/'session.bin',path/'upload0.bin',path/'upload1.bin',path/'present1.bin',path/'present2.bin',path/'present3.bin',case,fault]
  result=run(args);(case/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,result=result),indent=2)+'\n')
 (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),scope='Actual copied input and native application plus media/PLL/PMIC/pins/allocator/IOMMU/DE/DSI producers. Modeled MMIO/RTOS/source electrical/panel facts; translated canonical RCQ header/data consumed by model. No old framebuffer recycling, panel or hardware qualification.'),indent=2)+'\n')
if __name__=='__main__':main()
