#!/usr/bin/env python3
"""Exercise only extracted returning transport with mocked port metadata and subprocesses."""
import argparse,contextlib,importlib.util,json,subprocess,tempfile,types
from pathlib import Path
from unittest.mock import patch
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--evidence',type=Path,required=True);args=parser.parse_args();args.evidence.mkdir(parents=True,exist_ok=False)
root=Path(__file__).resolve().parents[2]
def load(name,path):
 spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
h=load('transport',root/'tools/a733-returning-host.py');base=load('base',root/'tools/check-a733-smp.py');results=[]
variants=('nominal','mismatch','transport-failure','subprocess-timeout','identity-changed','no-new-ROM','same-ROM','wrong-return-port','inaccessible-ROM','dirty-callback-failure','clean-admission-failure','return-session-failure')
with tempfile.TemporaryDirectory(prefix='returning-transport-') as temporary:
 t=Path(temporary);fel=t/'fel';fel.write_bytes(b'mocked executable, never launched');bundle=t/'bundle';bundle.mkdir()
 for variant in variants:
  state={'locked':False,'calls':[],'guard':[],'held':0,'quarantined':0,'admit':0,'old_changed':False};old=Path('/dev/bus/usb/003/056');new=Path('/dev/bus/usb/003/057');port=Path('/sys/bus/usb/devices/3-3.11')
  class Access:
   @contextlib.contextmanager
   def board_lock(self):
    state['locked']=True
    try:yield old
    finally:state['locked']=False
   def require_session(self,node):
    assert state['locked']
    if node==new and variant=='return-session-failure':raise ValueError('Modeled permission binding failure')
   def require_rom(self,node):assert state['locked']
   def identity(self,node):return [1,57 if node==new else (99 if state['old_changed'] else 56),3,4,5]
   def target(self,node):return port if not (node==new and variant=='wrong-return-port') else port.with_name('3-3.12')
   def session_node(self,config):
    assert config=={'port':port.name,'sysfs':str(port.resolve())}
    return None if variant=='no-new-ROM' else old if variant=='same-ROM' else new
   def quarantine_instance(self,*a):assert state['locked'];state['quarantined']+=1
  class Guard:
   def check_health(self):pass
   @contextlib.contextmanager
   def reenumeration(self,*arguments):
    assert state['locked'];state['guard'].append(arguments);yield
  def require(value,message):
   if not value:raise ValueError(message)
  def validate(path):assert path==bundle;return {'image_sha256':'f'*64}
  def admit(a,p,node,identity,selected):
   assert state['locked'] and node==old and identity==[1,56,3,4,5] and selected==port.name;state['admit']+=1
   if variant=='clean-admission-failure':raise ValueError('Modeled admission refusal')
  def execute(transaction,wait_return,ready,p,record,save):
   ready();record['dirty']=True;save()
   if variant=='dirty-callback-failure':raise ValueError('Modeled dirty callback failure')
   if variant=='identity-changed':state['old_changed']=True
   try:transaction('write','0x41000000','mocked-source','read','0x41000000',1,'mocked-destination',pass_fds=(7,))
   except (ValueError,subprocess.TimeoutExpired):
    count=len(state['calls'])
    try:transaction('version')
    except ValueError:pass
    else:raise AssertionError('Retry accepted after failure')
    assert len(state['calls'])==count
    raise
   record['launch_attempted']=True;save();transaction('exe','0x52000');wait_return();transaction('version');transaction('read','0x42000000',128,'mocked-report')
   record.update(complete=True,dram_restored=True,ownership_release_permitted=True);save();return {'functional_success':variant!='mismatch'}
  def process(command,**kwargs):
   assert state['locked'] and command[:3]==[str(fel),'-d','3:56' if len(state['calls'])<3 else '3:57'];state['calls'].append((command,kwargs))
   arguments=command[3:];assert kwargs['timeout']==(120 if 'read' in arguments or 'write' in arguments else 15)
   if arguments[0]=='write':
    assert kwargs['pass_fds']==(7,)
    if variant=='transport-failure':return types.SimpleNamespace(returncode=1,stdout='',stderr='modeled failure')
    if variant=='subprocess-timeout':raise subprocess.TimeoutExpired(command,120)
   return types.SimpleNamespace(returncode=0,stdout='soc=00001903(A733)' if arguments==['version'] else '',stderr='')
  def hold():assert state['locked'];state['held']+=1
  a=types.SimpleNamespace(bundle=bundle,fel=fel,evidence=t/variant,root_owned_claim='mock claim, no queue')
  error=None
  with patch.object(base,'digest',return_value=base.FEL_HASH),patch.object(base,'hold_ownership',hold),patch.object(h.subprocess,'run',process),patch.object(h.os,'access',return_value=variant!='inaccessible-ROM'),patch.object(h.time,'monotonic',side_effect=[0,31]),patch.object(h.time,'sleep',side_effect=AssertionError('No real wait')):
   try:result=h.run(a,base=base,profile='mock-returning-profile',validate_bundle=validate,admit=admit,execute=execute,description='Mock transport only',access=Access(),guard=Guard())
   except (ValueError,subprocess.TimeoutExpired) as failure:error=str(failure)
  normal=variant in ('nominal','mismatch');assert (error is None)==normal,(variant,error)
  assert not state['locked'];record=json.loads((a.evidence/'session.json').read_text())
  if normal:assert record['complete'] and not state['held'] and not state['quarantined'] and result['functional_success']==(variant=='nominal')
  elif variant=='clean-admission-failure':assert not state['calls'] and not state['held'] and not state['quarantined']
  else:assert state['held']==1 and state['quarantined']==1 and not record['complete'],(variant,state)
  if state['guard']:assert state['guard']==[('sunstoneos-a733-3-3.11','Mock transport only',2)]
  results.append({'name':variant,'passed':True,'FEL_requests':len(state['calls']),'retained':bool(state['held'])})
 # Check the production NPU adapter delegates admission and execution, without replaying reports.
 npu=load('npu_host',root/'apps/npu-finite/host.py');identity=[1,56,3,4,5];seen=[]
 paths=[]
 for name in ('ddr','survey','warm'):
  path=t/(name+'.json');path.write_bytes(name.encode());paths.append(path)
 admission={'identity':identity,'port':'3-3.11','ddr':{'path':'mock-ddr-ref'}}
 reservation={'evidence':[{'path':str(path.resolve()),'sha256':npu.sha(path.read_bytes())} for path in paths]}
 prepared={'_frozen':{'admission.json':json.dumps(admission).encode(),'ddr-record':json.dumps(reservation).encode()},'entry_evidence':{'mock-ddr-ref':{'name':'ddr-record'}}}
 a=types.SimpleNamespace(bundle=bundle,evidence=t/'adapter',ddr_evidence=paths[0],platform_survey=paths[1],warm_return=[paths[2]])
 kernel=types.SimpleNamespace(validate_platform_admission=lambda *values:seen.append(values))
 def delegated(arguments,**callbacks):
  assert arguments is a and callbacks['base'] is npu.base and callbacks['profile']==npu.PROFILE and callbacks['validate_bundle'] is npu.validate_bundle
  callbacks['admit'](a,prepared,old,identity,'3-3.11')
  return callbacks['execute']('transaction','wait','ready',prepared,{'marker':'record'},'journal')
 for successful in (True,False):
  def execute(*values):
   assert values==('transaction','wait','ready',bundle,prepared,a.evidence,{'marker':'record'},'journal');return {'functional_success':successful}
  with patch.object(npu,'module',return_value=kernel),patch.object(npu.returning,'run',delegated),patch.object(npu,'run_once',execute):assert npu.run(a)==(0 if successful else 2)
  assert seen[-1]==((b'ddr',b'survey'),old,identity,'3-3.11',base.FEL_HASH,base.FEL_HASH,(b'warm',))
  results.append({'name':'NPU-adapter-'+('success' if successful else 'mismatch'),'passed':True})
(args.evidence/'transport-result.json').write_text(json.dumps({'passed':True,'physical_access':False,'cases':results,'scope':'Extracted live orchestration only; fake per-port metadata, guard, FEL subprocess and clock. No native checks or pure report cases replayed.'},indent=2)+'\n');print('PASS extracted transport cases:',len(results))
