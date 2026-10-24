#!/usr/bin/env python3
"""Check GPU report and capture boundaries without USB or physical admission."""
import argparse,contextlib,copy,hashlib,importlib.util,json,struct,types
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
host=module('gpu_finite_host_check',ROOT/'apps/gpu-finite/host.py');report=host.report

def reports(p):
 g=p['geometry'];words=[((17+(i+1)*3)^(0x9e3779b9+(i<<16)))&0xffffffff for i in range(32)]*2
 log='GPU_STAGE=STOPPED\nGPU_COMPUTE_WORDS=\n'
 for n in range(2):log+='GPU_SEQUENCE=0x%016x\n'%(n+1)+''.join('0x%016x\n'%v for v in words[n*32:n*32+32])
 log+='PASS GPU two copied results, CLOSE and parked providers\n';log=log.encode()
 lines=(g['clean_end']-g['clean_start']+g['wb_end']-g['wb_start'])//64
 monitor=struct.pack('<16Q',0x4153373347535232,p['epoch'],p['generation'],5,0,0,100,1,300,*([0]*7))
 platform=bytearray(16384);struct.pack_into('<16Q',platform,0,0x534b4552,5,0,len(log),0,0,0,0,110,300,*([0]*6));platform[256:256+len(log)]=log
 receipt=struct.pack('<13Q',report.RECEIPT_MAGIC,p['epoch'],p['generation'],p['lease'],0,1,g['clean_start'],g['clean_end'],g['wb_start'],g['wb_end'],lines,10000,len(log))
 packet=struct.pack('<QIIQQ4I64I2Q',report.PACKET_MAGIC,1,320,p['epoch'],p['generation'],1,2,64,lines,*words,200,300)
 return monitor,bytes(platform),receipt,packet
class Data:
 def __init__(self,size,tag):self.size,self.tag=size,tag
 def __len__(self):return self.size
 def __eq__(self,other):return isinstance(other,Data) and (self.size,self.tag)==(other.size,other.tag)
class Store:
 def __init__(self,name,files):self.name,self.files=name,files
 def __truediv__(self,name):return Store(self.name+'/'+name,self.files)
 def stat(self):return types.SimpleNamespace(st_size=len(self.files[self.name]))
 def read_bytes(self):return self.files[self.name]
 def write_bytes(self,data):self.files[self.name]=data
 def __str__(self):return self.name

