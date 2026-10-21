#!/usr/bin/env python3
"""Run private DSI profile and application checks with explicit model authority."""
import argparse,hashlib,importlib.util,json,os,re,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--image',type=Path,required=True);p.add_argument('--resume',action='store_true');a=p.parse_args();out=a.output
 out.mkdir(parents=True,exist_ok=a.resume);commands=[]
 if (out/'commands.json').exists():commands=json.loads((out/'commands.json').read_text())
 def run(args):
  r=subprocess.run(list(map(str,args)),cwd=ROOT,capture_output=True,text=True);commands.append(dict(command=list(map(str,args)),returncode=r.returncode,stdout=r.stdout,stderr=r.stderr));(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');assert r.returncode==0,commands[-1];return r.stdout
 drivers=['drivers/display/'+n+'.c' for n in ('a733_display_native','a733_display_handoff','a733_display_pll','display_scanout','a733_dsi','a733_dsi_phy','a733_dsi_video','a733_tcon_lcd','a733_display_engine')]+['drivers/clock/a733_media_clock.c','drivers/clock/a733_clock_leases.c','drivers/gpu/a733_gpu_resources.c','drivers/iommu/a733_iommu.c','runtime/device_memory_pool.c']
 pmic=['drivers/power/a733_pmic.c','drivers/power/a733_rail_leases.c','drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c','drivers/storage/storage_digest.c']
 cc='/usr/bin/clang-21';flags=['-D_POSIX_C_SOURCE=200112L','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-missing-field-initializers','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Iinclude']
 sources=['checks/dsi-scanout-profile.c','apps/dsi-scanout/profile.c',*[v for v in drivers if v != 'drivers/display/a733_display_native.c'],'drivers/pinctrl/a733_pins.c','drivers/pinctrl/a733_pin_tables.c']
 if not (out/'profile-check').exists():run([cc,*flags,*sources,'-o',out/'profile-check'])
 factors=int(run([out/'profile-check','--factors']))
 hid=module('dsi_image',ROOT/'checks/usb-host-hid-native.py');image=hid.Image(a.image);layout=image.layouts['a733_board_session'];profile=module('dsi_values',ROOT/'apps/dsi-scanout/profile.py')
 baseline=Path.home()/'.cache/sunstoneos/evidence/board-supervised-20261017/batch1/session-fixture.bin';board=bytearray(baseline.read_bytes())
 def put(k,value):
  off=layout[k];board[off:off+len(value)]=value
 put('workload',struct.pack('<I',2));put('fabric',struct.pack('<3I',0x03000003,0x83000001,0x81000001));put('fabric_hz',struct.pack('<3I',150000000,300000000,300000000));put('pmic_pins',bytes([2,1,1]*2))
 buildid=next(data[image.address('a733_smp_build_id')-start:image.address('a733_smp_build_id')-start+32] for start,data in image.parts if start<=image.address('a733_smp_build_id')<start+len(data));put('image',buildid)
 (out/'build-id.c').write_text('const unsigned char a733_smp_build_id[32]={'+','.join(map(str,buildid))+'};\n')
 facts=[]
 for name in ('initial','memory','electrical','panel'):
  data=json.dumps(dict(schema='DSI-MODEL-ONLY',kind=name,physical_access_performed=False,launch_authority=False,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest()),sort_keys=True).encode();(out/(name+'-MODEL-ONLY.json')).write_bytes(data);facts.append(hashlib.sha256(data).digest())
 put('qualification',facts[0]);put('recovery',facts[0])
 for name,w,h,pixel,hs,ls,timing in [('720p',1280,720,80000000,480000000,80000000,(40,100,80,5,20,5)),('1080p',1920,1080,120000000,720000000,120000000,(44,148,88,5,36,4))]:
  values=dict(zip(profile.VALUES,(pixel,hs,ls,w,h,*timing,4,0,0,pixel,w*4,480000000,80000000)))
  pins=[dict(bank='D',pin=i,expected=[15,0,2],desired=[4,0,2],output=0,role='lane') for i in range(10,20)]
  prof=dict(schema='a733-dsi-panel-values-v1',ids=dict(zip(profile.IDS,(31,1,2,3,4,5,6,7,8,9,2))),values=values,initial_words=[factors|0x00800080,0x00120381,0x01372818,0x80000000,0x1f123456,0x01004000,0x172],timeout_us=50000000,seed=0,fetch_padding=4096,supplies=[dict(rail=1,uv=800000)],pins=pins,steps=[dict(op='packet',phase='before-video',type=5,hex='00'),dict(op='delay',phase='before-video',us=1000)])
  (out/(name+'-MODEL-ONLY-profile.json')).write_text(json.dumps(prof,indent=2)+'\n');raw=profile.encode(board,prof,facts);(out/(name+'-session.bin')).write_bytes(raw)
  if not (out/(name+'-profile-pass.txt')).exists():(out/(name+'-profile-pass.txt')).write_text(run([out/'profile-check',out/(name+'-session.bin')]))
 fr=Path.home()/'.cache/sunstoneos/sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2'
 tcp=Path.home()/'.cache/sunstoneos/sources/radio/stacks/FreeRTOS-Plus-TCP-b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source'
 includes=['-include','apps/board-single-core/FreeRTOSConfig.h','-Inetwork/include','-I'+str(tcp/'include'),'-I'+str(tcp/'portable/Compiler/GCC'),'-Iapps/dsi-scanout','-Iapps/board-single-core','-Iplatform/a733-storage-update','-I'+str(fr/'include'),'-I'+str(fr/'portable/GCC/ARM_AARCH64_SRE')]
 if not (out/'native-check').exists():run([cc,*flags,*includes,'-DSUNSTONE_BOARD_SUPERVISED=1','-DSUNSTONE_BOARD_APPLICATION=1','-DGUEST=1',out/'build-id.c','checks/dsi-scanout-native.c','apps/dsi-scanout/profile.c','apps/dsi-scanout/main.c','apps/dsi-scanout/native.c',*drivers,*pmic,'-o',out/'native-check'])
 for name,mode in [('720p','success'),('1080p','success'),('720p','mapping'),('720p','iommu'),('720p','late')]:
  result=out/(name+'-'+mode+'-pass.txt')
  if not result.exists():result.write_text(run([out/'native-check',out/(name+'-session.bin'),mode,out/(name+'-observation.bin')]))
 (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,scope='Actual application/media/PLL/IOMMU/PMIC/pins/allocator/scanout, modeled MMIO/clock/RTOS and explicit nonphysical panel facts',image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest()),indent=2)+'\n')
if __name__=='__main__':main()
