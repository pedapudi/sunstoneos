#!/usr/bin/env python3
"""Check SS keyboard image/class admission without physical authority or access."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','old-image','session','output'):p.add_argument('--'+n,type=Path,required=True)
 p.add_argument('--topology',action='store_true')
 a=p.parse_args();s=importlib.util.spec_from_file_location('ss_hid_prepare',ROOT/'apps/board/prepare.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
 session=bytearray(a.session.read_bytes());assert m.session_check(session) in ((8,10,12) if a.topology else (8,))
 raw=a.image.read_bytes();(m.usb3_topology_image_check if a.topology else m.usb3_hid_image_check)(raw)
 a.output.mkdir(parents=True,exist_ok=False);passed=['current-immutable-marker']
 # Actual preparation must reject an old image even if its manifest asserts
 # the new capability. This refusal precedes physical authority processing.
 old=a.old_image.read_bytes();
 if a.topology:struct.pack_into('<II',session,400,10,0)
 manifest=dict(target='sunstone-a733-board',execution_profile='board-supervised-v2',final_elf=hashlib.sha256(old).hexdigest(),usb3_gen1=True,usb3_hid=True)
 assert m.image_manifest_check(manifest,old)=='board-supervised-v2'
 for name,data in (('manifest.json',json.dumps(manifest).encode()),('admission.json',b'{}'),('session.bin',session)):(a.output/name).write_bytes(data)
 try:m.prepare(a.old_image,a.output/'manifest.json',a.output/'admission.json',a.output/'session.bin',a.output/'must-not-exist','unused-clang','unused-lld')
 except ValueError as ex:assert 'capability is absent' in str(ex),str(ex);passed.append('old-msc-image-with-asserted-hid-manifest-refused')
 else:raise AssertionError('Old MSC image admitted as keyboard')
 assert not (a.output/'must-not-exist').exists()
 # Authenticated physical evidence is an external prerequisite. These model
 # records exercise only the post-authentication class/capacity helper.
 path='/model-only/ss-hid-class.json';record={'superspeed':{'path':path}}
 for name,kind,capacity,accepted in [('keyboard','hid-keyboard',900,True),('msc','msc',900,False),('missing-class',None,900,False),('underqualified','hid-keyboard',500,False)]:
  value={'qualified_source_ma':capacity}
  if a.topology:value.update(root_protocol='usb2-companion',topology='direct')
  if kind is not None:value['device_class']=kind
  try:m.superspeed_source_check(record,{path:json.dumps(value).encode()},session)
  except ValueError:assert not accepted,name
  else:assert accepted,name
  passed.append(name)
 if a.topology:
  for name,root,topology,field,accepted in [('companion','usb2-companion','direct',0,True),('wrong-root','super-gen1','direct',0,False),('wrong-topology','usb2-companion','self-powered-hub',0,False),('hub','usb2-companion','self-powered-hub',1,True)]:
   struct.pack_into('<I',session,404,field);value=dict(qualified_source_ma=900,device_class='hid-keyboard',root_protocol=root,topology=topology)
   try:m.superspeed_source_check(record,{path:json.dumps(value).encode()},session)
   except ValueError:assert not accepted,name
   else:assert accepted,name
   passed.append(name)
  for workload,topology,accepted in [(10,2,False),(4,1,False),(9,0,False),(11,0,False),(12,1,True)]:
   struct.pack_into('<II',session,400,workload,topology)
   try:m.session_check(session)
   except ValueError:assert not accepted,(workload,topology)
   else:assert accepted,(workload,topology)
   passed.append('typed-%d-%d'%(workload,topology))
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=passed,image_sha256=hashlib.sha256(raw).hexdigest(),old_image_sha256=hashlib.sha256(old).hexdigest(),scope='Actual immutable ELF marker and production preparation old-image refusal; class/capacity helper after external authority authentication. Model values are not physical admission. No bundle or launcher created.'),indent=2)+'\n')
if __name__=='__main__':main()