def writer_check(image,manifest,geometry,evidence):
 # Execute one existing normal-return model; the monitor bytes come from the
 # frozen native writer, independently of the host fixture's magic constant.
 native=module('gpu_native_report_writer',ROOT/'apps/gpu-finite/return-check.py')
 assert hashlib.sha256(image.read_bytes()).hexdigest()=='2ab2cb1b3bac150d5eb695793ab0eb39bb453afc7fbd7ecb2f595ebfca3fd08d'
 # This pinned image predates cached table walks. Keep its actual TCR in the
 # existing translation model, as required by tools/a733_tcr.py.
 native.entry.shared.TCR=native.entry.shared.shared.TCR=0x200803019
 linked=native.layout.Image(image);linked.path=image;linked.ops={}
 for line in native.subprocess.check_output(['/usr/lib/llvm-21/bin/llvm-objdump','-d','--no-show-raw-insn',str(image)],text=True).splitlines():
  match=native.re.match(r'^\s*([0-9a-f]+):\s+(.+)$',line)
  if match:linked.ops[int(match[1],16)]=match[2].strip()
 template=Path.home()/'.cache/sunstoneos/evidence/gpu-single-core-20261018/provider-valid-fixture/fixtures/session-template.bin'
 session=bytearray(template.read_bytes());session[48:80]=bytes.fromhex(manifest['build_id'])
 machine=native.Machine(linked,bytes(session),'normal');machine.prepare();machine.finish()
 assert machine.reset and not machine.parked
 monitor=bytes(machine.cpu.mem_read(0x42000000,128));(evidence/'native-monitor.bin').write_bytes(monitor)
 m=struct.unpack('<16Q',monitor);p={'epoch':m[1],'generation':m[2],'lease':103,'geometry':geometry}
 # Only the monitor is native-produced. These companion reports model GPU
 # arithmetic/log output and align their counters to the actual monitor.
 raw=list(reports(p));raw[0]=monitor
 for part,offset,values in [(1,64,(m[6],m[8])),(2,88,(m[8]+10000,)),(3,304,(m[6],m[8]))]:
  data=bytearray(raw[part]);struct.pack_into('<'+'Q'*len(values),data,offset,*values);raw[part]=bytes(data)
 source=Path(report.__file__).read_text();assert source.count('0x4153373347535232')==1
 old_source=source.replace('0x4153373347535232','0x4153373343525331')
 old=types.ModuleType('gpu_previous_report_magic');exec(compile(old_source,'previous-report.py','exec'),old.__dict__)
 try:old.validate_reports(*raw,p)
 except ValueError as error:assert str(error)=='No matched healthy GPU monitor completion.'
 else:raise AssertionError('Previous parser accepted the native monitor.')
 assert report.validate_reports(*raw,p)['functional_success']
 cases=['previous-magic-parser-refused-native-monitor','native-monitor-accepted']
 for name,part,offset,value in [('stale-monitor-magic',0,0,0x4153373343525331),('checkpoint-detail',0,40,1),('checkpoint-zero-job-packet',3,0,0x3150434555504741)]:
  bad=list(raw);data=bytearray(bad[part]);struct.pack_into('<Q',data,offset,value);bad[part]=bytes(data)
  try:report.validate_reports(*bad,p)
  except ValueError:cases.append(name+'-refused')
  else:raise AssertionError(name)
 result={'passed':True,'physical_access':False,'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),'monitor_sha256':hashlib.sha256(monitor).hexdigest(),'monitor_words':list(m),'cases':cases,'previous_magic_parser_sha256':hashlib.sha256(old_source.encode()).hexdigest(),'source_hashes':{str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in [Path(__file__),Path(report.__file__),ROOT/'apps/gpu-finite/return-check.py']},'scope':'One existing normal-return instruction model produces unchanged monitor bytes from the frozen ELF. GPU completion, MMIO, system registers, reset and companion arithmetic/log/receipt/packet reports are modeled. No physical execution or new admission.'}
 (evidence/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS native GPU monitor to host parser:',len(cases),'cases')

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--evidence',type=Path,required=True);selection=parser.add_mutually_exclusive_group();selection.add_argument('--disposition-only',action='store_true');selection.add_argument('--rom-entry-only',action='store_true');selection.add_argument('--writer-only',action='store_true');parser.add_argument('--image-evidence',type=Path,default=Path.home()/'.cache/sunstoneos/evidence/gpu-returning-repeat-fix-20261019');args=parser.parse_args();args.evidence.mkdir(parents=True,exist_ok=True)
 image=args.image_evidence/'build/return.elf';manifest=json.loads((args.image_evidence/'manifest.json').read_bytes());parts,address,build,g=host.prepare.image_profile(image,manifest)
 if args.writer_only:writer_check(image,manifest,g,args.evidence);return
 p={'epoch':101,'generation':102,'lease':103,'geometry':g};raw=reports(p);assert report.validate_reports(*raw,p)['functional_success'];cases={'matched-arithmetic-two-results':True}
 if not args.disposition_only and not args.rom_entry_only:
  for part,offset in [(0,x) for x in (0,8,16,24,32,40,56,64)]+[(1,x) for x in (0,8,16,24,32,71,72,256)]+[(2,x if x!=88 else 95) for x in range(0,104,8)]+[(3,x) for x in (0,8,12,16,24,32,36,40,44,48,300,311,312)]:
   bad=list(raw);data=bytearray(bad[part]);data[offset]^=1;bad[part]=bytes(data)
   try:report.validate_reports(*bad,p)
   except (ValueError,UnicodeError):cases[f'report-{part}-offset-{offset}']=True
   else:raise AssertionError((part,offset))
 # The I/O model keeps full declared lengths without allocating802MiB per case.
 # It exercises the actual GPU run_once; transport and file contents are modeled.
 original_base=host.base;original_hash=host.hashlib;original_report=host.report
 class Hash:
  def __init__(self,data):self.data=data
  def hexdigest(self):return f'{len(self.data)}:{self.data.tag}' if isinstance(self.data,Data) else hashlib.sha256(self.data).hexdigest()
 @contextlib.contextmanager
 def sealed(data):yield data,17
 variants=['normal','backup-short','backup-transport','preload-failure','rom-entry-mismatch','watchdog','exe-failure','no-new-rom','wrong-rom','short-report','bad-report','changed-CPU-backup','changed-GPU-backup','restore-transport','restore-readback','second-attempt','middle-backup-failure','middle-restore-failure']
 variants=[('restore',v) for v in variants]
 if args.disposition_only:variants=[('restore','normal')]+[('discard',v) for v in ('normal','preload-failure','exe-failure','no-new-rom','bad-report','second-attempt')]
 if args.rom_entry_only:variants=[('discard',v) for v in ('normal','rom-entry-mismatch','rom-entry-short','rom-entry-nonhex','watchdog')]
 for disposition,variant in variants:
  files={};e=Store('evidence',files);record={};commands=[];restores=[];returned=False;preloaded=[];prepared={**copy.deepcopy(p),'_backup_hashes':{},'_frozen':{'admission.json':json.dumps({'ram_disposition':disposition}).encode()}}
  def digest(path):return Hash(path.read_bytes()).hexdigest()
  def preload(*unused):
   preloaded.append(True)
   if variant=='preload-failure':raise ValueError('modeled preload fault')
   prepared['_backup_hashes']={'SRAM':'modeled-frozen-SRAM'}
  host.base=types.SimpleNamespace(sync_file=lambda p:None,digest=digest,preload=preload,sealed_source=sealed)
  host.hashlib=types.SimpleNamespace(sha256=Hash)
  def wait():
   nonlocal returned
   if variant=='no-new-rom':raise ValueError('modeled missing ROM')
   returned=True
  def tx(*v,pass_fds=()):
   commands.append(tuple(str(x) for x in v))
   if v[0]=='read':
    address,size,path=int(v[1],16),v[2],v[3]
    if not record.get('launch_attempted'):
     if variant=='backup-transport' or (variant=='middle-backup-failure' and address==0x48000000+100*host.CHUNK_BYTES):raise ValueError('modeled backup transport failure')
     path.write_bytes(Data(size-1 if variant=='backup-short' else size,'backup'+hex(address)))
    else:
     assert returned
     indexes={0x42000000:0,g['log']:1,g['receipt']:2,0x42000100:3};data=raw[indexes[address]]
     if variant=='short-report':data=data[:-1]
     if variant=='bad-report' and address==0x42000100:data=bytes(320)
     path.write_bytes(data)
     if variant in ('changed-CPU-backup','changed-GPU-backup'):
      name='CPU' if variant=='changed-CPU-backup' else 'GPU';key='evidence/before-'+name+'-0000.bin'
      files[key]=Data(len(files[key]),'changed')
    return ''
   if v[0]=='readl':
    if len(v)>2:
     assert v==('readl','0x20','readl','0x24','readl','0x28','readl','0x3c','readl','0x40')
     words=['eb00008f','e3a00911','e3a0193d','e3a0605c','ea00004d']
     if variant=='rom-entry-mismatch':words[0]='0'
     if variant=='rom-entry-short':words.pop()
     if variant=='rom-entry-nonhex':words[0]='invalid'
     return ' '.join(words)
    return '1' if variant=='watchdog' else '0'
   if v[0]=='exe':
    if variant=='exe-failure':raise ValueError('modeled launch failure')
    return ''
   if v[0]=='version':return 'wrong' if variant=='wrong-rom' else 'soc=00001903(A733)'
   if v[0]=='write':
    assert returned and record.get('gpu_result');address=int(v[1],16);data=v[2];size=v[5];dest=v[6];assert pass_fds==(17,) and size==len(data)
    if variant=='restore-transport' or (variant=='middle-restore-failure' and address==0x48000000+100*host.CHUNK_BYTES):raise ValueError('modeled restore transport failure')
    restores.append((address,size));dest.write_bytes(Data(size,'corrupt') if variant=='restore-readback' else data);return ''
   raise AssertionError(v)
  if variant=='second-attempt':record['launch_attempted']=True
  try:result=host.run_once(tx,wait,lambda:None,None,prepared,e,record,lambda:None)
  except ValueError:
   assert variant!='normal' and not record.get('complete') and not record.get('ownership_release_permitted'),variant
   if variant not in ('restore-readback','middle-restore-failure'):assert not restores,variant
  else:
   assert variant=='normal' and result['functional_success'] and record['complete'] and record['sram_restored'] is False
   assert record['dram_restored']==(disposition=='restore') and record['ram_disposition']==('restored' if disposition=='restore' else 'discarded')
   if disposition=='restore':
    assert restores==[(a+off,min(host.CHUNK_BYTES,n-off)) for _,a,n in host.CAPTURE_RANGES for off in range(0,n,host.CHUNK_BYTES)]
    assert len(restores)==201 and restores[4]==(0x42000000,2*1024*1024) and restores[5][0]==0x48000000 and restores[-1][0]+restores[-1][1]==0x79000000
   else:
    assert not restores and not record['dram_backups'] and not any('/before-' in n for n in files)
   assert len(commands)==(410 if disposition=='restore' else 8),len(commands)
  assert not any('0x03000024' in c for c in commands)
  if variant.startswith('rom-entry-'):assert preloaded==[True] and record['dirty'] and not record.get('launch_attempted')
  cases['capture-'+disposition+'-'+variant]=True
 host.base=original_base;host.hashlib=original_hash;host.report=original_report
 assert host.CAPTURE_RANGES==( ('CPU',0x41000000,0x1200000),('GPU',0x48000000,0x31000000))
 (args.evidence/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'physical_admission_created':False,'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),'cases':cases,'source_hashes':{str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in [ROOT/'apps/gpu-finite/host.py',ROOT/'apps/gpu-finite/prepare.py',ROOT/'apps/gpu-finite/report.py',Path(__file__)]},'selection':'rom-entry-only' if args.rom_entry_only else 'disposition-only' if args.disposition_only else 'all','scope':'Linked exact GPU image/profile layout; arithmetic/log/receipt/packet decoder mutations; actual run_once full declared save/restore addresses and lengths with modeled files/transport. No USB or physical admission. Accepted25native handoff cases reused.'},indent=2)+'\n');print('PASS GPU returning host:',len(cases),'modeled cases')
if __name__=='__main__':main()
