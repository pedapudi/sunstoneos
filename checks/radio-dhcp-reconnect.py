#!/usr/bin/env python3
"""One image and only the new IP-task DHCP reconnect boundary cases."""
import argparse,hashlib,importlib.util,json,shlex,shutil,socket,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
ACCEPTED=Path.home()/'.cache/sunstoneos/evidence/radio-reconnect-20261020/native'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,v):p.write_text(json.dumps(v,indent=2)+'\n')
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--phase',choices=['build','run'],required=True)
 p.add_argument('--case',choices=['healthy','timeout']);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args()
 out=a.evidence.resolve();out.mkdir(parents=True,exist_ok=True)
 cache={}
 for line in (ACCEPTED/'build/CMakeCache.txt').read_text().splitlines():
  if '=' in line and ':' in line.split('=',1)[0]:cache[line.split(':',1)[0]]=line.split('=',1)[1]
 selected=['FREERTOS_DIR','SUNSTONE_TCP_DIR','SUNSTONE_RSN_DIR','SUNSTONE_NETWORK_C_HEADERS','TINYUSB_DIR','SUNSTONE_LLD']
 # Resolve every selected provider/build input before configuring anything.
 required=[Path(cache[k]) for k in selected]+[Path('/usr/bin/clang-21'),Path.home()/'.cache/sunstoneos/host/usr/bin/qemu-system-aarch64']
 assert all(x.exists() for x in required),[str(x) for x in required if not x.exists()]
 qemu=required[-1];pins=module('radio_pins',ROOT/'checks/radio-network.py');pins.verify(Path(cache['SUNSTONE_TCP_DIR']))
 if a.phase=='build':
  assert not (out/'build-result.json').exists()
  project=out/'project';project.mkdir();(project/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.24)
