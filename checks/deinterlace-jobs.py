#!/usr/bin/env python3
"""Check repeated DI301 jobs from plan generation to host predicates and the SRAM launcher.

Host plan generation runs the pinned DI301 code under ASan/UBSan for every
WEAVE, MOTION and TNR sequence with recorded synthetic statistics. The TNR
gain of each later job is recomputed here from those statistics with the
source's integer widths. Single-job plans must equal the base revision's.
The linked AArch64 image must generate identical plans from the same
statistics. Host preparation and capture must mirror the session rules, and
a rebuilt SRAM launcher runs in the shared AArch32 model inside an earlier TNR
bundle. Device effects, filtering arithmetic and physical execution are
outside every case.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import io
from unittest.mock import patch
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR,UC_ARM64_REG_PSTATE
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'checks'))
import harness
import sram_launcher
PLAN_BYTES=0x5000
M32=(1<<32)-1
M64=(1<<64)-1
INITIAL=(0x3a,38,48,62)
BOUNDS=(None,(16,38),(28,48),(52,62))
LEGACY=r'''#include "a733_deinterlace.h"
#include <stdio.h>
#include <string.h>
static struct a733_deinterlace_plan plan;
int main(int argc,char **argv)
{
 FILE *out=fopen(argv[1],"wb");if(argc!=2||!out)return 1;
 for(unsigned mode=1;mode<=3;++mode)for(unsigned strength=0;strength<(mode==3?4u:1u);++strength){
  struct a733_deinterlace_session s={.width=640,.height=480,.y_stride=640,.uv_stride=640,
   .format=A733_DEINTERLACE_FORMAT_NV12,.mode=mode,.top_field_first=1};
  s.strength=strength;memset(&plan,0,sizeof(plan));
  if(!a733_deinterlace_commands(&plan,&s)||fwrite(plan.words,1,sizeof(plan.words),out)!=sizeof(plan.words))return 1;
 }
 return fclose(out);
}
'''


def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def run(commands,out,stem):
    for index,command in enumerate(commands):
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True,env=harness.environment())
        (out/f'{stem}{index}.log').write_text(result.stdout+result.stderr)
        assert result.returncode==0,(stem,index,result.stdout+result.stderr)
    return result.stdout.strip()


def compile_command(args,tree,sources,output):
    # Headers the base archive omits come from this tree, after its own.
    return [args.cc,'-std=gnu11','-O1','-g','-w',*harness.SANITIZE,'-I'+str(tree/'include'),
        '-I'+str(tree/'drivers/deinterlace'),'-I'+str(ROOT/'include'),'-I'+str(args.generated),*map(str,sources),
        str(tree/'drivers/deinterlace/a733_deinterlace.c'),str(args.generated/'di301_alg.c'),
        str(args.generated/'di-formats.c'),'-o',str(output)]


def host_plans(args):
    out=args.output/'host';out.mkdir()
    summary=run([compile_command(args,ROOT,[ROOT/'checks/deinterlace-jobs-plan.c'],out/'plan'),
        [out/'plan',out/'plans.bin',out/'log.txt']],out,'step')
    data=(out/'plans.bin').read_bytes();cases=[];current=None
    for line in (out/'log.txt').read_text().splitlines():
        words=line.split()
        if words[0]=='plan':
            mode,strength,repeats,kind,job=map(int,words[1:6])
            if job==0:current=dict(mode=mode,strength=strength,repeats=repeats,kind=kind,jobs=[]);cases.append(current)
            index=sum(len(case['jobs']) for case in cases)
            plan=data[index*PLAN_BYTES:(index+1)*PLAN_BYTES]
            assert struct.unpack_from('<I',plan,0x4000)[0]==int(words[6],16)
            current['jobs'].append(dict(plan=plan,statistics=None))
        elif words[0]=='stats':
            current['jobs'][-1]['statistics']={int(k,16):int(v) for k,v in (item.split('=') for item in words[1:])}
    assert sum(len(case['jobs']) for case in cases)*PLAN_BYTES==len(data)
    return cases,summary


def gain(previous,statistics):
    """di_alg_tnr_strengths_adjustment with the source's u32, u64 and do_div widths."""
    noise_num=(statistics[0x4038]*1*63)&M32
    noise_den=(statistics[0x404c]*previous*2*64)&M32
    still_num=(statistics[0x4044]*2)&M32
    still_den=(640*480*5)&M32
    weight=(noise_num*still_num)&M64;den=(noise_den*still_den)&M64
    if den:
        divisor=den&M32;assert divisor,'Statistics would divide by zero'
        weight//=divisor
    else:weight=0
    return ((weight&M32)+31*previous)&M32,weight


