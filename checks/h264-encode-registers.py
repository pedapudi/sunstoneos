#!/usr/bin/env python3
"""Compare native programming with preserved effective A733 instruction traces."""
import argparse,hashlib,json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--evidence',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();r=a.evidence
native=json.loads((r/'native-nominal-count-width/result.json').read_text());ref=json.loads((r/'p4m-stable/result.json').read_text())
n=[(o,v) for o,v in native['programmed'] if o>=0xa00 and o!=0xb1c];s=[(x['offset'],x['value']) for x in ref['mmio'] if x['offset']!=0xb1c]
# Relocate only independently identified input/reference/ring/scratch addresses.
source={0x12000000:0x10001000,0x12100000:0x10074000,0x10114000:0x10400000,0x1053d000:0x10801000,0x105b5000:0x1087b000,0x1062d000:0x108f5000,0x10630000:0x108fa000,0x10633000:0x108ff000,0x10635000:0x10902000,0x10637000:0x10905000}
shifted={0xb80,0xb84,0xb9c,0xba0,0xba4,0xbb0,0xbb4,0xbb8,0xbbc,0xbc0,0xbc4}
full={0xa78,0xa7c,0xa80}
normalized=[]
for off,value in s:
 if off in shifted|full:
  value=value<<8 if off in shifted else value
  # Ring's end register represents its final256-byte block.
  extents=[(start,4194304 if start==0x10114000 else 491520 if start in (0x1053d000,0x105b5000) else 460800 if start in (0x12000000,0x12100000) else 11040 if start in (0x1062d000,0x10630000) else 5120 if start in (0x10633000,0x10635000) else 860) for start in source]
  matches=[(start,source[start]) for start,size in extents if start<=value<start+size];assert len(matches)==1,(hex(off),hex(value),matches)
  old,new=matches[0];value=new+value-old
  if off in shifted:value>>=8
 normalized.append((off,value))
assert n==normalized,[(i,tuple(map(hex,x)),tuple(map(hex,y))) for i,(x,y) in enumerate(zip(n,normalized)) if x!=y]
facts=json.loads((r/'facts/result.json').read_text());assert facts['frames'][0]['sps_pps']=='000000016742001f898940501e880000000168ce3c80'
result=dict(passed=True,native_elf=native['image_sha256'],compared_program_writes=len(n),reference_sha256=hashlib.sha256((r/'p4m-stable/result.json').read_bytes()).hexdigest(),physical_access=False,scope='Pure comparison of preserved traces, no decoder/encoder execution. All ENCPP/H264/EXT programming and partial-header commands match effective library after independently mapped DMA relocation. Native TOP reset/enable follows separately reviewed libVE sequence. Status W1C differs intentionally: native clears only observed status. Entropy/completion and reconstructed reference contents remain modeled.')
a.output.write_text(json.dumps(result,indent=2)+'\n');print('PASS',len(n),'source-matched programming writes')
