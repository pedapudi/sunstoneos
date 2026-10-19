"""Value-only selected-panel producer; no defaults or physical authority."""
import hashlib,struct
MAGIC=0x3153494433375341
IDS=('owner','board_id','panel_id','authority','pll_profile_id','pll_profile_generation','reference_generation','rail_generation','inactive_epoch','recovery_generation','pck_client')
VALUES=('pixel_hz','hs_hz','ls_hz','width','height','hsync','hback','hfront','vsync','vback','vfront','lanes','format','mode','actual_dclk_hz','pitch','initial_hs_hz','initial_ls_hz')
ROLES={'lane':0,'power':1,'reset':2,'backlight':3}
def require(ok,message):
    if not ok:raise ValueError(message)
def encode(board,profile,facts):
    require(len(board)==592 and len(facts)==4 and all(len(v)==32 and any(v) for v in facts),'Exact board prefix and four evidence digests required')
    require(set(profile)=={'schema','ids','values','initial_words','timeout_us','seed','fetch_padding','supplies','pins','steps'} and profile['schema']=='a733-dsi-panel-values-v1','Selected value-only profile required')
    require(set(profile['ids'])==set(IDS) and set(profile['values'])==set(VALUES),'Profile fields differ')
    raw=bytearray(4096);raw[:592]=board
    ids=[profile['ids'][k] for k in IDS];require(all(type(v) is int and 0<=v<2**64 for v in ids),'Invalid profile identities')
    struct.pack_into('<14Q',raw,592,MAGIC,1,4096,*ids);raw[704:832]=b''.join(facts)
    values=[profile['values'][k] for k in VALUES];words=profile['initial_words'];require(len(words)==7,'Seven qualified initial PLL words required')
    pins,supplies,steps=profile['pins'],profile['supplies'],profile['steps'];require(10<=len(pins)<=16 and 0<len(supplies)<=8 and 0<len(steps)<=48,'Profile exceeds admitted bounds')
    payload=bytearray();instructions=[]
    for step in steps:
        phase={'before-video':0,'after-video':1}[step['phase']]
        if step['op']=='gpio':
            require(set(step)=={'op','phase','role','level'},'GPIO fields differ');instruction=(1,ROLES[step['role']],0,phase,0,0,step['level'],0)
        elif step['op']=='delay':
            require(set(step)=={'op','phase','us'},'Delay fields differ');instruction=(2,0,0,phase,0,0,step['us'],0)
        elif step['op']=='packet':
            require(set(step)=={'op','phase','type','hex'},'Packet fields differ');data=bytes.fromhex(step['hex']);require(len(data)<=250,'Packet exceeds command SRAM');instruction=(3,0,step['type'],phase,len(payload),len(data),0,0);payload.extend(data)
        else:raise ValueError('Unsupported panel operation')
        instructions.append(instruction)
    require(len(payload)<=1536,'Panel payload exceeds sealed capacity')
    values+=words+[profile['timeout_us'],profile['seed'],profile['fetch_padding'],len(steps),len(payload),len(pins),len(supplies)]
    require(len(values)==32 and all(type(v) is int and 0<=v<2**32 for v in values),'Invalid panel scalar')
    struct.pack_into('<32I',raw,832,*values)
    for i,s in enumerate(supplies):require(set(s)=={'rail','uv'},'Supply fields differ');struct.pack_into('<2I',raw,960+i*8,s['rail'],s['uv'])
    for i,p in enumerate(pins):
        require(set(p)=={'bank','pin','expected','desired','output','role'} and len(p['expected'])==len(p['desired'])==3,'Pin fields differ')
        struct.pack_into('<16B',raw,1024+i*16,ord(p['bank'])-ord('A'),p['pin'],*p['expected'],*p['desired'],p['output'],ROLES[p['role']],*bytes(6))
    for i,instruction in enumerate(instructions):struct.pack_into('<4B2H2I',raw,1280+i*16,*instruction)
    raw[2048:2048+len(payload)]=payload
    return bytes(raw)
def value_digest(session):return hashlib.sha256(session[592:704]+session[832:]).hexdigest()
def frame_bytes(session):
    v=struct.unpack_from('<32I',session,832);return (v[15]*v[4]+v[27]+4095)&~4095

def frame_digest(session):
    v=struct.unpack_from('<32I',session,832);w,h,pitch,seed=v[3],v[4],v[15],v[26]
    require(0<w<=1920 and 0<h<=1080 and pitch>=w*4 and frame_bytes(session)<=0x1000000-32768,'Frame exceeds native capacity')
    colors=(0xffffff,0xffff00,0xffff,0xff00,0xff00ff,0xff0000,0xff,0)
    row=b''.join(struct.pack('<I',colors[(x*8//w+seed)&7]) for x in range(w))+bytes(pitch-w*4)
    digest=hashlib.sha256()
    for unused in range(h):digest.update(row)
    tail=frame_bytes(session)-pitch*h
    while tail:count=min(tail,65536);digest.update(bytes(count));tail-=count
    return digest.digest()
if __name__=='__main__':
    import argparse,json
    from pathlib import Path
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('board','profile','initial','memory','electrical','panel','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();facts=[hashlib.sha256(getattr(a,k).read_bytes()).digest() for k in ('initial','memory','electrical','panel')]
    raw=encode(a.board.read_bytes(),json.loads(a.profile.read_text()),facts)
    a.output.write_bytes(raw)