def gains(cases):
    rows=[];moved=set()
    for case in cases:
        if case['mode']!=3:continue
        strength=case['strength'];words=[struct.unpack_from('<I',job['plan'],0x4000)[0] for job in case['jobs']]
        assert words[0]==INITIAL[strength]*0x010101
        for job in range(1,len(words)):
            previous=words[job-1]&255
            if not strength:expected=INITIAL[0]
            else:
                low,high=BOUNDS[strength];total,weight=gain(previous,case['jobs'][job-1]['statistics'])
                expected=min(high,max(low,total//32))
            assert words[job]==expected*0x010101,(case['strength'],case['repeats'],case['kind'],job,hex(words[job]),expected)
            if expected!=previous:moved.add('rise' if expected>previous else 'fall')
            rows.append(dict(strength=strength,repeats=case['repeats'],kind=case['kind'],job=job,previous=previous,gain=expected))
    assert moved=={'rise','fall'},moved
    return rows


def legacy(args):
    """Single-job plans of every mode and strength equal the base revision's."""
    out=args.output/'legacy';out.mkdir();base=out/'base-tree';base.mkdir()
    archive=subprocess.run(['git','-C',str(ROOT),'archive',args.base,'include/a733_deinterlace.h',
        'include/a733_deinterlace_application.h','include/a733_board_application.h','drivers/deinterlace'],
        capture_output=True,check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:tar.extractall(base,filter='data')
    generator=out/'legacy.c';generator.write_text(LEGACY);plans={}
    for name,tree in (('base',base),('current',ROOT)):
        run([compile_command(args,tree,[generator],out/name),[out/name,out/(name+'.bin')]],out,name)
        plans[name]=(out/(name+'.bin')).read_bytes()
    assert len(plans['base'])==6*PLAN_BYTES and plans['base']==plans['current']
    return hashlib.sha256(plans['current']).hexdigest()


class Linked:
    def __init__(self,path):
        self.image=harness.Image(path,layout_names=('a733_deinterlace_session','a733_deinterlace_plan'))
        assert self.image.layouts['a733_deinterlace_session']['repeats']==886
        self.layout=self.image.layouts['a733_deinterlace_plan']
        self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.cpu.reg_write(UC_ARM64_REG_PSTATE,13)
        self.cpu.mem_map(0x41000000,0x1200000);self.cpu.mem_map(0x50000000,0x200000)
        for address,data in self.image.parts:
            if data:self.cpu.mem_write(address,data)
        self.session,self.plan,self.stop=0x41f00000,0x41f10000,0x501ff000
    def call(self,name,*values):
        for index,value in enumerate(values):self.cpu.reg_write(UC_ARM64_REG_X0+index,value)
        self.cpu.reg_write(UC_ARM64_REG_SP,0x501fe000);self.cpu.reg_write(UC_ARM64_REG_LR,self.stop)
        self.cpu.emu_start(self.image.address(name),self.stop,count=40000000)
        assert self.cpu.reg_read(UC_ARM64_REG_PC)==self.stop,(name,hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
        return self.cpu.reg_read(UC_ARM64_REG_X0)&0xffffffff
    def words(self):return bytes(self.cpu.mem_read(self.plan,PLAN_BYTES))


def geometry(mode,strength,repeats):
    return struct.pack('<7I2H',640,480,640,640,0x3231564e,mode,1,strength,repeats)


def linked_plans(args,cases,base):
    linked=Linked(args.image);plan_bytes=linked.layout['client']+4096+16
    results=[]
    for case in cases:
        raw=bytes(856)+geometry(case['mode'],case['strength'],case['repeats'])
        linked.cpu.mem_write(linked.session,raw);linked.cpu.mem_write(linked.plan,bytes(plan_bytes))
        assert linked.call('a733_deinterlace_commands',linked.plan,linked.session)==1
        assert linked.words()==case['jobs'][0]['plan'],('job0',case['mode'],case['strength'],case['repeats'],case['kind'])
        for job in range(1,len(case['jobs'])):
            for offset,value in case['jobs'][job-1]['statistics'].items():
                linked.cpu.mem_write(linked.plan+offset,struct.pack('<I',value))
            assert linked.call('a733_deinterlace_next',linked.plan,linked.session)==1
            assert linked.words()==case['jobs'][job]['plan'],(job,case['mode'],case['strength'],case['repeats'],case['kind'])
        assert linked.call('a733_deinterlace_next',linked.plan,linked.session)==0
        results.append(dict(mode=case['mode'],strength=case['strength'],repeats=case['repeats'],kind=case['kind'],jobs=len(case['jobs'])))
    # The admitted session validator follows the same repeat limit.
    for repeats in range(5):
        raw=bytearray(base);struct.pack_into('<2H',raw,884,0,repeats)
        linked.cpu.mem_write(linked.session,bytes(raw))
        assert linked.call('a733_deinterlace_session_valid',linked.session)==(repeats<=3),repeats
        linked.cpu.mem_write(linked.plan,bytes(plan_bytes))
        assert linked.call('a733_deinterlace_commands',linked.plan,linked.session)==(repeats<=3),repeats
    return results


def host_predicates(args,base):
    prep=module('deinterlace_jobs_preparation',ROOT/'apps/deinterlace/prepare.py')
    capture=module('deinterlace_jobs_capture',ROOT/'apps/deinterlace/capture.py')
    producer=module('deinterlace_jobs_producer',ROOT/'apps/deinterlace/producer.py')
    image=args.image.read_bytes();previous=args.previous_image.read_bytes();cases=[]
    for mode in (1,2,3):
        for strength in (0,1,3,4):
            for repeats in range(5):
                words=struct.unpack('<8I',geometry(mode,strength,repeats))
                want=(strength==0 or (mode==3 and strength<=3)) and repeats<=3
                try:size=prep.geometry_check(words)
                except ValueError:accepted=False
                else:accepted=True;assert size==460800*(repeats+(1 if mode==1 else 2))
                assert accepted==want,(mode,strength,repeats);cases.append(dict(mode=mode,strength=strength,repeats=repeats,accepted=accepted))
    epoch,generation=struct.unpack_from('<2Q',base,24)
    def frame(raw,status=0,finished=0,strengths=b''):
        header=struct.pack('<4I2Q',0x52494453,1,status,0,epoch,generation)+raw[768:800]+hashlib.sha256(raw).digest()+bytes(32)
        header+=raw[856:888]+(struct.pack('<4I',1,0x1,0x200,0) if status else bytes(16))
        tail=struct.pack('<I',finished)+strengths if status else b''
        return header+tail+bytes(192-len(header)-len(tail))
    refusals=[]
    for repeats in range(4):
        raw=bytearray(base);struct.pack_into('<2H',raw,884,2,repeats);raw=bytes(raw);prep.session_check(raw)
        prep.embedded_reference(image,raw);assert prep.jobs(raw)==1+repeats
        if repeats:
            harness.refused(lambda:prep.embedded_reference(previous,raw),'compiled input capacity')
            # A pending reply, and a failure after the second job, decode.
            assert capture.decode(frame(raw),raw)['mode']=='TNR-ADAPTIVE-MIDDLE'
            decoded=capture.decode(frame(raw,2,min(2,repeats),bytes([48,44])+bytes(2)),raw)
            assert decoded['finished_jobs']==min(2,repeats) and decoded['tnr_strengths'][:2]==[48,44]
            for name,bad in (('finished-beyond-jobs',frame(raw,2,repeats+2,bytes([48,44,40,36]))),
                    ('strength-outside-level',frame(raw,2,1,bytes([48,27,0,0]))),
                    ('first-strength-differs',frame(raw,2,1,bytes([44,44,0,0]))),
                    ('strength-gap',frame(raw,2,1,bytes([48,0,44,0]))),
                    ('reserved-tail',frame(raw,2,1,bytes([48,0,0,0,1])))):
                harness.refused(lambda:capture.decode(bad,raw),'Deinterlace');refusals.append(name+'-'+str(repeats))
        else:
            prep.embedded_reference(previous,raw)
            harness.refused(lambda:capture.decode(frame(raw,2,1,bytes([48,0,0,0])),raw),'reserved result bytes')
    raw=bytearray(base);struct.pack_into('<I',raw,876,2);struct.pack_into('<2H',raw,884,0,2);raw=bytes(raw)
    harness.refused(lambda:capture.decode(frame(raw,2,1,bytes([58,0,0,0])),raw),'strength bytes outside TNR')
    assert capture.decode(frame(raw,2,1),raw)['tnr_strengths'] is None
    # Physical admission of a sequence needs the reviewed fact that finish ends
    # DI access to a job's buffers; a single job does not.
    facts={'initial':['all_masters_excluded','prior_transactions_drained','prior_walks_drained',
            'di_dedicated_reset_held_bus_and_module_gated','skipped_masters_physically_quiescent',
            'remote_secure_writers_excluded','di_irq_owned_masked','iommu_irqs_owned_masked'],
        'memory':['private_nc_arenas','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable',
            'di_device_mapping','dma_visibility_established'],
        'electrical':['dcdc2_shared_800mv','di_operating_point_source_reviewed','stable_di_parent'],
        'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse_after_fault','one_attempt']}
    folder=(args.output/'admission').resolve();folder.mkdir();image_hash=hashlib.sha256(image).hexdigest()
    def admission(repeats,reuse):
        raw=bytearray(base);struct.pack_into('<2H',raw,884,0,repeats);record=dict(schema='a733-deinterlace-supervised-entry-v1',
            identity='model',port='none',physical_authority=True,modeled=False,deinterlace_ddr=dict(path=str(folder/'absent-ddr.json'),sha256='0'*64))
        for key,offset in (('initial',640),('memory',672),('electrical',704),('supervision',736)):
            values={name:True for name in facts[key]+(['di_finish_ends_job_buffer_access'] if key=='memory' and reuse else [])}
            data=json.dumps(dict(schema='a733-deinterlace-'+key+'-v1',identity='model',port='none',image_sha256=image_hash,
                reviewed=True,physical_authority=True,modeled=False,**values)).encode()
            path=folder/f'{key}-{repeats}-{int(reuse)}.json';path.write_bytes(data);raw[offset:offset+32]=hashlib.sha256(data).digest()
            record[key]=dict(path=str(path),sha256=hashlib.sha256(data).hexdigest())
        with patch.object(prep.board,'validate_admission',lambda *values:{}):
            return prep.validate_admission(record,image_hash,'0'*64,bytes(raw))
    for repeats,reuse,reason in ((0,False,'No such file'),(1,True,'No such file'),(1,False,'Required deinterlace physical fact absent')):
        harness.refused(lambda:admission(repeats,reuse),reason,(ValueError,OSError))
    refusals.append('sequence-without-reuse-fact')
    for mode in (1,2,3):
        for repeats in range(4):
            assert len(producer.field_pattern(mode,repeats))==460800*(repeats+(1 if mode==1 else 2))
    assert producer.field_pattern(2)==producer.field_pattern(2,1)[:921600]
    return cases,refusals


def launcher(args,base):
    out=args.output/'launcher-build';out.mkdir()
    prep=module('deinterlace_jobs_launcher_preparation',ROOT/'apps/deinterlace/prepare.py')
    prep.build_launcher(out,str(args.cc),str(harness.LLD))
    bundle=args.output/'launcher-bundle';bundle.mkdir()
    for name in ('image.elf','session.bin','contract.bin','request.bin'):shutil.copy2(args.bundle/name,bundle/name)
    shutil.copy2(out/'launch.bin',bundle/'launch.bin')
    assert struct.unpack_from('<2I',base,876)==(3,1)
    cases=[('repeats-%d-handoff'%repeats,12,(884,struct.pack('<2H',strength,repeats)),None)
        for repeats in (1,2,3) for strength in (0,3)]
    cases+=[('repeats-out-of-range',14,(884,struct.pack('<2H',0,4)),None),
        ('repeats-and-strength-out-of-range',14,(884,struct.pack('<2H',4,1)),None),
        ('repeats-with-weave-strength',14,(876,struct.pack('<3I',1,1,1|(1<<16))),None),
        ('weave-repeats-handoff',12,(876,struct.pack('<3I',1,1,2<<16)),None)]
    return sram_launcher.check_bundle(bundle,args.output/'launcher',(0x4153373344494131,1,888),(0x4153373344494331,288),20,cases,
        'Rebuilt deinterlacer SRAM launcher in an earlier TNR bundle; geometry words patched. System registers, counter and MMIO are modeled.',
        'PASS {count} rebuilt deinterlacer SRAM launcher cases')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generated',type=Path,required=True,help='Adapted DI301 sources from the application build')
    parser.add_argument('--image',type=Path,required=True,help='The revision5 deinterlace ELF')
    parser.add_argument('--previous-image',type=Path,required=True,help='A revision4 deinterlace ELF')
    parser.add_argument('--bundle',type=Path,required=True,help='An earlier prepared TNR bundle; read only')
    parser.add_argument('--base',required=True,help='Git revision whose single-job plans must be unchanged')
    parser.add_argument('--cc',type=Path,default=harness.CLANG)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    base=(args.bundle/'session.bin').read_bytes();assert len(base)==888 and struct.unpack_from('<I',base,884)[0]==0
    cases,summary=host_plans(args)
    rows=gains(cases)
    legacy_sha256=legacy(args)
    linked=linked_plans(args,cases,base)
    predicates,refusals=host_predicates(args,base)
    launch=launcher(args,base)
    result=dict(passed=True,physical_access=False,scope=__doc__,host=summary,gains=rows,
        legacy_plans_sha256=legacy_sha256,base=args.base,linked=linked,host_predicates=predicates,capture_refusals=refusals,
        launcher_cases=[case['case'] for case in launch['cases']],launcher_bytes=(args.output/'launcher-bundle/launch.bin').stat().st_size,
        image_sha256=hashlib.sha256(args.image.read_bytes()).hexdigest(),
        previous_image_sha256=hashlib.sha256(args.previous_image.read_bytes()).hexdigest(),
        launcher_sha256=launch['launcher_sha256'])
    (args.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(summary);print('PASS',len(rows),'recomputed gains,',len(linked),'linked sequences,',len(predicates),'host predicates,',
        len(refusals),'capture refusals,',len(launch['cases']),'launcher cases')


if __name__=='__main__':main()
