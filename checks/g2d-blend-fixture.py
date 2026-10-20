#!/usr/bin/env python3
"""Produce selected MODEL-only blend entry inputs from the accepted G2D profile.

This is not a complete physical admission or an allocator proof. The unchanged
producer authenticates the selected ELF, operation and source; the existing
launcher builder consumes explicit cached tool paths once.
"""
import argparse,hashlib,importlib.util,json,os,shutil,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','profile','output','clang','lld'):p.add_argument('--'+n,type=Path,required=True)
 p.add_argument('--reuse-launcher',type=Path)
 a=p.parse_args()
 for tool in (a.clang,a.lld):assert tool.is_file() and os.access(tool,os.X_OK),tool
 old=(a.profile/'session.bin').read_bytes();raw=(a.profile/'input.argb').read_bytes();assert len(old)==904 and len(raw)==16384
 assert hashlib.sha256(raw).digest()==old[768:800]
 producer=load('g2d_blend_producer',ROOT/'apps/g2d/producer.py');prep=producer.preparation();image=a.image.read_bytes();ih=hashlib.sha256(image).hexdigest();_,address,identity=prep._image_parts(image)
 a.output.mkdir(parents=True,exist_ok=False)
 target=dict(identity='g2d-source-over-solid-model',port='no-physical-port',image_sha256=ih,modeled=True,physical_authority=False)
 def record(name,value):
  data=(json.dumps(value,sort_keys=True,indent=2)+'\n').encode();(a.output/name).write_bytes(data);return hashlib.sha256(data).digest()
 note=record('MODEL-NOT-PHYSICAL.json',dict(**target,purpose=__doc__,source_profile_sha256=hashlib.sha256(old).hexdigest()))
 board=bytearray(old[:592]);board[48:80]=identity;ids=struct.unpack('<3Q',hashlib.sha256(image+b'g2d-source-over-solid-model').digest()[:24]);assert all(ids);struct.pack_into('<3Q',board,24,*ids)
 for off in (80,216,248,280):board[off:off+32]=note
 facts={}
 for name in ('initial','memory','electrical','supervision'):
  value=json.loads((a.profile/(name+'.json')).read_bytes());value.update(target);record(name+'.json',value);facts[name]=(a.output/(name+'.json')).read_bytes()
 owner,authority,client=struct.unpack_from('<3Q',old,616)
 session=producer.extend(bytes(board),raw,owner=owner,authority=authority,client=client,**facts,width=60,height=64,stride=256,operation=3,dx=9,dy=17,w=31,h=23,sx=5,sy=7,color=0xff315a93)
 prep.embedded_reference(image,session);request,contract=prep.request_bytes(address,identity,session,*ids)
 for name,data in [('image.elf',image),('session.bin',session),('input.argb',raw),('request.bin',request),('contract.bin',contract),('upload.bin',prep.upload_bytes(raw,session))]:(a.output/name).write_bytes(data)
 if a.reuse_launcher:
  inputs={'launch.c':ROOT/'apps/g2d/launch.c','launch.S':ROOT/'platform/a733-smp-observe/launch.S','launch.ld':ROOT/'platform/a733-smp-observe/launch.ld','boot.h':ROOT/'platform/a733-smp-cached/boot.h','a733_g2d_application.h':ROOT/'include/a733_g2d_application.h','a733_board_application.h':ROOT/'include/a733_board_application.h','base-supervised-launch.c':ROOT/'platform/a733-smp-supervised/launch.c'}
  assert all((a.reuse_launcher/n).read_bytes()==path.read_bytes() for n,path in inputs.items())
  names=[*inputs,'g2d-launch-base.c','launch.bin','launch.elf','launch.c.o','launch.S.o','launch.c.d','launch.S.d','launcher-build.log','launcher-commands.json']
  for name in names:shutil.copy2(a.reuse_launcher/name,a.output/name)
 else:prep.build_launcher(a.output,str(a.clang),str(a.lld))
 record('provenance.json',dict(**target,build_id=identity.hex(),session_sha256=hashlib.sha256(session).hexdigest(),source_sha256=hashlib.sha256(raw).hexdigest(),launcher_sha256=hashlib.sha256((a.output/'launch.bin').read_bytes()).hexdigest(),profile=str(a.profile),launcher_reused=bool(a.reuse_launcher),profile_geometry=list(struct.unpack_from('<12I',old,856)),geometry=list(struct.unpack_from('<12I',session,856)),scope=__doc__))
if __name__=='__main__':main()
