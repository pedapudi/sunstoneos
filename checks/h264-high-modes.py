#!/usr/bin/env python3
"""High-build long-term references, MMCO5, POC/frame_num wrap and profiles.

libx264 never emits long-term marking, memory-management operation 5 or short
POC/frame_num moduli, so this check makes such streams by rewriting the slice
headers (and, for wrap, the SPS) of small libx264 High/CABAC clips. Slice data
is copied unchanged after re-aligning to the CABAC byte boundary; the decoded
pixels therefore drift from the source, but each rewritten stream stays valid.
FFmpeg must decode every nominal stream with error detection set to explode.
FFmpeg's trace_headers syntax then drives an independent Python model of POC,
initial lists, list modification and reference marking (8.2.1, 8.2.4, 8.2.5).
The actual sanitized native syntax producer must match that model picture by
picture, or refuse exactly where the model says a case must be refused.
Baseline and Main clips are used unchanged, and an SPS relabelled as Extended
profile must be refused. apps/h264/high.py then builds the sealed manifest
from each nominal stream.
No device, board, USB or pixel decoding is involved.
"""
import argparse,hashlib,importlib.util,json,os,re,subprocess
from pathlib import Path

# ---- Annex B and RBSP bit handling -------------------------------------------------
def nal_units(data):
    starts=list(re.finditer(b'\x00\x00\x01',data));out=[]
    for i,m in enumerate(starts):
        end=starts[i+1].start() if i+1<len(starts) else len(data)
        out.append(data[m.end():end].rstrip(b'\0'))
    return out
def rbsp(nal):
    out=bytearray();zeros=0
    for b in nal:
        if zeros>=2 and b==3:zeros=0;continue
        out.append(b);zeros=zeros+1 if b==0 else 0
    return bytes(out)
def escape(raw):
    out=bytearray();zeros=0
    for b in raw:
        if zeros>=2 and b<=3:out.append(3);zeros=0
        out.append(b);zeros=zeros+1 if b==0 else 0
    if out and out[-1]==0:out.append(3)
    return bytes(out)
def bits(raw):return ''.join(f'{b:08b}' for b in raw)
def pack(text):
    assert len(text)%8==0;return bytes(int(text[i:i+8],2) for i in range(0,len(text),8))
def ue(v):b=bin(v+1)[2:];return '0'*(len(b)-1)+b
def se(v):return ue(2*v-1 if v>0 else -2*v)
def u(n,v):assert 0<=v<1<<n;return format(v,f'0{n}b') if n else ''
class Reader:
    def __init__(self,text,at=0):self.t=text;self.p=at
    def u(self,n):v=int(self.t[self.p:self.p+n],2) if n else 0;self.p+=n;return v
    def ue(self):
        z=0
        while self.t[self.p+z]=='0':z+=1
        v=int(self.t[self.p+z:self.p+2*z+1],2)-1;self.p+=2*z+1;return v
    def se(self):v=self.ue();return (v+1)//2 if v&1 else -(v//2)

# ---- Parameter sets and slice headers (High, frame-only, one slice group) ------------
def parse_sps(text):
    r=Reader(text,8);profile=r.u(8);r.u(16);r.ue();s=dict(profile=profile)
    if profile in (100,110,122,244,44,83,86,118,128,138,139,134,135):
        assert r.ue()==1;r.ue();r.ue();r.u(1)
        if r.u(1):
            for i in range(8):
                if r.u(1):
                    last=nxt=8
                    for _ in range(16 if i<6 else 64):
                        if nxt:nxt=(last+r.se()+256)%256
                        last=nxt or last
    s['frame_at']=r.p;s['frame_bits']=r.ue()+4;s['poc_type']=r.ue()
    assert s['poc_type'] in (0,2)
    if s['poc_type']==0:s['poc_bits']=r.ue()+4
    s['rest_at']=r.p;s['refs']=r.ue();r.u(1);r.ue();r.ue();s['frame_mbs_only']=r.u(1);return s
def rewrite_sps(text,sps,frame_bits,poc_bits):
    stop=text.rindex('1');body=text[:sps['frame_at']]+ue(frame_bits-4)+ue(0)+ue(poc_bits-4)+text[sps['rest_at']:stop]+'1'
    return body+'0'*(-len(body)%8)
