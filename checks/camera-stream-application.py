#!/usr/bin/env python3
"""Check finite stream scheduling and USB output using an already built camera profile."""
import argparse, hashlib, importlib.util, json, re, shlex, struct, subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
p.add_argument('--modes',action='store_true',help='Actual selected-size owner, USB and fragmented full transport')
p.add_argument('--output-only',action='store_true',help='Resume selected native output and host capture cases')
p.add_argument('--admission-only',action='store_true',help='Check compiled connector admission only')
p.add_argument('--response-dir',type=Path,help='Check only host collection from previously saved native responses')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
flags=(a.build/'CMakeFiles/sunstone-a733-camera.dir/flags.make').read_text()
def flag(name):return shlex.split(re.search(r'^'+name+r' = (.*)$',flags,re.M)[1])
base=['/usr/bin/clang-21','--target=aarch64-none-elf',*flag('C_DEFINES'),*flag('C_INCLUDES'),*flag('C_FLAGS')]
assert '-DSUNSTONE_CAMERA_SUPERVISED=1' in base
is_stream='-DSUNSTONE_CAMERA_STREAM=1' in base
assert is_stream or a.modes
spec=importlib.util.spec_from_file_location('camera_profile_prepare',ROOT/'apps/camera/prepare.py')
prep=importlib.util.module_from_spec(spec);spec.loader.exec_module(prep)
profile=prep.image_data((a.build/'sunstone-a733-camera.elf').read_bytes())[3]['image_profile']
width,height=profile['width'],profile['height'];plane=width*height;total=2*plane
linker=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'
result={'physical_access':False,'cases':{},'image_sha256':hashlib.sha256((a.build/'sunstone-a733-camera.elf').read_bytes()).hexdigest()}
for label,units,cases in ([(('stream-owner' if is_stream else 'still-owner'),['checks/camera-application.c'],4 if is_stream else 2),
                         ('selected-output',['checks/camera-application-output.c','apps/camera/capture.c','drivers/storage/storage_digest.c'],4)] if a.modes else [] if a.response_dir else
                        [('connector-admission',['checks/camera-supervised-admission.c','apps/camera/admission.c','drivers/storage/storage_digest.c'],1),
                         ('ordinary-admission',['checks/camera-application-admission.c','apps/camera/admission.c','drivers/storage/storage_digest.c'],1)] if a.admission_only else [('stream-owner',['checks/camera-application.c'],4),
                         ('stream-output',['checks/camera-application-output.c','apps/camera/capture.c','drivers/storage/storage_digest.c'],3),
                         ('still-output',['checks/camera-application-output.c','apps/camera/capture.c','drivers/storage/storage_digest.c'],3)]):
    if a.output_only and label.endswith('owner'):continue
    selected=base if label not in ('still-output','ordinary-admission') else [x for x in base if x not in ('-DSUNSTONE_CAMERA_STREAM=1','-DSUNSTONE_CAMERA_SUPERVISED=1')]
    elf_path=a.output/(label+'.elf')
    cmd=[*selected,*[str(ROOT/x) for x in units],str(ROOT/'runtime/memory.c'),'-nostdlib',f'-fuse-ld={linker}',
         '-Wl,-Ttext=0x41000000','-Wl,-e,camera_check_run','-Wl,--gc-sections','-o',str(elf_path)]
    run=subprocess.run(cmd,capture_output=True,text=True)
    (a.output/(label+'.log')).write_text(run.stdout+run.stderr);run.check_returncode()
    with elf_path.open('rb') as stream:
        elf=ELFFile(stream)
        segments=[(s['p_vaddr'],s.data(),s['p_memsz']) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
        symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    for case in range(cases):
        uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM);pages=set()
        for address,data,size in segments:
            for page in range(address&~4095,(address+size+4095)&~4095,4096):
                if page not in pages:uc.mem_map(page,4096);pages.add(page)
            uc.mem_write(address,data)
        if label in ('connector-admission','ordinary-admission'):uc.mem_map(0x41f00000,0x1000)
        uc.mem_map(0x50000000,0x10000);uc.reg_write(UC_ARM64_REG_SP,0x5000f000);uc.reg_write(UC_ARM64_REG_X0,case)
        uc.hook_add(UC_HOOK_INTR,lambda m,i,c:m.emu_stop())
        uc.emu_start(symbols['camera_check_run'],0,count=max(5000000,total*16))
        value=int.from_bytes(uc.mem_read(symbols['camera_check_result'],4),'little')
        result['cases'][f'{label}-{case}']={'passed':value==1,'result':value,'elf_sha256':hashlib.sha256(elf_path.read_bytes()).hexdigest()}
        (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        assert value==1,(label,case,value)
        if label in ('stream-output','selected-output') and case==0:
            frame=bytes(uc.mem_read(symbols['wire'],(80 if is_stream else 72)+4096))
            size=int.from_bytes(uc.mem_read(symbols['app']+16,8),'little')
            session=bytes(uc.mem_read(symbols['app'],size))
            (a.output/'native-frame.bin').write_bytes(frame);(a.output/'native-session.bin').write_bytes(session)
            spec=importlib.util.spec_from_file_location('camera_decoder',ROOT/'apps/camera/observe.py')
            decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
            decoded=decoder.decode(frame,session,1,plane-1,4096,profile=profile)
            assert decoded['state']==(4 if is_stream else 2) and not decoded['drain_proved']
            if is_stream:assert decoded['sensor_stopped'] and decoded['stream_retained']
            assert decoded['payload']==bytes(((plane-1+i)*17)&255 for i in range(4096))
if a.admission_only:
    result['source_sha256']={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in
        ('checks/camera-stream-application.py','checks/camera-supervised-admission.c',
         'checks/camera-application-admission.c','apps/camera/admission.c','include/a733_camera_native.h')}
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS compiled CAM1/CAM2 session admission and authenticated selector changes')
    raise SystemExit(0)
native=a.response_dir or a.output
frame=(native/'native-frame.bin').read_bytes();session=(native/'native-session.bin').read_bytes()
spec=importlib.util.spec_from_file_location('camera_stream_host',ROOT/'tools/check-a733-camera-supervised.py')
host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
pixels=bytes((i*17)&255 for i in range(total))
def exchange(request,timeout):
    op,offset,count=struct.unpack('<III',request)
    header=bytearray(frame[:80 if is_stream else 72]);struct.pack_into('<II',header,12,offset,count if op==1 else 0)
    return bytes(header)+(pixels[offset:offset+count] if op==1 else b'')
record={}
host.capture(exchange,session,a.output,record,lambda:None,1.,now=lambda:0.,pause=lambda t:None,profile=profile)
assert record['camera_result']==('copied-frame-stream-retained' if is_stream else 'observed-success')
if is_stream:assert record['camera_stream']=={'sensor_stopped':True,'drain_proved':False,'stream_retained':True}
assert (a.output/f'frame-{width}x{height}-nv16.bin').read_bytes()==pixels
for value in ((0,2,7) if is_stream else ()):
    invalid=bytearray(frame);struct.pack_into('<I',invalid,72,value)
    try:host.decoder.decode(invalid,session,1,plane-1,4096,profile=profile)
    except ValueError:pass
    else:raise AssertionError('Invalid stream retention status accepted')
if a.modes:
    class Device:
        pending=b''; reads=0; writes=0
        def write(self,endpoint,request,timeout):
            assert endpoint==2 and not self.pending;self.writes+=1
            self.pending=exchange(request,timeout);self.part=0;return len(request)
        def read(self,endpoint,count,timeout):
            assert endpoint==0x81 and self.pending;self.reads+=1
            # Split inside the version/size fields and across the 72/80-byte boundary.
            n=min(count,len(self.pending),[5,14,53,8,4096][min(self.part,4)]);self.part+=1
            value=self.pending[:n];self.pending=self.pending[n:];return value
    device=Device();transport=a.output/'transport';transport.mkdir();transport_record={}
    host.capture_device(device,lambda:None,host.time.monotonic()+60.,session,transport,transport_record,lambda:None,profile=profile)
    assert (transport/f'frame-{width}x{height}-nv16.bin').read_bytes()==pixels
    assert device.writes==1+(total+4095)//4096
    result['cases']['fragmented-transport']={'passed':True,'requests':device.writes,'reads':device.reads,'bytes':total}
result['cases']['host-collection']={'passed':True,'response_sha256':hashlib.sha256(frame).hexdigest()}
result['source_sha256']={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in
    ('checks/camera-stream-application.py','checks/camera-application.c','checks/camera-application-output.c',
     'apps/camera/capture.c','apps/camera/usb0.c','apps/camera/observe.py','tools/check-a733-camera-supervised.py')}
(a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS stream scheduling, retained partial/final copies and actual USB framing; hardware and endpoint effects modeled')
