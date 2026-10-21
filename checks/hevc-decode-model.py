#!/usr/bin/env python3
"""Check native HEVC slice parsing and register programming on the host.

Builds checks/hevc-decode-model.c with the generated GStreamer parser, the
native parser, storage, controller and decoder binding under AddressSanitizer
and UBSan, and runs it on the recorded two-picture clip. It then compares:

* header bit lengths and slice fields with FFmpeg's trace_headers output, an
  independent parser;
* the IDR picture's registers and frame record with the effective A733
  libawh265 instruction trace in stream-adapted-registers/result.json;
* the P picture's registers with the Cedrus and CedarC rules it follows.

No device, USB connection or board is involved. Matching registers do not
establish numerical decoding or physical operation.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
CACHE=Path.home()/'.cache/sunstoneos'
SOURCES=['checks/hevc-decode-model.c','apps/hevc/parse.c','apps/hevc/storage.c',
    'drivers/video/a733_video_hevc.c','drivers/video/a733_video_platform.c','drivers/video/a733_video_unit.c',
    'drivers/clock/a733_clock_leases.c','drivers/gpu/a733_gpu_resources.c','drivers/storage/storage_digest.c',
    'runtime/device_memory_pool.c','drivers/iommu/a733_iommu.c']


def run(command,log,**kwargs):
    result=subprocess.run([str(x) for x in command],capture_output=True,text=True,**kwargs)
    log.write_text(result.stdout+result.stderr)
    if result.returncode:raise SystemExit(f'{command[0]} failed; see {log}')
    return result.stdout


def slice_headers(ffmpeg,clip,log):
    """Per slice: name -> (RBSP bit position from the NAL header, value)."""
    out=run([ffmpeg,'-nostdin','-hide_banner','-loglevel','trace','-i',clip,'-c','copy',
        '-bsf:v','trace_headers','-f','null','-'],log)
    text=log.read_text();slices=[];current=None;sps={}
    for line in text.splitlines():
        if '[trace_headers' not in line:continue
        if 'Slice Segment Header' in line:current={};slices.append(current);continue
        if 'Parameter Set' in line:current=None;continue
        m=re.search(r'\]\s+(\d+)\s+([\w\[\]]+)\s+[01]+\s+=\s+(-?\d+)\s*$',line)
        if m:
            if current is None:sps.setdefault(m[2],int(m[3]))
            else:current.setdefault(m[2],(int(m[1]),int(m[3])))
    del out
    return slices,sps


def field(value,high,low):return (value>>low)&((1<<(high-low+1))-1)


def main():
    p=argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--evidence',type=Path,default=CACHE/'evidence/hevc-decode-20261020')
    p.add_argument('--source',type=Path,default=CACHE/'sources/hevc-decode')
    p.add_argument('--reader',type=Path,default=CACHE/'sources/h264-high')
    p.add_argument('--clang',default='clang')
    p.add_argument('--ffmpeg',default='/usr/bin/ffmpeg')
    a=p.parse_args();out=a.output;out.mkdir(parents=True,exist_ok=False)
    clip=a.evidence/'two-pictures.hevc';packets=json.loads((a.evidence/'packets.json').read_text())['packets']
    first=int(packets[1]['pos']);reference=json.loads((a.evidence/'stream-adapted-registers/result.json').read_text())
    assert reference['identity']['chip']==0x3331000021320 and reference['stream']['sha256']==hashlib.sha256(clip.read_bytes()).hexdigest()
    gst=out/'gst'
    run(['python3',ROOT/'apps/hevc/gst-source.py','--source',a.source,'--reader',a.reader,'--output',gst],out/'gst-source.log')
    flags=[a.clang,'-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',
        '-ffunction-sections','-fdata-sections','-DSUNSTONE_BOARD_SUPERVISED=1','-I'+str(ROOT/'include'),
        '-I'+str(ROOT/'checks'),'-I'+str(ROOT/'apps/hevc'),'-I'+str(gst),'-I'+str(ROOT/'platform/a733-smp-cached')]
    objects=[]
    for name in ('gst-hevc-parser.c','gst-nal.c'):
        # Third-party generated sources: warnings stay upstream's.
        objects.append(out/(name+'.o'));run(flags+['-w','-c',gst/name,'-o',objects[-1]],out/(name+'.log'))
    model=out/'hevc-decode-model'
    run(flags+['-Wall','-Wextra','-Werror','-Wl,--gc-sections',*[ROOT/x for x in SOURCES],*objects,'-o',model],out/'build.log')
    trace_path=out/'trace.json'
    stdout=run([model,clip,first,trace_path],out/'model.log')
    trace=json.loads(trace_path.read_text());pictures=trace['pictures'];buffers=trace['buffers']
    slices,sps=slice_headers(a.ffmpeg,clip,out/'trace-headers.log')
    assert len(slices)==2 and len(pictures)==2
    report={}
    # Independent syntax: header length and slice fields from FFmpeg.
    for i,(picture,s) in enumerate(zip(pictures,slices)):
        r=picture['registers'];reg=lambda off:r[off//4]
        get=lambda name,default=0:s[name][1] if name in s else default
        assert picture['header_bits']==s['alignment_bit_equal_to_one'][0],(i,picture['header_bits'])
        assert picture['type']==get('slice_type') and picture['poc']==get('slice_pic_order_cnt_lsb')
        info0,info1=reg(0x520),reg(0x524)
        assert field(info0,3,2)==get('slice_type') and field(info0,0,0)==1
        assert field(info0,7,7)==get('slice_sao_luma_flag') and field(info0,8,8)==get('slice_sao_chroma_flag')
        assert field(info0,6,6)==get('slice_temporal_mvp_enabled_flag') and field(info0,26,24)==get('five_minus_max_num_merge_cand')
        assert field(info0,11,11)==get('collocated_from_l0_flag',1)
        assert field(info1,6,0)==get('slice_qp_delta')&0x7f and field(info1,22,22)==get('slice_loop_filter_across_slices_enabled_flag')
        assert field(info1,21,21)==(get('slice_type')!=2)
        spsr=reg(0x504)
        assert (field(spsr,1,0),field(spsr,10,9),field(spsr,12,11),field(spsr,14,13),field(spsr,16,15),field(spsr,19,17),field(spsr,22,20))==(
            sps['chroma_format_idc'],sps['log2_min_luma_coding_block_size_minus3'],sps['log2_diff_max_min_luma_coding_block_size'],
            sps['log2_min_luma_transform_block_size_minus2'],sps['log2_diff_max_min_luma_transform_block_size'],
            sps['max_transform_hierarchy_depth_inter'],sps['max_transform_hierarchy_depth_intra'])
        assert (field(spsr,23,23),field(spsr,24,24),field(spsr,25,25),field(spsr,26,26))==(sps['amp_enabled_flag'],
            sps['sample_adaptive_offset_enabled_flag'],sps.get('sps_temporal_mvp_enabled_flag',0),sps['strong_intra_smoothing_enabled_flag'])
        assert reg(0x508)==640|(480<<16) and reg(0x500)==picture['nal_type']|0x40
        report[f'picture{i}_syntax']=dict(header_bits=picture['header_bits'],poc=picture['poc'],slice_type=picture['type'])
    # Effective A733 library writes for the IDR picture, up to its trigger.
    decode=[x for x in reference['mmio'] if x['phase']=='H265DecoderDecode']
    init=max(k for k,x in enumerate(decode) if x['offset']==0x534 and x['value']==7)
    effective={}
    for x in decode:effective[x['offset']]=x['value']
    ours=pictures[0]['registers'];idr=pictures[0];reg=lambda off:ours[off//4]
    same=[0x0,0x500,0x504,0x508,0x510,0x514,0x518,0x520,0x524,0x528,0x530,0x53c,0x55c,
        0xc4,0xc8,0xcc,0xe8,0xec,0x550,0x554,0x558,0x580]
    differ={off:(reg(off),effective[off]) for off in same if reg(off)!=effective[off]}
    assert not differ,{hex(k):[hex(x) for x in v] for k,v in differ.items()}
    # PCM is disabled in both; only its unsignaled sample fields differ.
    assert not field(reg(0x50c),15,14) and not field(effective[0x50c],15,14)
    # Same bitstream position: the library starts at the slice data offset,
    # the native controller flushes the header bits from the slot start.
    assert reg(0x544)==0 and effective[0x544]==8*idr['offset']+idr['header_bits']
    assert reg(0x548)==8*idr['bytes'] and effective[0x544]+effective[0x548]>=8*(idr['offset']+idr['bytes'])
    assert reg(0x540)>>28==effective[0x540]>>28==7 and reg(0x540)&0x0fffffff==buffers[0][0]>>8
    assert reg(0x54c)==(buffers[0][0]+buffers[0][1]-1)>>8 and reg(0x560)==buffers[6][0]>>8 and reg(0x564)==buffers[7][0]>>8
    record=[x['value'] for x in decode if x['offset']==0x5e4];assert effective[0x5e0]==0x400 and len(record)==6
    sram=idr['sram'][0x400//4:0x400//4+6]
    assert record[0]==record[1]==0 and record[2]==record[3] and record[5]-record[4]==307200>>8
    assert sram==[0,0,buffers[4][0]>>8,buffers[4][0]>>8,buffers[2][0]>>8,(buffers[2][0]+307200)>>8],sram
    # Write order after INIT_SWDEC, ignoring the cycle counter, the CTB count
    # cleared before INIT here and the entry-point address Cedrus also writes.
    events=trace['events'];ve=0x01c0e000
    start=max(k for k,(addr,value) in enumerate(events[:idr['trigger_event']]) if addr==ve+0x534 and value==7)
    native=[addr-ve for addr,value in events[start+1:idr['trigger_event']+1] if addr-ve not in (0x534,0x564)]+[0x534]
    library=[x['offset'] for x in decode[init+1:] if x['offset'] not in (0x8,0x53c,0x534)]+[0x534]
    assert native==library,(native,library)
    flushes=[v>>8 for addr,v in events[start+1:idr['trigger_event']] if addr==ve+0x534 and v&255==3]
    assert sum(flushes)==idr['header_bits']
    report['idr_effective_trace']=dict(equal_registers=[hex(x) for x in same],write_order_equal=True,
        bitstream_offset_bits=effective[0x544],flushed_bits=flushes,pcm=[hex(reg(0x50c)),hex(effective[0x50c])],
        omitted=['0x8 cycle counter'],added=['0x52c/0x568/0x56c zero','0x53c before INIT','0x538 status clear','0x564 entry points'])
    # P picture: Cedrus/CedarC low-delay bit, both frame records, list0 = slot 0.
    pp=pictures[1];r1=pp['registers'];reg1=lambda off:r1[off//4]
    assert reg1(0x500)==0x41 and reg1(0x55c)==1 and field(reg1(0x524),21,21)==1 and field(reg1(0x520),19,16)==0
    frames=pp['sram'][0x400//4:0x400//4+16]
    assert frames[:6]==[0,0,buffers[4][0]>>8,buffers[4][0]>>8,buffers[2][0]>>8,(buffers[2][0]+307200)>>8]
    assert frames[8:14]==[pp['poc'],pp['poc'],buffers[5][0]>>8,buffers[5][0]>>8,buffers[3][0]>>8,(buffers[3][0]+307200)>>8]
    assert pp['sram'][0xc00//4]==0
    for off in (0x530,0xc4,0xc8,0xcc,0xe8,0xec,0x518,0x528):assert reg1(off)==reg(off)
    report['p_picture']=dict(not_low_delay=True,output_index=1,list0=[0])
    result=dict(passed=True,physical_access=False,clip_sha256=hashlib.sha256(clip.read_bytes()).hexdigest(),
        model_sha256=hashlib.sha256(model.read_bytes()).hexdigest(),model_output=stdout.splitlines(),report=report,
        scope='Host model of the native parser, storage, controller and decoder binding with modeled MMIO. '
            'Register and syntax agreement with FFmpeg and the effective A733 library trace; no numerical decode, '
            'native execution or physical operation.')
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS HEVC parse, storage, controller and binding model; IDR registers match the effective A733 trace')


if __name__=='__main__':main()