project(dhcp_reconnect C ASM)
add_subdirectory("{ROOT}" base)
get_target_property(units radio-network-model SOURCES)
list(FILTER units EXCLUDE REGEX "/checks/radio-network[.]c$")
set_property(TARGET radio-network-model PROPERTY SOURCES "${{units}}")
target_sources(radio-network-model PRIVATE "{ROOT}/checks/radio-dhcp-reconnect.c")
''')
  toolchain=out/'toolchain.cmake';shutil.copyfile(ACCEPTED/'toolchain.cmake',toolchain)
  # The repository toolchain also defines the path-map helpers its CMakeLists uses.
  with toolchain.open('a') as f:f.write('set(SUNSTONE_LLD /home/sunil/.cache/sunstoneos/host/usr/bin/ld.lld-21)\ninclude("'+str(ROOT)+'/cmake/aarch64-clang.cmake")\n')
  record={'physical_access':False,'commands':[],'accepted_toolchain':str(ACCEPTED/'toolchain.cmake')}
  commands=[['cmake','-G',cache['CMAKE_GENERATOR'],'-S',str(project),'-B',str(out/'build'),'-DCMAKE_TOOLCHAIN_FILE='+str(toolchain),'-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']+['-D'+k+'='+cache[k] for k in selected],['cmake','--build',str(out/'build'),'--target','radio-network-model','-j4']]
  for command in commands:
   r=subprocess.run(command,capture_output=True,text=True);record['commands'].append({'command':command,'returncode':r.returncode,'stdout':r.stdout,'stderr':r.stderr});save(out/'build-result.json',record)
   assert r.returncode==0,r.stdout[-2000:]+r.stderr[-4000:]
  elf=out/'build/base/radio-network-model.elf';record.update(elf=str(elf),elf_sha256=sha(elf));save(out/'build-result.json',record)
  collector=module('current_collector',ROOT/'tools/evidence-snapshot.py')
  deps,commands,objects=collector.collect_dependencies(out/'build/base','radio-network-model')
  for name,row in objects.items():row['sha256']=sha(name)
  save(out/'compiled-closure.json',{'elf_sha256':sha(elf),'inputs':deps,'objects':objects,'commands':commands,'collector':{'path':str(ROOT/'tools/evidence-snapshot.py'),'sha256':sha(ROOT/'tools/evidence-snapshot.py')}})
  print(elf);return
 assert a.case;record=json.loads((out/'build-result.json').read_text());elf=Path(record['elf']);assert sha(elf)==record['elf_sha256']
 closure=json.loads((out/'compiled-closure.json').read_text())
 for name,h in closure['inputs'].items():assert sha(name)==h,name
 case=out/a.case;case.mkdir();symbols={}
 required={'reconnect_checkpoint_gate','reconnect_observation','net','transport','wifi','security','supplicant','client_done','client_ready','transaction','discovers','acknowledgements','stale_injected','silent_resume','platform_panic'}
 for line in subprocess.check_output(['nm','-S',str(elf)],text=True).splitlines():
  c=line.split()
  if len(c)==4 and (c[3] in required or c[3]=='__assert_func'):
   assert c[3] not in symbols,c[3];symbols[c[3]]=(int(c[0],16),int(c[1],16),c[2])
 assert required<=set(symbols),required-set(symbols)
 disk=case/'runtime.qcow2';source_checkpoint=None
 if a.case=='healthy':subprocess.run(['qemu-img','create','-f','qcow2',str(disk),'16M'],check=True,capture_output=True)
 else:
  prior=json.loads((out/'healthy/result.json').read_text());assert prior['returncode']==0 and prior['elf_sha256']==sha(elf)
  source=out/'healthy/runtime.qcow2';assert sha(source)==prior['disk_sha256'];shutil.copyfile(source,disk)
  source_checkpoint={'path':str(source),'sha256':sha(source),'snapshot':'before-offline','record_sha256':sha(out/'healthy/before-offline.json')}

 qmp_path=case/'qmp.sock';gdb_path=case/'gdb.sock';assert max(len(str(qmp_path)),len(str(gdb_path)))<108
 command=[str(qemu),'-M','virt,gic-version=3,secure=off,virtualization=off','-cpu','cortex-a53','-m','128M','-nographic','-nic','none','-monitor','none','-semihosting-config','enable=on,target=native,arg='+a.case,'-kernel',str(elf),'-qmp',f'unix:{qmp_path},server=on,wait=off','-gdb',f'unix:{gdb_path},server=on,wait=off','-drive',f'if=none,id=checkpoint,file={disk},format=qcow2','-S']
 if source_checkpoint:command+=['-loadvm','before-offline']
 journal={'source_checkpoint':source_checkpoint,'command':command,'elf_sha256':sha(elf),'symbols':symbols,'runner_sha256':sha(__file__)}
 packets=module('accepted_packets',ROOT/'checks/radio-reconnect-wpa.py')
 with (case/'stdout.log').open('w') as stdout,(case/'stderr.log').open('w') as stderr:
  process=subprocess.Popen(command,stdout=stdout,stderr=stderr)
  try:
   for _ in range(100):
    if qmp_path.exists():break
    assert process.poll() is None;time.sleep(.01)
   monitor=socket.socket(socket.AF_UNIX);monitor.settimeout(10);monitor.connect(str(qmp_path));f=monitor.makefile('rwb',buffering=0);json.loads(f.readline())
   def qmp(name,args=None):
    f.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode())
    while True:
     answer=json.loads(f.readline())
     if 'return' in answer or 'error' in answer:return answer
   qmp('qmp_capabilities');debugger=socket.socket(socket.AF_UNIX);debugger.settimeout(10);debugger.connect(str(gdb_path))
   def packet(c):return packets.packet(debugger,c)
   for name in ['__assert_func','platform_panic']:
    if name in symbols:assert packet(f'Z0,{symbols[name][0]:x},4')=='OK'
   def capture(tag):
    state=qmp('query-status')['return'];assert not state['running'],state
    rows={}
    for name,(address,size,kind) in symbols.items():
     if kind.lower() not in ['b','d']:continue
     raw=b''.join(bytes.fromhex(packet(f'm{address+off:x},{min(1024,size-off):x}')) for off in range(0,size,1024))
     path=case/(tag+'-'+name+'.bin');path.write_bytes(raw);rows[name]={'path':str(path),'sha256':sha(path)}
    result={'status':state,'registers':qmp('human-monitor-command',{'command-line':'info registers'}),'memory':rows,'savevm':qmp('human-monitor-command',{'command-line':'savevm '+tag})}
    assert result['savevm'].get('return')=='';save(case/(tag+'.json'),result);return result
   if not source_checkpoint:
    qmp('cont');deadline=time.monotonic()+45
    while 'CHECKPOINT DHCP before offline' not in (case/'stdout.log').read_text():
     assert process.poll() is None,'Exit before changed boundary'
     assert qmp('query-status')['return']['status']!='debug','Fixture preflight assertion'
     assert time.monotonic()<deadline;time.sleep(.01)
    qmp('stop')
   journal['before']=capture('before-offline')
   if source_checkpoint:
    address,size,kind=symbols['silent_resume'];assert kind.lower()=='b' and size in [1,4]
    assert packet(f'm{address:x},{size:x}')==bytes(size).hex()
    value=(1).to_bytes(size,'little').hex();assert packet(f'M{address:x},{size:x}:{value}')=='OK'
    assert packet(f'm{address:x},{size:x}')==value
    journal['model_injection']={'symbol':'silent_resume','address':address,'bytes':size,'value':value}
   address,size,kind=symbols['reconnect_checkpoint_gate'];assert size==4 and kind.lower()=='b'
   assert packet(f'm{address:x},4')=='00000000';assert packet(f'M{address:x},4:01000000')=='OK';assert packet(f'm{address:x},4')=='01000000'
   qmp('cont');deadline=time.monotonic()+45
   while process.poll() is None:
    try:state=qmp('query-status')['return']
    except (OSError,ValueError):process.wait(timeout=1);break
    assert state['status']!='debug','Fixture assertion; terminal state preserved'
    assert time.monotonic()<deadline;time.sleep(.05)
   journal.update(returncode=process.returncode,stdout=(case/'stdout.log').read_text(),stderr=(case/'stderr.log').read_text(),disk_sha256=sha(disk));save(case/'result.json',journal)
   assert process.returncode==0 and 'PASS DHCP reconnect' in journal['stdout'];debugger.close();print(journal['stdout'],end='')
  except BaseException as error:
   journal['error']=repr(error)
   if process.poll() is None and 'qmp' in locals():
    try:qmp('stop');journal['failure']=capture('failed-boundary')
    except BaseException as preservation_error:journal['failure_preservation_error']=repr(preservation_error)
   journal.update(returncode=process.poll(),stdout=(case/'stdout.log').read_text(),stderr=(case/'stderr.log').read_text());save(case/'stopped.json',journal)
   raise
if __name__=='__main__':main()
