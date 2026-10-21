#!/usr/bin/env python3
"""Compare native SRAM/reference traces with independent FFmpeg syntax and DPB arithmetic."""
import argparse,json,re,struct
from pathlib import Path
def trace(path):
 slices=[];params={};current=None
 for line in path.read_text().splitlines():
  if 'Slice Header' in line:
   current=[];slices.append(current)
  elif any(x in line for x in ('Access Unit Delimiter','Sequence Parameter Set','Picture Parameter Set','Supplemental Enhancement Information')):current=None
  m=re.search(r'\]\s+\d+\s+([\w\[\]]+)\s+[01]+\s+=\s+(-?\d+)\s*$',line)
  if m:
   item=(m[1],int(m[2]))
   if current is None:params.setdefault(*item)
   else:current.append(item)
 return slices,params
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--evidence',type=Path,required=True);p.add_argument('--native',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();reports=[]
 for case in json.loads((a.native/'result.json').read_text())['cases']:
  if not case['case'].endswith('-qualification'):continue
  name=case['case'].rsplit('-',1)[0];slices,parameters=trace(a.evidence/name/'headers.log');refs=[];proof=[]
  w=16*(parameters['pic_width_in_mbs_minus1']+1);h=16*(parameters['pic_height_in_map_units_minus1']+1);field=((w//16*((h//16+1)//2)*32+1023)//1024)*1024;mvstep=((2*field+4095)//4096)*4096;ystep=(w*h+4095)//4096*4096;cstep=(w*h//2+4095)//4096*4096;mod=1<<(parameters['log2_max_frame_num_minus4']+4)
  # Retained-failure branches execute from the same pre-picture checkpoint.
  # Select the successful path's last trigger for each decode ordinal; branch
  # triggers remain preserved in the native report, not counted as new pictures.
  triggers={tr['picture']:tr for tr in case['triggers']};assert sorted(triggers)==list(range(len(slices)))
  for i,fields in enumerate(slices):
   tr=triggers[i]
   s=dict(fields);r={int(k):v for k,v in tr['registers'].items()};ram=tr['sram'];kind=s['slice_type']%5;poc=s['pic_order_cnt_lsb'];number=s['frame_num'];slot=r[0x24c]
   if s['nal_unit_type']==5:refs=[]
   assert slot not in [x['slot'] for x in refs]
   def picnum(x):return x['frame'] if x['frame']<=number else x['frame']-mod
   if kind==0:lists=[sorted(refs,key=picnum,reverse=True),[]]
   elif kind==1:
    lower=sorted([x for x in refs if x['poc']<poc],key=lambda x:x['poc'],reverse=True);higher=sorted([x for x in refs if x['poc']>=poc],key=lambda x:x['poc']);lists=[lower+higher,higher+lower]
    if len(refs)>1 and lists[0]==lists[1]:lists[1][0],lists[1][1]=lists[1][1],lists[1][0]
   else:lists=[[],[]]
   counts=[]
   for l in range(2):
    active=0 if kind==2 or (l==1 and kind!=1) else (s[f'num_ref_idx_l{l}_active_minus1']+1 if s.get('num_ref_idx_active_override_flag') else parameters[f'num_ref_idx_l{l}_default_active_minus1']+1);counts.append(active)
    if s.get(f'ref_pic_list_modification_flag_l{l}'):
     begin=next(j for j,(k,v) in enumerate(fields) if k==f'ref_pic_list_modification_flag_l{l}')+1;prediction=number;at=0
     for j in range(begin,len(fields)):
      key,value=fields[j]
      if key=='modification_of_pic_nums_idc':
       if value==3:break
       assert value in (0,1) and fields[j+1][0]=='abs_diff_pic_num_minus1';delta=fields[j+1][1]+1;prediction=(prediction+(-delta if value==0 else delta))%mod;target=prediction if prediction<=number else prediction-mod;x=next(x for x in refs if picnum(x)==target);lists[l].insert(at,x);at+=1;lists[l]=lists[l][:at]+[v for v in lists[l][at:] if picnum(v)!=target]
    assert len(lists[l])>=active
    expected=bytes([2*x['slot'] for x in lists[l][:active]]+[0]*(32-active));assert struct.pack('<8I',*ram[400+9*l:408+9*l])==expected,(name,i,l)
   assert r[0x20c]>>24&31==max(0,counts[0]-1) and r[0x20c]>>16&31==max(0,counts[1]-1)
   for x in refs+[dict(slot=slot,poc=poc)]:
    k=x['slot'];assert ram[256+8*k:258+8*k]==[x['poc']&0xffffffff]*2,(name,i,x,ram[256+8*k:258+8*k])
    assert ram[259+8*k:263+8*k]==[0x10201000+k*ystep,0x109fa000+k*cstep,0x10e30000+k*mvstep,0x10e30000+k*mvstep+field]
   assert r[0xc8]==w|(w//2<<16) and r[0xc4]==w*h//4
   implicit=kind==1 and parameters['weighted_bipred_idc']==2
   explicit=(kind==0 and parameters['weighted_pred_flag']) or (kind==1 and parameters['weighted_bipred_idc']==1)
   ld=5 if implicit else s.get('luma_log2_weight_denom',0);cd=5 if implicit else s.get('chroma_log2_weight_denom',0);assert r[0x210]==ld|(cd<<4)
   if implicit or explicit or kind==2:
    expected=[]
    for l in range(2):
     for j in range(32):
      weight=s.get(f'luma_weight_l{l}[{j}]',1<<ld) if explicit and j<counts[l] else 1<<ld;offset=s.get(f'luma_offset_l{l}[{j}]',0) if explicit and j<counts[l] else 0;expected.append((weight&511)|((offset&511)<<16))
     for j in range(32):
      for c in range(2):
       weight=s.get(f'chroma_weight_l{l}[{j}][{c}]',1<<cd) if explicit and j<counts[l] else 1<<cd;offset=s.get(f'chroma_offset_l{l}[{j}][{c}]',0) if explicit and j<counts[l] else 0;expected.append((weight&511)|((offset&511)<<16))
    assert ram[:192]==expected,(name,i,'weights')
   if s['nal_ref_idc']:
    if s.get('adaptive_ref_pic_marking_mode_flag'):
     for j,(key,value) in enumerate(fields):
      if key=='memory_management_control_operation' and value:
       assert value==1 and fields[j+1][0]=='difference_of_pic_nums_minus1';target=number-fields[j+1][1]-1;old=len(refs);refs=[x for x in refs if picnum(x)!=target];assert len(refs)==old-1
    elif len(refs)==parameters['max_num_ref_frames']:refs.remove(min(refs,key=picnum))
    refs.append(dict(frame=number,poc=poc,slot=slot))
   proof.append(dict(decode=i,poc=poc,slot=slot,list0=[x['poc'] for x in lists[0][:counts[0]]],list1=[x['poc'] for x in lists[1][:counts[1]]]))
  assert len(proof)==len(slices);reports.append(dict(geometry=name,retained_branch_triggers=len(case['triggers'])-len(triggers),cases=proof))
 a.output.write_text(json.dumps(dict(passed=True,physical_access=False,cases=reports,scope='Independent FFmpeg header syntax drives separate short-term DPB/default/list-modification/marking arithmetic; compared to native SRAM lists/POCs/MV extents, coded stride registers, explicit P weights and implicit B weights. Fixtures do not exercise POC wrap or MMCO5. No device arithmetic or physical reference visibility claim.'),indent=2)+'\n');print('PASS independent reference list/POC/MV/stride register oracle')
if __name__=='__main__':main()
