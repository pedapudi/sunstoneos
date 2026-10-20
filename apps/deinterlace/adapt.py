#!/usr/bin/env python3
"""Adapt pinned DI301 configuration code to a private CPU register image."""
import argparse,hashlib,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def block(text,name):
    start=re.search(r'(?:const char \*|u32 )'+name+r'\(',text).start();a=text.index('{',start);n=a+1;depth=1
    while depth:depth+=(text[n]=='{')-(text[n]=='}');n+=1
    return text[start:n]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cache',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    pins=json.loads((ROOT/'apps/deinterlace/sources.json').read_text())['reference_pins'];values={}
    for name,pin in pins.items():
        raw=(a.cache/name).read_bytes();assert hashlib.sha256(raw).hexdigest()==pin['sha256'],name;values[Path(name).name]=raw.decode()
    names=['di301.c','di301_alg.c','di301_alg.h','di301_reg.h','di_client.h','sunxi_di.h','di_utils.h','di30x.h','drm_fourcc.h']
    for name in names:
        text=values[name]
        # Upstream register masks use signed1<<31; preserve their intended
        # 32-bit bit patterns without C signed-shift undefined behavior.
        if name.endswith('.c'):text=re.sub(r'\b1(\s*<<)',r'1U\1',text)
        text=re.sub(r'^#include [^\n]+\n','',text,flags=re.M)
        dependencies={'di301.c':['di30x.h','di301_reg.h','di301_alg.h','di_client.h','di_utils.h'],'di301_alg.c':['di301_alg.h'],'di301_alg.h':['di_client.h'],'di_client.h':['sunxi_di.h','di_utils.h'],'di30x.h':['sunxi_di.h'],'sunxi_di.h':['drm_fourcc.h'],'di_utils.h':['sunxi_di.h']}.get(name,[])
        text='#include "di-port.h"\n'+''.join('#include "'+v+'"\n' for v in dependencies)+text
        (a.output/name).write_text(text)
    raw=values['di_utils.c'];notice=raw[:raw.index('#include')]
    (a.output/'di-formats.c').write_text(notice+'#include "di-port.h"\n#include "di_utils.h"\n'+ '\n'.join(block(raw,n) for n in ['di_format_to_string','di_format_get_planar_num','di_format_get_uvseq'])+'\n')
if __name__=='__main__':main()
