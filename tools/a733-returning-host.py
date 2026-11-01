#!/usr/bin/env python3
"""One-attempt A733 returning-ROM transport; profile callbacks own admission and RAM."""
import importlib.util,json,math,os,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
crumbs=module('returning_crumbs',ROOT/'tools/a733-crumbs.py')

class WatchdogReset(RuntimeError):
 """The armed WDT0 reset the board to a new ROM instance; DRAM is lost, the run failed."""

def retain_failure(base,record,save,quarantine,error,hold=None):
 hold=base.hold_ownership if hold is None else hold
 if record.get('dirty') or record.get('transport_failed') or record.get('launch_attempted'):
  try:base.stop_after_failure(record,save,quarantine,error,hold=hold)
  finally:
   if not record.get('launch_attempted'):hold()

def run(args,*,base,profile,validate_bundle,admit,execute,description,access=None,guard=None,return_timeout=30,watchdog=None):
 """Call admit(args,p,node,identity,port) before launch and execute(tx,launch,ready,p,record,save).

 With watchdog={'mode':m,'session_seconds':s} (the image's marker), launch()
 clears the RTC breadcrumb words and arms WDT0 just before exe, inside the
 churn guard. The image restarts WDT0 while its session is live, so the return
 window becomes the session plus one interval plus 10 s. On the returned
 instance one request reads the mode, disarms and reads the breadcrumbs. A set
 enable bit means the watchdog reset the board: the report is written, the run
 fails with WatchdogReset, and the fresh ROM instance is neither quarantined
 nor held. The capture continues only for a clear enable bit with this
 session's RETURN_ROM breadcrumb and mode 0; anything else is an unresolved
 transition and is retained.

 execute owns protected upload, reports and RAM restoration; launch() alone
 executes the fixed SRAM launcher and waits for the new permitted ROM instance.
 Every transport error
 is terminal. No callback creates queue authority; the caller owns the claim.
 Optional access/guard are dependency injection for offline transport fixtures.
 """
 require=base.require
 require(type(return_timeout) in (int,float) and math.isfinite(return_timeout) and return_timeout>0,
         'Return window must be a finite positive number of seconds.')
 bundle=args.bundle.resolve();p=validate_bundle(bundle)
 require(base.digest(args.fel)==base.FEL_HASH,'FEL executable differs from reviewed bytes.')
 access=module('returning_access',ROOT/'tools/usb-access.py') if access is None else access
 if guard is None:
  sys.path.insert(0,str(ROOT/'tools'));import usb_guard
  guard=usb_guard
 guard.check_health();args.evidence.mkdir(parents=True,exist_ok=False)
 if watchdog is not None:
  require(set(watchdog)=={'mode','session_seconds'} and all(type(v) is int for v in watchdog.values()) and
          watchdog['mode']&0xff0f==1 and watchdog['mode']>>4 in crumbs.INTERVALS and
          1<=watchdog['session_seconds']<=600,'Watchdog mode or session length differs.')
  # Restarts stop at a failure or the session deadline, so the reset comes at
  # most one interval later; a longer wait only delays the failure report.
  return_timeout=watchdog['session_seconds']+crumbs.interval(watchdog['mode'])+10
 record={'mode':'kernel','execution_profile':profile,'complete':False,'launch_attempted':False,'dirty':False,'transport_failed':False,'root_owned_claim':args.root_owned_claim,'transactions':[],'return_timeout_seconds':return_timeout,'tool_sha256':base.FEL_HASH,'image_sha256':p['image_sha256']}
 def journal():base.save_json(args.evidence/'session.json',record)
 with access.board_lock() as node:
  access.require_session(node);access.require_rom(node);identity=access.identity(node);port=access.target(node);config={'port':port.name,'sysfs':str(port.resolve())};failed=False
  record.update(initial_node=str(node),initial_identity=identity,port=port.name);journal()
  launch_called=False;transition_active=False;transition_retained=False;recovered=None
  def ready():
   require(not failed,'No admission after transport failure.');validate_bundle(bundle)
   if not record['launch_attempted']:admit(args,p,node,identity,port.name)
  def transaction(*arguments,pass_fds=()):
   nonlocal failed
   require(not failed,'A transport failure forbids all further requests.')
   require(not any(op in arguments for op in ('exe','reset64')) or transition_active,'Native execution requires the guarded launch callback.')
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
   old=identity;deadline=time.monotonic()+return_timeout
   while True:
    # Identity comes from one guarded lstat; a node removed by the expected disconnect is absent.
    candidate,current=base.present_instance(access,config)
    available=candidate is not None and current!=old and os.access(candidate,os.R_OK|os.W_OK)
    require(time.monotonic()<deadline,'No new ROM instance within return window; no retry.')
    if available:break
    time.sleep(.05)
   node,identity=candidate,current;access.require_session(node);access.require_rom(node)
   require(access.target(node).name==port.name,'Returned instance changed physical port.')
   record.update(returned_node=str(node),returned_identity=identity,automatic_access=True);journal()
  def arm_watchdog():
   mode=watchdog['mode'];record['watchdog']={'mode':mode,'interval_seconds':crumbs.interval(mode),'armed':False};journal()
   transaction(*crumbs.arm_arguments(mode))
   record['watchdog'].update(armed=True,armed_monotonic=time.monotonic());journal()
   words=[int(x,16)&0xffff for x in transaction('readl',hex(crumbs.WDT_MODE),'readl',hex(crumbs.WDT_CONFIG),'readl',hex(crumbs.WDT_OUTPUT)).split()]
   require(words==[mode,1,2],'WDT0 readback after arming differs.')
  def watchdog_return():
   """Classify the returned instance before any DRAM access; return the reset report or None."""
   state=record['watchdog'];state['returned_after_seconds']=round(time.monotonic()-state['armed_monotonic'],3)
   before,after,words=crumbs.parse_return(transaction(*crumbs.return_arguments()))
   elf=crumbs.bundle_image(bundle,p['image_sha256'])
   decoded=crumbs.decode(words,p['epoch'],elf)
   state.update(mode_after_return=before,mode_after_disarm=after,disarmed=not after&1,
                fired=bool(before&1),breadcrumbs=decoded);journal()
   require(not after&1,'WDT0 did not disarm on the returned instance.')
   if not before&1:
    last=decoded.get('last',{})
    require(decoded['session'] and last.get('stage')==crumbs.RETURN_ROM and last.get('value')==0,
            'Returned with WDT0 disarmed but without this session\'s RETURN_ROM breadcrumb.')
    return None
   # Mode 0x01: the image's EL3 stop shortened WDT0 to 0.5 s before parking.
   report={'watchdog_fired':True,'interval_seconds':state['interval_seconds'],
           'reset_interval_seconds':crumbs.interval(before),'stop_shortened':before&0xffff==1,
           'returned_after_seconds':state['returned_after_seconds'],'breadcrumbs':decoded,
           'dram':'lost: DDR is uninitialized after the reset; run cold DDR before any DRAM access',
           'cpu_rails':'PMIC rails keep the values the image set; restore before shared reuse',
           'summary':'WDT0 reset the board after %.1f s; %s'%(state['returned_after_seconds'],decoded['summary'])}
   launcher=args.evidence/'launcher-report-after-reset.bin'
   transaction('read','0x52d00',128,launcher)
   report['launcher_report_words']=[hex(int.from_bytes(launcher.read_bytes()[i:i+8],'little')) for i in range(0,56,8)]
   base.save_json(args.evidence/'watchdog-report.json',report)
   return report
  def launch():
   nonlocal launch_called,transition_active,transition_retained,recovered
   require(not launch_called and not record['launch_attempted'],'One returning launch callback only.');launch_called=True
   with guard.reenumeration('sunstoneos-a733-'+port.name,description,2):
    # Churn admission can wait while RAM is already uploaded. Recheck the
    # original entry before recording any native launch attempt.
    ready()
    record['launch_attempted']=True;journal();transition_active=True
    try:
     if watchdog is not None:arm_watchdog()
     transaction('exe','0x52000');wait_return()
     if watchdog is not None:recovered=watchdog_return()
    except BaseException as error:
     # Keep an unresolved transition inside the guard while ownership is held.
     # A pre-launch refusal above instead retains only this port outside it.
     transition_retained=True;record['transport_failed']=failed
     retain_failure(base,record,journal,lambda:access.quarantine_instance(node,'Returning transition unresolved; no requests or retry; root recovery required.',identity),error)
     raise
    finally:transition_active=False
   if recovered is not None:
    # The new instance passed the ROM checks and WDT0 is disarmed; only DRAM
    # and rails are unresolved, so the claim stays retained without a hold.
    record.update(watchdog_reset=True,rom_usable=True,dram_restored=False,dram_lost=True,
                  cold_ddr_required=True,ownership_release_permitted=False,failure=recovered['summary'])
    journal()
    try:print('RETURN FAILED: '+recovered['summary'],file=sys.stderr,flush=True)
    except (OSError,ValueError):pass
    raise WatchdogReset(recovered['summary'])
  try:
   ready();require('soc=00001903(A733)' in transaction('version'),'Target is not A733.')
   result=execute(transaction,launch,ready,p,record,journal)
  except BaseException as error:
   record['transport_failed']=failed
   if not transition_retained and recovered is None:retain_failure(base,record,journal,lambda:access.quarantine_instance(node,'Returning attempt stopped; no requests or retry; root recovery required.',identity),error)
   raise
 return result