def parse_pps(text):
    r=Reader(text,8);r.ue();r.ue();p=dict(cabac=r.u(1),bottom=r.u(1));assert r.ue()==0
    p['l0']=r.ue()+1;p['l1']=r.ue()+1;p['weighted']=r.u(1);p['bipred']=r.u(2);r.se();r.se();r.se()
    p['deblock']=r.u(1);r.u(1);p['redundant']=r.u(1);assert p['cabac'] and not p['redundant'];return p
def parse_slice(text,sps,pps):
    """Split a slice header into named spans so selected fields can be replaced."""
    r=Reader(text,0);r.u(1);ref=r.u(2);kind=r.u(5);r.ue();t=r.ue()%5;r.ue();assert sps['frame_mbs_only'] and t in (0,1,2)
    h=dict(ref=ref,idr=kind==5,type=t);cut=[('head',0,r.p)]
    at=r.p;h['frame_num']=r.u(sps['frame_bits']);cut.append(('frame_num',at,r.p));at=r.p
    if h['idr']:r.ue()
    cut.append(('idr',at,r.p));at=r.p
    if sps['poc_type']==0:
        h['poc_lsb']=r.u(sps['poc_bits'])
        if pps['bottom']:r.se()
    cut.append(('poc',at,r.p));at=r.p
    if t==1:r.u(1)
    if t!=2 and r.u(1):r.ue();t==1 and r.ue()
    cut.append(('active',at,r.p));at=r.p
    for l in range(2 if t==1 else 1 if t==0 else 0):
        if r.u(1):
            while True:
                idc=r.ue()
                if idc==3:break
                r.ue()
    cut.append(('modification',at,r.p));at=r.p
    assert not ((pps['weighted'] and t==0) or (pps['bipred']==1 and t==1)),'explicit weights are not rewritten'
    if ref:
        if h['idr']:r.u(2)
        elif r.u(1):
            while True:
                op=r.ue()
                if not op:break
                if op in (1,2,3,4):r.ue()
                if op in (3,6):r.ue()
    cut.append(('marking',at,r.p));at=r.p
    if t!=2:r.ue()
    r.se()
    if pps['deblock'] and r.ue()!=1:r.se();r.se()
    cut.append(('tail',at,r.p));end=r.p+(-r.p%8);assert set(text[r.p:end])<={'1'}
    h['spans']=cut;h['data']=end;return h
def emit_slice(text,h,sps,edit):
    out=''
    for name,a,b in h['spans']:out+=edit.get(name,text[a:b])
    return out+'1'*(-len(out)%8)+text[h['data']:]
def modification(entries):
    out='1'
    for e in entries:out+=ue(e[0])+ue(e[1])
    return out+ue(3)
def adaptive(ops):
    out='1'
    for op in ops:
        out+=ue(op[0])
        if op[0] in (1,3):out+=ue(op[1])
        if op[0] in (2,4,6):out+=ue(op[1])
        if op[0]==3:out+=ue(op[2])
    return out+ue(0)

# ---- Stream variants -------------------------------------------------------------
def x264(ffmpeg,path,frames,params,profile='high'):
    command=[ffmpeg,'-hide_banner','-loglevel','error','-f','lavfi','-i','testsrc2=size=320x240:rate=6','-frames:v',str(frames),
        '-c:v','libx264','-profile:v',profile,'-level:v','4.0','-preset','medium','-x264-params',
        'scenecut=0:b-adapt=0:b-pyramid=none:aud=1:repeat-headers=1:weightp=0:weightb=0:'+params,'-f','h264',str(path)]
    subprocess.run(command,check=True);return command
