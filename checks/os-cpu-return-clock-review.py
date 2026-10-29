#!/usr/bin/env python3
"""Exercise production under-test parser with explicitly modeled authority.

No physical qualification is issued. Actual native image/rail helper markers
and their copied manifests are used; authority, identity and history are models.
"""
import argparse,copy,hashlib,importlib.util,json,struct
from pathlib import Path
from harness import refused,save
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('entry_review_prepare',ROOT/'apps/os/prepare.py')
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)
def digest(b):return hashlib.sha256(b).hexdigest()
def main():
 parser=argparse.ArgumentParser(description=__doc__)
 for n in ('image','manifest','helper','out'):parser.add_argument('--'+n,type=Path,required=True)
 parser.add_argument('--compatibility', action='store_true')
 args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=False)
 image=args.image.read_bytes();manifest=json.loads(args.manifest.read_text())
 p.image_manifest_check(manifest,image,cpu_return=True)
 evidence={}
 def store(name,value):
  raw=value if isinstance(value,bytes) else json.dumps(value,sort_keys=True).encode()
  path='/MODEL-NOT-PHYSICAL/'+name;evidence[path]=raw;return {'path':path,'sha256':digest(raw)}
 def get(r):
  p.require(isinstance(r,dict) and set(r)=={'path','sha256'} and r['path'] in evidence,'modeled evidence absent')
  b=evidence[r['path']];p.require(digest(b)==r['sha256'],'modeled evidence changed');return b
 common=dict(identity=['MODEL-NO-USB'],port='MODEL-NO-PORT',image_sha256=digest(image),epoch=4660,generation=9,lease=24000000)
 record={**common,'inference':manifest['inference']}
 for k in ('initial','electrical','clocks','supervision','ddr','platform','mimus_ddr'):record[k]=store(k,{'MODEL-NOT-PHYSICAL':True})
 raw=bytearray(904);struct.pack_into('<3Q',raw,24,4660,9,24000000)
 raw[48:80]=bytes.fromhex(manifest['build_id']);struct.pack_into('<I',raw,400,2);struct.pack_into('<I',raw,804,70000)
 struct.pack_into('<6Q4I',raw,808,p.GRANT_MAGIC,1,96,777,4660,9,3,1794,2002,4)
 history={**common,'schema':'a733-os-cpu-return-historical-qualification-v1','reviewed':True,
  'same_physical_board':True,'board_mapping_basis':'MODEL ONLY no physical mapping','sid_word':0x400,
  'steps':[1,2,3,4,5,6],'evidence':[store('physical-model',{'MODEL-NOT-PHYSICAL':True})]}
 a={**common,'schema':p.cpu_return_clock.SCHEMA,'execution_profile':'os-cpu-return-v1','status':'under-test','reviewed':True,
  'build_id':manifest['build_id'],'session_template_without_approval_sha256':digest(raw),
  'grant':{'owner':777,'clusters':3,'max_mhz':[1794,2002],'changes':4},'inference':manifest['inference'],
  'guard_marker':list(p.cpu_return_clock.MARKER),'thermal':{k:record[k] for k in ('initial','electrical','clocks','supervision')},
  'thermal_config_sha256':digest(raw[760:808]),'one_attempt':True,'failure_policy':'retain-without-retry',
  'supervisor':'MODEL ONLY','root_owned_claim':'MODEL ONLY','approval_text':'MODEL ONLY first functional under test',
  'approval_date':'MODEL ONLY','conditions':{k:store('condition-'+k,{'MODEL-NOT-PHYSICAL':True}) for k in p.cpu_return_clock.CONDITIONS},
  'historical_qualification':store('history',history),'historical_sid_word':0x400,'historical_bin':4,
  'sources':[store('source-model',{'MODEL-NOT-PHYSICAL':True})],
  'entry_continuity':{k:record[k] for k in ('ddr','platform','mimus_ddr')},
  'rail_helper':{k:store('helper-'+k,(args.helper/n).read_bytes()) for k,n in [('manifest','input-identities.json'),('image','build/probe.elf'),('binary','probe.bin')]}}
 rows=[]
 def validate(value,returning=True,perf=False):
  b=json.dumps(value,sort_keys=True).encode();r=copy.deepcopy(record);r['cpu_clock_approval']=store('approval',b)
  data=bytearray(raw);data[872:904]=hashlib.sha256(b).digest()
  r['normal_return_review']=store('final-review',{'cpu_clock_approval':r['cpu_clock_approval'],'session_template_sha256':digest(data)})
  p.cpu_clock_admission(r,digest(image),data,p.capture_profile(data),get,perf_modes=perf,cpu_return=returning)
 validate(a);rows.append({'name':'exact-under-test-model-authority','passed':True})
 refused(lambda:validate(a,False),'approval record names another');rows.append({'name':'ordinary-profile-refuses-new-schema','passed':True})
 for name,change,reason in [
  ('passed-claim',lambda x:x.update(passed=True),'another profile'),
  ('wrong-profile',lambda x:x.update(execution_profile='os-supervised-v1'),'another profile'),
  ('wrong-session',lambda x:x.update(epoch=1),'another profile'),
  ('wrong-image',lambda x:x.update(image_sha256='0'*64),'another profile'),
  ('wrong-board',lambda x:x.update(identity=['OTHER']),'another profile'),
  ('wrong-template',lambda x:x.update(session_template_without_approval_sha256='0'*64),'normalized template'),
  ('wrong-grant',lambda x:x['grant'].update(changes=5),'grant differs'),
  ('wrong-inference',lambda x:x['inference'].update(workers=2),'inference or guard'),
  ('wrong-guard',lambda x:x.update(guard_marker=[0,1,4,8]),'inference or guard'),
  ('wrong-thermal',lambda x:x.update(thermal_config_sha256='0'*64),'thermal configuration'),
  ('missing-condition',lambda x:x['conditions'].pop(next(iter(x['conditions']))),'review obligations'),
  ('no-history',lambda x:x.pop('historical_qualification'),'modeled evidence absent'),
  ('no-source',lambda x:x.update(sources=[]),'source provenance'),
  ('wrong-helper',lambda x:x['rail_helper'].update(binary=x['rail_helper']['image']),'rail-helper identity'),
  ('wrong-continuity',lambda x:x.update(entry_continuity={}), 'entry continuity'),
  ('wrong-historical-bin',lambda x:x.update(historical_bin=3),'historical bin')]:
  value=copy.deepcopy(a);change(value);refused(lambda:validate(value),reason);rows.append({'name':name,'passed':True})
 # Legacy B1 branch remains selected even for returning callers. Its actual
 # production rejection proves the new schema has no implicit fallback.
 value=copy.deepcopy(a);value['schema']=p.APPROVAL_SCHEMA;value['step']='TOP'
 value['cluster']='both';value['highest_mhz']={'cpu-l':1794,'cpu-b':2002};value['changes']=4
 value['b1']=store('old-b1',{'schema':p.B1_SCHEMA,'passed':False})
 refused(lambda:validate(value),"board's B1 record")
 rows.append({'name':'legacy-still-requires-passed-B1','passed':True})
 if args.compatibility:
  a=copy.deepcopy(a);a['schema']=p.cpu_return_clock.COMPATIBILITY_SCHEMA
  a['conditions']={k:store('compat-condition-'+k,{'MODEL-NOT-PHYSICAL':True}) for k in p.cpu_return_clock.COMPATIBILITY_CONDITIONS}
  historical={'schema':'a733-os-cpu-return-historical-mechanism-v1','reviewed':True,
   'identity':[7,100,200,0,0],'port':'MODEL-HISTORICAL-OTHER-BOARD','date':'MODEL-PAST-DATE',
   'sid_word':0x400,'bin':4,'steps':[1,2,3,4,5,6],'evidence':history['evidence']}
  a['historical_qualification']=store('compatible-history',historical)
  compatibility={**common,'schema':'a733-os-cpu-return-mechanism-compatibility-v1',
   'reviewed':True,'status':'under-test','execution_profile':'os-cpu-return-v1',
   'build_id':a['build_id'],'session_template_without_approval_sha256':a['session_template_without_approval_sha256'],
   'historical_mechanism':a['historical_qualification'],'source_qualification_reused_as':'mechanism_evidence',
   'target_device_qualification':'under-test','review_basis':'MODEL ONLY compatible sources; no actual mapping',
   'applicability':{k:store('applicability-'+k,{'MODEL-NOT-PHYSICAL':True}) for k in p.cpu_return_clock.APPLICABILITY},
   'residuals':{k:'under-test' for k in p.cpu_return_clock.RESIDUALS},'sources':a['sources']}
  a['mechanism_compatibility']=store('compatibility',compatibility)
  validate(a);rows.append({'name':'different-historical-board-compatible-mechanism-under-test','passed':True})
  refused(lambda:validate(a,False),'approval record names another')
  refused(lambda:validate(a,True,True),'performance-mode approval')
  rows.extend({'name':n,'passed':True} for n in ('compat-ordinary-caller-refused','compat-perf-caller-refused'))
  invented=copy.deepcopy(a);invented['same_physical_board']=True
  refused(lambda:validate(invented),'cannot assert historical board identity')
  rows.append({'name':'compat-approval-invented-mapping-refused','passed':True})
  old=copy.deepcopy(a);old['schema']=p.cpu_return_clock.SCHEMA
  old['conditions']={k:store('old-condition-'+k,{'MODEL-NOT-PHYSICAL':True}) for k in p.cpu_return_clock.CONDITIONS}
  refused(lambda:validate(old),'board mapping absent');rows.append({'name':'old-schema-still-refuses-other-board','passed':True})
  for name,change,reason in [
   ('wrong-target',lambda x:x.update(identity=['OTHER']), 'another target/session'),
   ('wrong-image',lambda x:x.update(image_sha256='0'*64), 'another target/session'),
   ('wrong-session',lambda x:x.update(epoch=1), 'another target/session'),
   ('wrong-build',lambda x:x.update(build_id='0'*64), 'another target/session'),
   ('wrong-template',lambda x:x.update(session_template_without_approval_sha256='0'*64), 'another target/session'),
   ('wrong-history',lambda x:x.update(historical_mechanism=a['sources'][0]), 'another target/session'),
   ('target-passed',lambda x:x.update(target_device_qualification='passed'), 'another target/session'),
   ('passed-claim',lambda x:x.update(passed=True), 'another target/session'),
   ('same-board-claim',lambda x:x.update(same_physical_board=True), 'another target/session'),
   ('missing-applicability',lambda x:x.update(applicability={}), 'applicability'),
   ('missing-residual',lambda x:x.update(residuals={}), 'applicability'),
   ('missing-source-pins',lambda x:x.update(sources=[]), 'applicability')]:
   changed=copy.deepcopy(compatibility);change(changed);value=copy.deepcopy(a)
   value['mechanism_compatibility']=store('changed-compatibility',changed)
   refused(lambda:validate(value),reason);rows.append({'name':'compat-'+name,'passed':True})
  for name,change in [('invented-mapping',lambda x:x.update(same_physical_board=True)),
                      ('historical-pass-relabel',lambda x:x.update(passed=True)),
                      ('historical-identity-absent',lambda x:x.pop('identity')),
                      ('historical-date-absent',lambda x:x.pop('date'))]:
   changed=copy.deepcopy(historical);change(changed);value=copy.deepcopy(a)
   value['historical_qualification']=store('changed-history',changed)
   refused(lambda:validate(value),'actual source identity');rows.append({'name':'compat-'+name,'passed':True})
 save(args.out/'results.json',{'scope':'production parser; modeled authority, actual helper/native markers; no physical admission','cases':rows})
 print('PASS',len(rows),'under-test admission cases')
if __name__=='__main__':main()
