#!/usr/bin/env python3
"""Postprocess preserved encoder instruction states; never execute a codec library."""
import argparse,gzip,hashlib,json,pickle,struct
from pathlib import Path
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--checkpoint',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(exist_ok=False);state=pickle.loads(gzip.decompress(a.checkpoint.read_bytes()));mem=dict(state['memory']);ram=mem[0x10000000]
 def raw(at,n):return ram[at-0x10000000:at-0x10000000+n]
 def get(at,fmt='Q'):return struct.unpack('<'+fmt,raw(at,struct.calcsize(fmt)))[0]
 ctx=state['context'];manager=get(ctx+0x268);ring=get(ctx+0x298);descriptor=get(manager+0x60);headerbytes=get(ctx+0x14,'I');n=get(descriptor+4,'I');header=raw(ring,headerbytes);assert header[:2]==b'\xff\xd8' and headerbytes==623;segments=[];q={};huff={};at=2
 while at<len(header):
  assert header[at]==255;kind=header[at+1];nseg=int.from_bytes(header[at+2:at+4],'big');end=at+2+nseg;assert 2<=nseg and end<=len(header);body=header[at+4:end];segments.append(dict(marker=hex(kind),offset=at,bytes=2+nseg))
  if kind==0xdb:assert len(body)==65 and body[0] in [0,1];q[body[0]]=list(body[1:])
  if kind==0xc4:
   assert body[0] in [0,1,16,17];counts=body[1:17];assert len(body)==17+sum(counts);huff[body[0]]=dict(counts=list(counts),symbols=list(body[17:]))
  if kind==0xc0:assert body==bytes.fromhex('0801e0028003012200021101031101')
  at=end
 assert at==len(header) and segments[-1]['marker']=='0xda' and set(q)=={0,1} and set(huff)=={0,1,16,17};assert raw(ring+n-2,2)==b'\xff\xd9' and n==657
 writes=state['mmio'];registers={x['offset']:x['value'] for x in writes};quant=[x['value'] for x in writes if x['offset']==0xbe4];assert len(quant)==128
 # JPEG DQT transmits zigzag order; hardware reciprocal table is raster order.
 zig=[0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63];raster=[]
 for k in [0,1]:
  v=[0]*64
  for i,x in enumerate(q[k]):v[zig[i]]=x
  raster.extend(v)
 expected=[((x//2)<<16)|(65536//x+1) for x in raster];assert quant==expected
 assert registers[0xb80]==ring>>8 and registers[0xb84]==(ring+1048576-1)>>8;assert registers[0xb88]==headerbytes*8 and registers[0xb8c]==1048576*8 and registers[0xb18]==0x10008
 assert registers[0xa78]==0x10800000 and registers[0xa7c]==0x1084b000 and registers[0xa80]==0x1085dc00
 assert [x['bytes'] for x in state['allocations'] if 'Palloc' in x['kind']]==[1048576]
 (a.output/'cpu-header.bin').write_bytes(header);(a.output/'quant-sram.bin').write_bytes(struct.pack('<128I',*quant))
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,checkpoint=dict(path=str(a.checkpoint),sha256=hashlib.sha256(a.checkpoint.read_bytes()).hexdigest(),stage=state['stage']),libraries=state['libraries'],inputs=state['inputs'],header_bytes=headerbytes,modeled_frame_bytes=n,segments=segments,quant_dqt=q,huffman=huff,registers={hex(k):v for k,v in registers.items() if k not in [0xbe0,0xbe4]},allocations=state['allocations'],physical_access=False,scope='Postprocessing only: actual CPU JPEG header syntax, raster reciprocal table and encoder/input/output extent programming from preserved instructions. 32 entropy bytes and absolute bit count were explicitly modeled. No JPEG arithmetic, hardware burst bound, physical visibility or drain qualification.'),indent=2)+'\n');print('PASS preserved header/table/input-output extent facts')
if __name__=='__main__':main()