def rewrite(source,plan,frame_bits=None,poc_bits=None):
    """plan(decode index, header) -> dict of span replacements for that slice."""
    sps=pps=None;out=bytearray();index=0
    for nal in nal_units(source):
        kind=nal[0]&31;text=bits(rbsp(nal))
        if kind==7:
            sps=parse_sps(text)
            if frame_bits:text=rewrite_sps(text,sps,frame_bits,poc_bits)
            nal=escape(pack(text))
        elif kind==8:pps=parse_pps(text)
        elif kind in (1,5):
            h=parse_slice(text,sps,pps);edit={}
            # A narrower SPS field width re-encodes every value modulo the new range.
            if frame_bits:edit['frame_num']=u(frame_bits,h['frame_num']%(1<<frame_bits))
            if poc_bits and 'poc_lsb' in h:edit['poc']=u(poc_bits,h['poc_lsb']%(1<<poc_bits))
            edit.update(plan(index,h,dict(sps,frame_bits=frame_bits or sps['frame_bits'],poc_bits=poc_bits or sps.get('poc_bits'))))
            nal=escape(pack(emit_slice(text,h,sps,edit)));index+=1
        out+=b'\0\0\0\1'+nal
    return bytes(out)
def long_term_plan(case):
    """Nominal long-term marking in the first GOP; each refusal case alters one
    picture so that the native producer must refuse exactly there."""
    def plan(i,h,sps):
        e={}
        if i==0:e['marking']='00' if case=='mmco6-without-index' else '01'
        elif i==2:e['marking']=adaptive([(6,0)] if case=='mmco6-without-index' else [(4,2),(3,0,2 if case=='index-above-max' else 1)])
        elif i==5:e['modification']=modification([(2,2 if case=='list-index-above-max' else 0)])
        elif i==6:e['modification']=modification([(2,1),(0,0)])
        elif i==7:e['marking']=adaptive([(2,1)])
        elif i==8 and case=='absent-long-term':e['marking']=adaptive([(2,1)])
        elif i==9:e['marking']=adaptive([(1,1),(6,1)])
        elif i==11:e['marking']=adaptive([(4,1)])
        elif i==12:e['marking']=adaptive([(3,0,0)])
        return e
    return plan
def wrap_plan(mmco5):
    def plan(i,h,sps):
        base=mmco5 if mmco5 is not None and i>mmco5 else 0
        e={'frame_num':u(sps['frame_bits'],(i-base)%(1<<sps['frame_bits'])),'poc':u(sps['poc_bits'],(2*(i-base))%(1<<sps['poc_bits']))}
        if i==mmco5:e['marking']=adaptive([(5,)])
        return e
    return plan
def poc_wrap_plan(i,h,sps):return {}
def b_long_term_plan(i,h,sps):return {'marking':'01'} if i==0 else {}
def relabel(stream,profile):
    """Change only profile_idc; Baseline, Main and Extended share SPS syntax."""
    out=bytearray()
    for nal in nal_units(stream):
        if nal[0]&31==7:assert nal[1] in (66,77);nal=nal[:1]+bytes([profile])+nal[2:]
        out+=b'\0\0\0\1'+nal
    return bytes(out)

# ---- Independent model from FFmpeg trace_headers ---------------------------------
def trace(ffmpeg,stream,log):
    r=subprocess.run([ffmpeg,'-hide_banner','-i',str(stream),'-c','copy','-bsf:v','trace_headers','-f','null','-'],capture_output=True,text=True,check=True)
    log.write_text(r.stderr);slices=[];params={};current=None
    for line in r.stderr.splitlines():
        if line.endswith('] Slice Header'):current=[];slices.append(current);continue
        if re.search(r'\] (Sequence|Picture) Parameter Set$|\] Access Unit Delimiter$|\] Supplemental',line):current=None;continue
        m=re.search(r'\]\s+\d+\s+([A-Za-z_0-9]+)(?:\[[^\]]*\])*\s+[01]+\s+=\s+(-?\d+)\s*$',line)
        if m:
            if current is None:params.setdefault(m[1],int(m[2]))
            else:current.append((m[1],int(m[2])))
    return slices,params
