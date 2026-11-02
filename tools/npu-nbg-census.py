"""Census of the wire features of compiled network binary graphs: operation kinds, the patch
destinations and sources used by each kind, the load-state packet headers and the shader operations.
It uses the OS decoder (drivers/npu/nbg_*.c) through checks/nbg-model.py and reads nothing else.
"""
import importlib.util,sys,collections,struct,json,ctypes as c
from pathlib import Path
R=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('nm',R/'checks/nbg-model.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
LIB=None
KIND={1:'SH',2:'NN',6:'SP',7:'ACT',8:'NT',65535:'INIT'}
def census(path):
    nb=Path(path).read_bytes()
    n=m.Native(Path(LIB),nb)
    ops=n.operations();pat=n.patches()
    def table(idx,row):
        sp=m.Span();assert n.lib.nbg_model_table(c.byref(n.view),idx,row,c.byref(sp))==1;return c.string_at(sp.data,sp.bytes)
    out={'file':str(path),'version':hex(n.view.header.version),'pool':n.view.header.pool_bytes,'vip_sram':n.view.header.vip_sram_bytes,'axi_sram':n.view.header.axi_sram_bytes,
         'sections':{i:n.view.sections[i].bytes for i in range(36) if n.view.sections[i].bytes}}
    kinds=collections.Counter(KIND.get(o[0],o[0]) for o in ops);out['ops']=dict(kinds)
    feat=collections.defaultdict(collections.Counter)
    heads=collections.defaultdict(collections.Counter)
    for o in ops:
        k=KIND.get(o[0],o[0])
        for p in pat[o[4]:o[4]+o[5]]:
            feat[k][('cmd' if p[0]==0 else 'desc',p[1],p[2])]+=1
        b=table(26,o[3]);w=struct.unpack('<%dI'%(len(b)//4),b)
        # packet headers: words that look like 0x0801xxxx / 0x0800xxxx
        for x in w:
            if (x>>16)==0x0801 or (x>>16)==0x0800 or (x>>20)==0x080: heads[k][hex(x)]+=1
    out['patch_features']={k:{'%s dest=%d src=%d'%f:v for f,v in c_.items()} for k,c_ in feat.items()}
    out['state_headers']={k:dict(v.most_common(12)) for k,v in heads.items()}
    out['state_bytes']={k:sorted({len(table(26,o[3])) for o in ops if KIND.get(o[0],o[0])==k}) for k in kinds}
    sh=[o for o in ops if o[0]==1]
    out['sh']=[{'op':o[:6],'aux':o[6:],'state_bytes':len(table(26,o[3])),'patches':[p for p in pat[o[4]:o[4]+o[5]]]} for o in sh]
    return out
if __name__=='__main__':
    import argparse,subprocess,tempfile
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--native',type=Path,help='shared library of the decoder; built from drivers/npu when absent')
    ap.add_argument('--work',type=Path,required=True,help='new directory for the library')
    ap.add_argument('graphs',nargs='+',type=Path)
    a=ap.parse_args()
    a.work.mkdir(parents=True,exist_ok=False)
    LIB=a.native or a.work/'nbg.so'
    if not a.native:
        subprocess.run(['/usr/bin/clang-21','-shared','-fPIC','-O1','-I'+str(R/'include'),'-I'+str(R/'drivers/npu'),'-o',str(LIB),*(str(R/'drivers/npu'/f) for f in ('nbg_model.c','nbg_checksum.c','nbg_relocation.c'))],check=True)
    for p in a.graphs:
        print(json.dumps(census(p),indent=1,default=str)[:6000]); print('=====')
