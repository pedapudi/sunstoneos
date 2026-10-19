#!/usr/bin/env python3
"""Build the real DSI SRAM launcher and run only its changed contract cases."""
import argparse,hashlib,importlib.util,json,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ('image','session','output'):p.add_argument('--'+name,type=Path,required=True)
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False);prep=module('dsi_launch_prep',ROOT/'apps/dsi-scanout/prepare.py');image=module('dsi_launch_image',ROOT/'checks/usb-host-hid-native.py').Image(a.image)
 prep.build_launcher(a.output,Path('/usr/bin/clang-21'),Path('/home/sunil/.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld'))
 raw=a.session.read_bytes();prep.session_check(raw);request,contract=prep.request_bytes(image.address('a733_smp_build_id'),raw[48:80],raw,*struct.unpack_from('<3Q',raw,24),100)
 for name,data in [('session.bin',raw),('build-id.bin',raw[48:80]),('request.bin',request),('contract.bin',contract)]:(a.output/name).write_bytes(data)
 (a.output/'prepared.json').write_text(json.dumps(dict(physical_access=False,segments=[dict(address=0x41f00000,name='session.bin'),dict(address=image.address('a733_smp_build_id'),name='build-id.bin')]),indent=2)+'\n')
 # Reuse the accepted instruction fixture's MMIO/counter model and checks.
 # Only the selected contract constants and the new application cases change.
 source=(ROOT/'checks/a733-smp-supervised-launch.py').read_text().replace('ticks[0]+480000000','ticks[0]+1440000000').replace('0x4153373353554333','0x4153373344534331').replace('reason in (1,2,3,4,5,6,13)','reason in (1,2,3,4,5,6,13,14)')
 start=source.index('    if args.frequency_only:');end=source.index('    result =',start)
 source=source[:start]+'''    cases=[check('DSI288 extended contract with60second retention hands off',12),
      check('wrong DSI profile refused',14,mutation=(0x41f00000+592,0)),
      check('wrong DSI profile extent refused',14,mutation=(0x41f00000+608,592)),
      check('wrong workload refused',14,mutation=(0x41f00000+400,0)),
      check('live timer claims refused',14,mutation=(0x41f00000+112,1)),
      check('wrong full WB extent refused',3,mutation=(CONTRACT+192,0x78100000)),
      check('entry digest mismatch refused',3,mutation=(CONTRACT+256,0))]
'''+source[end:]
 checker=a.output/'selected-launch-check.py';checker.write_text(source)
 r=subprocess.run([sys.executable,str(checker),'--bundle',str(a.output),'--output',str(a.output/'result.json')],capture_output=True,text=True)
 (a.output/'execution.json').write_text(json.dumps(dict(command=r.args,returncode=r.returncode,stdout=r.stdout,stderr=r.stderr,source=str(ROOT/'checks/a733-smp-supervised-launch.py'),source_sha256=hashlib.sha256((ROOT/'checks/a733-smp-supervised-launch.py').read_bytes()).hexdigest(),physical_access=False),indent=2)+'\n');assert r.returncode==0,r.stderr
if __name__=='__main__':main()
