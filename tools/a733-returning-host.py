#!/usr/bin/env python3
"""One-attempt A733 returning-ROM transport; profile callbacks own admission and RAM."""
import importlib.util,os,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def retain_failure(base,record,save,quarantine,error,hold=None):
 hold=base.hold_ownership if hold is None else hold
 if record.get('dirty') or record.get('transport_failed') or record.get('launch_attempted'):
  try:base.stop_after_failure(record,save,quarantine,error,hold=hold)
  finally:
   if not record.get('launch_attempted'):hold()

def run(args,*,base,profile,validate_bundle,admit,execute,description,access=None,guard=None):
 """Call admit(args,p,node,identity,port) before launch and execute(tx,wait,ready,p,record,save).

 execute owns protected upload, reports and RAM restoration. Every transport error
 is terminal. No callback creates queue authority; the caller owns the claim.
 Optional access/guard are dependency injection for offline transport fixtures.
 """
 require=base.require;bundle=args.bundle.resolve();p=validate_bundle(bundle)
 require(base.digest(args.fel)==base.FEL_HASH,'FEL executable differs from reviewed bytes.')
 access=module('returning_access',ROOT/'tools/usb-access.py') if access is None else access
 if guard is None:
  sys.path.insert(0,str(Path.home()/'git/mimus/tools'));import usb_guard
  guard=usb_guard
 guard.check_health();args.evidence.mkdir(parents=True,exist_ok=False)
 record={'mode':'kernel','execution_profile':profile,'complete':False,'launch_attempted':False,'dirty':False,'transport_failed':False,'root_owned_claim':args.root_owned_claim,'transactions':[],'tool_sha256':base.FEL_HASH,'image_sha256':p['image_sha256']}
 def journal():base.save_json(args.evidence/'session.json',record)
 with access.board_lock() as node:
  access.require_session(node);access.require_rom(node);identity=access.identity(node);port=access.target(node);config={'port':port.name,'sysfs':str(port.resolve())};failed=False
  record.update(initial_node=str(node),initial_identity=identity,port=port.name);journal()
  def ready():
   require(not failed,'No admission after transport failure.');validate_bundle(bundle)
   if not record['launch_attempted']:admit(args,p,node,identity,port.name)
  def transaction(*arguments,pass_fds=()):
   nonlocal failed
   require(not failed,'A transport failure forbids all further requests.')
   entry={'arguments':[str(x) for x in arguments]};record['transactions'].append(entry);journal()
   try:
    require(access.identity(node)==identity and access.target(node).name==port.name,'USB instance or physical port changed before FEL request.');access.require_rom(node)
    result=subprocess.run([str(args.fel),'-d',f'{int(node.parent.name)}:{int(node.name)}',*map(str,arguments)],capture_output=True,text=True,timeout=120 if 'read' in arguments or 'write' in arguments else 15,pass_fds=pass_fds)
    entry.update(exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr);require(result.returncode==0,'FEL request failed.')
   except (OSError,ValueError,subprocess.TimeoutExpired) as error:
    failed=True;entry['failure']=str(error);record['transport_failed']=True;journal();raise
   journal();return result.stdout
  def wait_return():
   nonlocal node,identity
   old=identity;deadline=time.monotonic()+30
   while True:
    candidate=access.session_node(config)
    if candidate is not None and access.identity(candidate)!=old and os.access(candidate,os.R_OK|os.W_OK):break
    require(time.monotonic()<deadline,'No new ROM instance within return window; no retry.');time.sleep(.05)
   node=candidate;identity=access.identity(node);access.require_session(node);access.require_rom(node)
   require(access.target(node).name==port.name,'Returned instance changed physical port.')
   record.update(returned_node=str(node),returned_identity=identity,automatic_access=True);journal()
  try:
   ready();require('soc=00001903(A733)' in transaction('version'),'Target is not A733.')
   with guard.reenumeration('sunstoneos-a733-'+port.name,description,2):result=execute(transaction,wait_return,ready,p,record,journal)
  except BaseException as error:
   record['transport_failed']=failed
   retain_failure(base,record,journal,lambda:access.quarantine_instance(node,'Returning attempt stopped; no requests or retry; root recovery required.',identity),error)
   raise
 return result