def model(slices,params):
    """Return per picture {poc, list0, list1} as decode indices, or the index of
    the first picture whose B lists hold an active long-term reference."""
    max_frame=1<<(params['log2_max_frame_num_minus4']+4);limit=max(1,params['max_num_ref_frames'])
    poc_type=params['pic_order_cnt_type'];max_lsb=1<<(params.get('log2_max_pic_order_cnt_lsb_minus4',0)+4)
    dpb=[];long_max=-1;prev_msb=prev_lsb=0;prev_frame=prev_offset=0;out=[]
    for i,fields in enumerate(slices):
        f=dict(fields);t=f['slice_type']%5;idr=f['nal_unit_type']==5;ref=f['nal_ref_idc'];number=f['frame_num']
        if idr:dpb=[];prev_msb=prev_lsb=prev_frame=prev_offset=0
        if poc_type==0:
            lsb=f['pic_order_cnt_lsb'];msb=prev_msb
            if lsb<prev_lsb and prev_lsb-lsb>=max_lsb//2:msb+=max_lsb
            elif lsb>prev_lsb and lsb-prev_lsb>max_lsb//2:msb-=max_lsb
            poc=msb+lsb
        else:
            offset=0 if idr else prev_offset+(max_frame if number<prev_frame else 0);poc=0 if idr else 2*(offset+number)-(0 if ref else 1)
        for x in dpb:
            if not x['lt']:x['pic_num']=x['frame']-max_frame if x['frame']>number else x['frame']
        st=[x for x in dpb if not x['lt']];lt=sorted([x for x in dpb if x['lt']],key=lambda x:x['idx'])
        if t==0:lists=[sorted(st,key=lambda x:-x['pic_num'])+lt,[]]
        elif t==1:
            lower=sorted([x for x in st if x['poc']<poc],key=lambda x:-x['poc']);higher=sorted([x for x in st if x['poc']>poc],key=lambda x:x['poc'])
            lists=[lower+higher+lt,higher+lower+lt]
            if len(lists[1])>1 and lists[0]==lists[1]:lists[1][0],lists[1][1]=lists[1][1],lists[1][0]
        else:lists=[[],[]]
        active=[0,0]
        for l in range(2 if t==1 else 1 if t==0 else 0):
            active[l]=f[f'num_ref_idx_l{l}_active_minus1']+1 if f.get('num_ref_idx_active_override_flag') else params[f'num_ref_idx_l{l}_default_active_minus1']+1
            lists[l]=lists[l][:active[l]]
            if f.get(f'ref_pic_list_modification_flag_l{l}'):
                start=next(j for j,(k,_) in enumerate(fields) if k==f'ref_pic_list_modification_flag_l{l}')+1;pred=number;at=0
                for j in range(start,len(fields)):
                    key,value=fields[j]
                    if key!='modification_of_pic_nums_idc':continue
                    if value==3:break
                    if value==2:
                        target=fields[j+1][1];x=next(x for x in dpb if x['lt'] and x['idx']==target)
                        lists[l].insert(at,x);at+=1;lists[l]=lists[l][:at]+[y for y in lists[l][at:] if not (y['lt'] and y['idx']==target)]
                    else:
                        d=fields[j+1][1]+1;pred=(pred+(-d if value==0 else d))%max_frame;target=pred-max_frame if pred>number else pred
                        x=next(x for x in dpb if not x['lt'] and x['pic_num']==target)
                        lists[l].insert(at,x);at+=1;lists[l]=lists[l][:at]+[y for y in lists[l][at:] if y['lt'] or y['pic_num']!=target]
                lists[l]=lists[l][:active[l]]
            assert len(lists[l])==active[l],(i,l,'missing reference')
        if t==1 and any(x['lt'] for l in lists for x in l):return out,i
        out.append(dict(poc=poc,list0=[x['decode'] for x in lists[0]],list1=[x['decode'] for x in lists[1]]))
        reset=False;cur_lt=False;cur_idx=0
        if ref:
            if idr:
                cur_lt=bool(f['long_term_reference_flag']);long_max=0 if cur_lt else -1
            elif f.get('adaptive_ref_pic_marking_mode_flag'):
                ops=[(j,v) for j,(k,v) in enumerate(fields) if k=='memory_management_control_operation' and v]
                for j,op in ops:
                    args={k:v for k,v in fields[j+1:j+3]}
                    if op in (1,3):
                        target=number-args['difference_of_pic_nums_minus1']-1;x=next(x for x in dpb if not x['lt'] and x['pic_num']==target)
                        if op==1:dpb.remove(x)
                        else:
                            dpb=[y for y in dpb if not (y['lt'] and y['idx']==args['long_term_frame_idx'])];x['lt']=True;x['idx']=args['long_term_frame_idx']
                    elif op==2:dpb=[y for y in dpb if not (y['lt'] and y['idx']==args['long_term_pic_num'])]
                    elif op==4:long_max=args['max_long_term_frame_idx_plus1']-1;dpb=[y for y in dpb if not (y['lt'] and y['idx']>long_max)]
                    elif op==5:dpb=[];long_max=-1;reset=True
                    elif op==6:dpb=[y for y in dpb if not (y['lt'] and y['idx']==args['long_term_frame_idx'])];cur_lt=True;cur_idx=args['long_term_frame_idx']
            elif len(dpb)==limit:dpb.remove(min((x for x in dpb if not x['lt']),key=lambda x:x['pic_num']))
            assert len(dpb)<limit
            stored=0 if reset else poc
            dpb.append(dict(frame=0 if reset else number,poc=stored,lt=cur_lt,idx=cur_idx,decode=i,pic_num=0))
            if poc_type==0:prev_msb,prev_lsb=(0,0) if reset else (msb,lsb)
        if poc_type==2:prev_frame,prev_offset=(0,0) if reset else (number,offset)
        if reset:out[-1]['poc']=0
    return out,None

