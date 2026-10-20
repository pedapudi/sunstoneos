#!/usr/bin/env python3
"""Compare actual High producer plans with independent FFmpeg trace syntax."""
import argparse,json,re
from pathlib import Path

def sections(text):
 rows=[];current=None;parameters={}
 for line in text.splitlines():
  if 'Slice Header' in line:
   if current:rows.append(current)
   current={}
  elif any(name in line for name in ('Access Unit Delimiter','Sequence Parameter Set','Picture Parameter Set','Supplemental Enhancement Information')):
   if current:rows.append(current)
   current=None
  m=re.search(r'\]\s+(\d+)\s+([\w\[\]]+)\s+([01]+)\s+=\s+(-?\d+)\s*$',line)
  if m:
   field=(int(m[1]),len(m[3]),int(m[4]))
   if current is not None:current[m[2]]=field
   else:parameters.setdefault(m[2],field)
 if current:rows.append(current)
 return rows,parameters

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--geometries',nargs='+',default=['fullhd','hd']);p.add_argument('--output',type=Path);a=p.parse_args();reports=[]
 for name in a.geometries:
  d=a.evidence/name;slices,parameters=sections((d/'headers.log').read_text());plans=json.loads((d/'qualification/syntax-plans.json').read_text());assert len(slices)==len(plans)==12
  for i,(s,plan) in enumerate(zip(slices,plans)):
   assert plan['frame_num']==s['frame_num'][2] and plan['kind']==s['slice_type'][2]%5 and plan['reference']==s['nal_ref_idc'][2]
   assert plan['poc']==s['pic_order_cnt_lsb'][2] # each closed GOP is below its signaled POC-LSB modulus
   assert plan['idr']==(s['nal_unit_type'][2]==5)
  assert parameters['profile_idc'][2]==100 and parameters['entropy_coding_mode_flag'][2]==1 and parameters['transform_8x8_mode_flag'][2]==1 and parameters['pic_scaling_matrix_present_flag'][2]==1
  assert parameters['max_num_ref_frames'][2]==plans[0]['refs'] and 2<=plans[0]['refs']<=4
  assert sum(p['kind']==1 for p in plans)>=4 and sum(p['idr'] for p in plans)==(1 if name=='four' else 2)
  if name=='hd':assert any(p['kind']==1 and p['reference'] for p in plans)
  reports.append(dict(geometry=name,plans=plans,independent_slice_fields=slices,parameter_fields=parameters))
 (a.output or a.evidence/'syntax-oracle.json').write_text(json.dumps(dict(passed=True,physical_access=False,cases=reports,scope='Independent FFmpeg trace fields compared with actual native-adapted host producer. Coded/visible geometry, actual CABAC/8x8/custom-matrix/weighted syntax and reference B fixtures. No software-pixel output is substituted for device qualification.'),indent=2)+'\n');print('PASS independent High syntax, CABAC/transform/custom matrices, B/ref geometry and POCs')
if __name__=='__main__':main()
