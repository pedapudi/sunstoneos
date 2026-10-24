#!/usr/bin/env python3
"""Capture one root-admitted GPU return using the shared protected transport."""
import argparse,hashlib,importlib.util,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
prepare=module('finite_gpu_prepare',ROOT/'apps/gpu-finite/prepare.py');report=module('finite_gpu_report',ROOT/'apps/gpu-finite/report.py')
base=prepare.base;require=prepare.require
# The launcher grants 60 seconds of native execution; allow the existing
# 30-second ROM discovery interval after that deadline.
RETURN_TIMEOUT_SECONDS=60+30
CHUNK_BYTES=4*1024*1024
CAPTURE_RANGES=(('CPU',0x41000000,0x1200000),('GPU',0x48000000,0x31000000))
def transfer(transaction,address,data,destination):
 with base.sealed_source(data) as (source,fd):transaction('write',hex(address),source,'read',hex(address),len(data),destination,pass_fds=(fd,))
 require(destination.read_bytes()==data,'GPU DRAM restoration readback differs.');base.sync_file(destination)
def run_once(transaction,wait_return,ready,bundle,p,evidence,record,journal):
 require(not record.get('launch_attempted'),'One GPU attempt only.');ready();backups=[]
 disposition=json.loads(p['_frozen']['admission.json']).get('ram_disposition','restore')
 require(disposition in ('restore','discard'),'Unknown GPU RAM disposition.')
 record.update(ram_policy=disposition,ram_disposition='unresolved')
 for parent,address,length in (() if disposition=='discard' else CAPTURE_RANGES):
  for offset in range(0,length,CHUNK_BYTES):
   size=min(CHUNK_BYTES,length-offset);name=parent+'-%04d'%(offset//CHUNK_BYTES);backup=evidence/('before-'+name+'.bin')
   transaction('read',hex(address+offset),size,backup)
   require(backup.stat().st_size==size,'Incomplete '+name+' snapshot.');base.sync_file(backup);backups.append((name,address+offset,backup,base.digest(backup)))
 record.update(dram_backups={name:{'address':address,'bytes':backup.stat().st_size,'sha256':digest} for name,address,backup,digest in backups},dram_restored=False);journal()
 record['dirty']=True;journal();base.preload(transaction,bundle,p,evidence)
 # RVBAR=0x20 enters forced FEL directly; the normal-boot input is irrelevant.
 words=[int(x,16) for x in transaction(*[v for address in ('0x20','0x24','0x28','0x3c','0x40') for v in ('readl',address)]).split()]
 require(words==[0xeb00008f,0xe3a00911,0xe3a0193d,0xe3a0605c,0xea00004d],'Forced-FEL ROM entry differs from the reviewed instructions.')
 ready();require(int(transaction('readl','0x02050014').strip(),16)&1==0,'Watchdog enabled before GPU execution.')
 record.update(launch_attempted=True,backup_sha256=p['_backup_hashes'].copy());journal();transaction('exe','0x52000');wait_return()
 require('soc=00001903(A733)' in transaction('version'),'Returned service is not A733 ROM.')
 reports=[]
 for name,address,size in [('monitor',0x42000000,128),('platform',p['geometry']['log'],16384),('receipt',p['geometry']['receipt'],104),('packet',0x42000100,320)]:
  path=evidence/(name+'.bin');transaction('read',hex(address),size,path);require(path.stat().st_size==size,'Short GPU '+name+' report.');base.sync_file(path);reports.append(path.read_bytes())
 result=report.validate_reports(*reports,p);record['gpu_result']=result;record['result']={'magic':0x534b4552,'stage':5,'status':0};journal()
 # Verify every saved parent before the first restore. The returned ROM owns SRAM.
 for name,address,path,digest in backups:require(base.digest(path)==digest,'Saved '+name+' DRAM changed.')
 for name,address,path,digest in backups:
  data=path.read_bytes();require(0<len(data)<=CHUNK_BYTES and hashlib.sha256(data).hexdigest()==digest,'Saved '+name+' bytes changed.');transfer(transaction,address,data,evidence/(name+'-restored.bin'))
 record.update(complete=True,dirty=False,dram_restored=disposition=='restore',ram_disposition='discarded' if disposition=='discard' else 'restored',rom_usable=True,sram_restored=False,ownership_release_permitted=True);journal();return result

def admit(args,p,node,identity,port):
 admission=json.loads(p['_frozen']['admission.json']);require(admission['identity']==identity and admission['port']==port,'GPU admission covers another live instance.')
 prepare.validate_bundle(args.bundle.resolve())
 kernel=module('finite_gpu_kernel',ROOT/'tools/check-a733-kernel.py')
 kernel.validate_platform_admission((args.ddr_evidence.read_bytes(),args.platform_survey.read_bytes()),node,identity,port,base.FEL_HASH,base.FEL_HASH,tuple(x.read_bytes() for x in args.warm_return))
 refs={n:p['_frozen'][x['name']] for n,x in p['entry_evidence'].items()};covered={(x['path'],x['sha256']) for x in json.loads(refs[admission['ddr']['path']])['evidence']}
 require(all((str(x.resolve()),prepare.sha(x.read_bytes())) in covered for x in [args.ddr_evidence,args.platform_survey,*args.warm_return]),'Live CPU DDR/survey continuity absent from admission.')
def run(args):
 shared=module('gpu_returning_transport',ROOT/'tools/a733-returning-host.py')
 return shared.run(args,base=base,profile=prepare.PROFILE,validate_bundle=prepare.validate_bundle,admit=admit,execute=lambda tx,wait,ready,p,record,journal:run_once(tx,wait,ready,args.bundle.resolve(),p,args.evidence,record,journal),description='GPU finite computation and validated RAM return',return_timeout=RETURN_TIMEOUT_SECONDS)
if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__)
 for n in ('bundle','fel','evidence','ddr-evidence','platform-survey'):parser.add_argument('--'+n,type=Path,required=True)
 parser.add_argument('--warm-return',type=Path,action='append',default=[]);parser.add_argument('--root-owned-claim',required=True)
 args=parser.parse_args();raise SystemExit(0 if run(args)['functional_success'] else 2)