# ---- Native producer comparison ------------------------------------------------------
def producer(binary,directory,frames):
    paths=[]
    for i,data in enumerate(frames):p=directory/f'au-{i:03}.h264';p.write_bytes(data);paths.append(str(p))
    r=subprocess.run([str(binary),*paths],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:halt_on_error=1'})
    assert 'runtime error:' not in r.stderr and 'ERROR: AddressSanitizer' not in r.stderr,r.stderr
    text=r.stdout.strip()
    if r.returncode and not text.endswith(']'):text+='\n]'
    return r.returncode,json.loads(text) if text else []
def access_units(stream):
    config={};out=[]
    for nal in nal_units(stream):
        kind=nal[0]&31
        if kind in (7,8):config[kind]=b'\0\0\0\1'+nal
        elif kind in (1,5):out.append(config[7]+config[8]+b'\0\0\0\1'+nal)
    return out
def compare(plans,expected):
    owner={}
    for i,(plan,want) in enumerate(zip(plans,expected)):
        got=[[owner[s] for s in plan['list0']],[owner[s] for s in plan['list1']]]
        assert plan['poc']==want['poc'] and got==[want['list0'],want['list1']],(i,plan,want,got)
        owner[plan['slot']]=i

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--producer',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--ffmpeg',default='/usr/bin/ffmpeg')
    p.add_argument('--session',type=Path,help='Optional sealed High session; each nominal case then also gets a native-check fixture session.')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    spec=importlib.util.spec_from_file_location('high',Path(__file__).resolve().parents[1]/'apps/h264/high.py');high=importlib.util.module_from_spec(spec);spec.loader.exec_module(high)
    sources={'p3':(30,'keyint=15:min-keyint=15:bframes=0:ref=3','high'),'p1':(40,'keyint=48:min-keyint=48:bframes=0:ref=1','high'),
             'b2':(40,'keyint=48:min-keyint=48:bframes=2:ref=2','high'),'baseline':(30,'keyint=15:min-keyint=15:bframes=0:ref=3','baseline'),
             'main':(30,'keyint=15:min-keyint=15:bframes=2:ref=2:weightb=1','main')}
    commands={}
    for name,(frames,params,profile) in sources.items():commands[name]=x264(a.ffmpeg,a.output/f'{name}.h264',frames,params,profile)
    read=lambda n:(a.output/f'{n}.h264').read_bytes()
    # name: (source, rewrite arguments, expected refusal index or None)
    cases={'long-term':('p3',dict(plan=long_term_plan('nominal')),None),
           'mmco5-wrap':('p1',dict(plan=wrap_plan(21),frame_bits=4,poc_bits=4),None),
           'poc-wrap-b':('b2',dict(plan=poc_wrap_plan,frame_bits=4,poc_bits=4),None),
           'long-term-b-refused':('b2',dict(plan=b_long_term_plan),'model'),
           'index-above-max':('p3',dict(plan=long_term_plan('index-above-max')),2),
           'list-index-above-max':('p3',dict(plan=long_term_plan('list-index-above-max')),5),
           'absent-long-term':('p3',dict(plan=long_term_plan('absent-long-term')),8),
           'mmco6-without-index':('p3',dict(plan=long_term_plan('mmco6-without-index')),2),
           'baseline-profile':('baseline',None,None),'main-profile-b':('main',None,None),
           'extended-profile-refused':('baseline',88,0)}
    reports=[]
    for name,(source,args,refusal) in cases.items():
        d=a.output/name;d.mkdir()
        stream=read(source) if args is None else relabel(read(source),args) if isinstance(args,int) else rewrite(read(source),**args)
        (d/'input.h264').write_bytes(stream)
        frames=access_units(stream);rc,plans=producer(a.producer,d,frames);report=dict(case=name,source=source,pictures=len(frames),returncode=rc)
        if refusal is None or refusal=='model':
            decode=subprocess.run([a.ffmpeg,'-hide_banner','-loglevel','error','-err_detect','explode','-xerror','-i',str(d/'input.h264'),'-pix_fmt','yuv420p','-f','rawvideo',str(d/'reference.yuv')],capture_output=True,text=True)
            assert decode.returncode==0 and not decode.stderr.strip(),(name,decode.stderr)
            assert (d/'reference.yuv').stat().st_size==len(frames)*320*240*3//2
            slices,params=trace(a.ffmpeg,d/'input.h264',d/'headers.log');assert len(slices)==len(frames)
            expected,stop=model(slices,params)
            if refusal is None:
                assert stop is None and rc==0 and len(plans)==len(frames),(name,rc,stop)
                compare(plans,expected)
                bundle=d/'qualification';manifest=high.prepare_inputs(d/'input.h264',bundle,d/'reference.yuv',a.producer)
                if a.session:
                    # Sessions of one High image differ only in the manifest digest.
                    session=bytearray(a.session.read_bytes());assert len(session)==856 and session[600]==4
                    session[768:800]=hashlib.sha256(manifest).digest();(d/'qualification-session.bin').write_bytes(session)
                rows=high.descriptors(manifest);assert len(rows)==len(frames) and sorted(r['presentation'] for r in rows)==list(range(len(frames)))
                report.update(manifest_sha256=hashlib.sha256(manifest).hexdigest(),gops=sorted({x['gop'] for x in plans}),
                    long_term_lists=sum(any(slices[i][k][0]=='modification_of_pic_nums_idc' and slices[i][k][1]==2 for k in range(len(slices[i]))) for i in range(len(slices))),
                    pocs=[x['poc'] for x in plans])
            else:
                assert stop is not None and rc==4 and len(plans)==stop,(name,rc,stop,len(plans));compare(plans,expected[:stop])
                report['refused_at']=stop
        else:
            assert rc==4 and len(plans)==refusal,(name,rc,len(plans));report['refused_at']=refusal
        reports.append(report)
    # Each nominal stream must exercise the mode it is named for.
    by={r['case']:r for r in reports}
    assert by['long-term']['long_term_lists']>=2 and by['long-term']['gops']==[0,1]
    assert by['mmco5-wrap']['gops']==[0,1] and max(by['mmco5-wrap']['pocs'])>=32
    assert max(by['poc-wrap-b']['pocs'])>=64
    assert all(by[n]['returncode']==0 and by[n]['gops']==[0,1] for n in ('baseline-profile','main-profile-b'))
    (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,producer_sha256=hashlib.sha256(a.producer.read_bytes()).hexdigest(),
        source_commands=commands,cases=reports,scope='Header-rewritten libx264 High/CABAC streams decoded by FFmpeg with explode error detection; FFmpeg trace syntax drives an independent POC/list/marking model compared with the actual sanitized native syntax producer and the High manifest builder. No device arithmetic, pixel, reference visibility or physical claim.'),indent=2)+'\n')
    print('PASS High-build long-term references, MMCO5, POC/frame_num wrap, Baseline/Main profiles and refusals')
if __name__=='__main__':main()
