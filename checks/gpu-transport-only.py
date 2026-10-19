#!/usr/bin/env python3
"""Focused transport-only application branch, immutable mode and host capture checks."""
import argparse,copy,hashlib,importlib.util,json,struct
from pathlib import Path
from unittest.mock import patch
from unicorn import UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
transport=module('transport_case_base',ROOT/'checks/gpu-usb-ordering.py')
class Machine(transport.Machine):
 def __init__(self,image,fail=None):
  super().__init__(image);self.fail=fail;self.calls=[];self.created=0;self.gpu_calls=[]
  self.sealed=0x43010000;self.cpu.mem_write(self.sealed,bytes(592));fields=image.layouts['a733_board_session']
  for name,value in [('workload',2),('epoch',123),('generation',124),('pmic_owner',10),('usb0_owner',11)]:self.write(self.sealed+fields[name],value,4 if name=='workload' else 8)
  runtime=image.address('a733_cached_runtime');fields=image.layouts['a733_cached_runtime']
  for name,value in [('epoch',123),('startup_deadline',24000000),('diagnostic_deadline',61000000*24)]:self.write(runtime+fields[name],value,8)
  names=['a733_smp_device_preparation_admitted','a733_device_memory_ready','gpu_application_session','a733_board_session_valid','a733_native_usb_init','xTaskCreateStatic','a733_native_usb_bind','vTaskStartScheduler','platform_panic']
  self.hooks.update({image.address(n):n for n in names})
  for name in ['a733_gpu_platform_native_init','a733_gpu_platform_init','a733_gpu_platform_bind_pmic','a733_gpu_reserved_prepare','a733_gpu_compute_reserved_prepare','a733_gpu_backend_bind','pvr_gpu_service_init','pvr_gpu_service_bind','pvr_gpu_service_interrupt']:
   if image.symbols.get(name,set())-{0}:self.hooks[image.address(name)]='forbidden GPU '+name
  self.cpu.hook_add(UC_HOOK_MEM_READ,self.mmio,begin=0x01800000,end=0x080fffff);self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.mmio,begin=0x01800000,end=0x080fffff)
 def mmio(self,*args):raise AssertionError('Application diagnostic accessed MMIO')
 def hook(self,cpu,address,size,unused):
  name=self.hooks.get(address)
  if name and name.startswith('forbidden GPU '):self.gpu_calls.append(name);raise AssertionError(name)
  if name in ['a733_smp_device_preparation_admitted','a733_device_memory_ready','a733_board_session_valid','a733_native_usb_init','a733_native_usb_bind']:
   self.calls.append(name);self.ret(0 if name==self.fail else 1);return
  if name=='gpu_application_session':self.ret(self.sealed);return
  if name=='xTaskCreateStatic':self.created+=1;self.ret(0x43002000+0x100*self.created);return
  if name in ['vTaskStartScheduler','platform_panic']:self.stop=name;cpu.emu_stop();return
  super().hook(cpu,address,size,unused)

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--image',type=Path,required=True);parser.add_argument('--manifest',type=Path,required=True);parser.add_argument('--compute-image',type=Path,required=True);parser.add_argument('--output',type=Path,required=True);a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=False)
 image=transport.layout.Image(a.image);cases=[]
 def passed(name,**extra):cases.append({'name':name,'passed':True,**extra})
 m=Machine(image);m.run('a733_smp_diagnostic_start');assert m.stop=='vTaskStartScheduler' and m.created==3 and m.field('preparation_failed') and not m.field('result') and not m.field('fatal') and not m.gpu_calls
 m.client();assert m.stop=='delay' and not m.opened
 m.queue();frame=b''.join(m.tx);assert len(frame)==40 and struct.unpack('<7IQI',frame)==(0x43555047,1,0,0,0,2,0,123,0)
 m.complete(20);assert not m.field('usb_observed');m.client();assert not m.opened
 m.complete(20);assert m.field('usb_observed');m.client();assert m.stop=='delay' and not m.opened
 m.run('gpu_application_irq');assert not m.gpu_calls and not m.field('fatal')
 passed('diagnostic-binds-real-tasks-parks-client-before-and-after-complete-pending-frame',frame=frame.hex(),modeled=['native shared init/bind','scheduler','TinyUSB callbacks'])
 for fail in ['a733_native_usb_init','a733_native_usb_bind']:
  m=Machine(image,fail);m.run('a733_smp_diagnostic_start');assert m.stop=='platform_panic' and not m.gpu_calls;passed(fail+'-still-panics')
 m=Machine(image);m.run('a733_smp_diagnostic_start');m.health=-1;m.queue();assert m.stop=='delay' and m.field('fatal') and not m.tx and not m.opened;passed('runtime-provider-failure-still-fatal')
 host=module('transport_host',ROOT/'tools/check-a733-gpu-supervised.py');manifest=json.loads(a.manifest.read_text());raw=a.image.read_bytes();host.preparation.image_manifest_check(manifest,raw);passed('transport-manifest-binds-readonly-marker-and-single-core-geometry')
 for name,body,label in [('transport-as-compute',raw,{**manifest,'workload':'fixed-arithmetic-repeat'}),('compute-as-transport',a.compute_image.read_bytes(),{**manifest,'final_elf':hashlib.sha256(a.compute_image.read_bytes()).hexdigest()}),('render-as-transport',raw,{**manifest,'target':'sunstone-a733-gpu-render','execution_profile':'gpu-supervised-v2'})]:
  try:host.preparation.image_manifest_check(label,body)
  except ValueError:passed(name+'-rejected')
  else:raise AssertionError(name)
 session=bytearray(592);struct.pack_into('<Q',session,24,123)
 for variant in ['pending','success-output','failure','pending-output','wrong-epoch','short','late','read-failure','save-failure']:
  directory=a.output/variant;directory.mkdir();state={'time':100.,'calls':0};record={'complete':False};committed=[]
  def exchange(packet,timeout):
   assert packet==bytes(4);state['calls']+=1
   if variant=='read-failure':raise OSError('fixture read failure')
   if variant=='late':state['time']=200
   response=frame
   if variant=='success-output':response=struct.pack('<7IQI',0x43555047,1,1,0,32,2,1,123,64)+struct.pack('<32I',*[((17+(i+1)*3)^(0x9e3779b9+(i<<16)))&0xffffffff for i in range(32)])
   elif variant=='failure':response=struct.pack('<7IQI',0x43555047,1,2,0,0,2,0,123,0)
   elif variant=='pending-output':response+=b'payload'
   elif variant=='wrong-epoch':response=struct.pack('<7IQI',0x43555047,1,0,0,0,2,0,999,0)
   elif variant=='short':response=response[:12]
   return response
  def save():
   if variant=='save-failure' and record.get('gpu_transport_result')=='observed-success':raise OSError('fixture journal failure')
   committed.append(copy.deepcopy(record))
  try:
   with patch.object(host.preparation,'session_check',lambda data:None):host.capture(exchange,session,directory,record,save,150,now=lambda:state['time'],pause=lambda n:None,workload='shared-usb-pending')
   error=None
  except (ValueError,OSError,RuntimeError,struct.error) as ex:error=str(ex)
  assert state['calls']==1 and record.get('gpu_result')=='unobserved' and not record['complete'] and not (directory/'compute.bin').exists()
  if variant=='pending':assert error is None and record['gpu_transport_result']=='observed-success' and record['durable_result']['sha256']==hashlib.sha256(frame).hexdigest()
  else:assert error and record.get('gpu_transport_result')=='unobserved' and not any(r.get('gpu_transport_result')=='observed-success' for r in committed)
  passed('host-'+variant,error=error)
 # Exercise production dispatch without queue/device access. Bundle bytes and
 # supervisor entry are modeled; immutable mode admission above uses real ELF.
 bundle=a.output/'modeled-bundle';bundle.mkdir();payloads={'session.bin':bytes(session),'session-template.bin':bytes(session),'admission.json':b'{}','image-input-manifest.json':a.manifest.read_bytes(),'image.elf':raw,'prepared.json':b'{}'}
 for name,data in payloads.items():(bundle/name).write_bytes(data)
 prepared={'files':{k:hashlib.sha256(v).hexdigest() for k,v in payloads.items() if k!='prepared.json'},'entry_evidence':{},'retention':'whole-board-power-removal'}
 (bundle/'prepared.json').write_text(json.dumps(prepared))
 def run(*args,**kwargs):assert kwargs['result_key']=='gpu_transport_result';return 0
 with patch.object(host.preparation,'validate_bundle',lambda b:copy.deepcopy(prepared)),patch.object(host.supervisor,'run_session',run),patch.object(host,'require_embedded_firmware',lambda data:(_ for _ in ()).throw(AssertionError('Transport mode required GPU firmware'))):
  assert host.main(['--bundle',str(bundle),'--fel','/modeled-fel','--evidence',str(a.output/'no-device-evidence'),'--database','/modeled-db','--claim','model','--owner','root','--supervised-manual-reconnect'])==0
 passed('host-dispatch-selects-transport-result-without-GPU-firmware-admission')
 result={'passed':True,'physical_access':False,'queue_access':False,'image_sha256':hashlib.sha256(raw).hexdigest(),'cases':cases,'scope':'Only diagnostic branch, unchanged pending serializer composition, immutable mode admission and diagnostic host capture. Shared provider constructors, scheduler, USB and device transport are modeled; CPU/startup, PMIC/USB drivers and arithmetic suites are reused without rerun.'}
 (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(str(len(cases))+' focused transport-only cases passed')
if __name__=='__main__':main()
