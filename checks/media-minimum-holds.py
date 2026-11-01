#!/usr/bin/env python3
"""Exercise media minimum holds with a fractional 24 MHz counter.

The original driver callbacks and state machines execute on the host. Models
supply hardware effects and admission; no device is accessed. Small native
helpers are extracted verbatim where their complete application needs hardware.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

import harness

ROOT = Path(__file__).resolve().parents[1]
CLOCK = r'''
#include <stdint.h>
#include <assert.h>
#include "a733_range.h"
static uint64_t ticks, cost, last_lock_ticks, analog_ticks, command_ticks, reset_ticks __attribute__((unused));
static uint64_t us(void){return ticks/24;}
static void write_cost(void){ticks+=cost;}
'''

def extract(path, name):
    source = (ROOT/path).read_text()
    match = re.search(r'^(?:static )?[\w *]+\b'+name+r'\([^;{}]*\)\s*\{', source, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]+'\n'


def fixture(name):
    return '#define main preserved_fixture_main\n#include "'+name+'"\n#undef main\n'+CLOCK

PLL = fixture('a733-display-pll.c')+r'''
static int clock_hold(void *v,uint64_t *out){struct fixture *f=v;f->now=us();*out=us();return 1;}
static int read_hold(void *v,uintptr_t a,uint32_t *out){
 struct fixture *f=v;f->now=us();
 if(a==0x055090e0){if(last_lock_ticks)assert(ticks-last_lock_ticks>=5*24);last_lock_ticks=ticks;}
 return read32(v,a,out);
}
static int write_hold(void *v,uintptr_t a,uint32_t value){
 struct fixture *f=v;
 if(f->writes==7){write_cost();command_ticks=ticks;}
 if(f->writes==8)assert(command_ticks&&ticks-command_ticks>=20*24);
 f->now=us();return write32(v,a,value);
}
int main(void){
 for(unsigned phase=0;phase<3;phase++)for(unsigned costly=0;costly<2;costly++){
  struct fixture f;setup(&f,0,480000000,120000000,600000000,100000000);
  ticks=24+(phase==2?23:phase);cost=costly?37:0;last_lock_ticks=command_ticks=0;
  f.pll.config.ops.now_us=clock_hold;f.pll.config.ops.read32=read_hold;f.pll.config.ops.write32=write_hold;
  for(unsigned n=0;f.pll.state==A733_DISPLAY_PLL_PENDING;n++){assert(n<500000);a733_display_pll_poll(&f.pll);++ticks;}
  assert(f.pll.state==A733_DISPLAY_PLL_READY_RETAINED&&f.updates==2);
 }
 struct fixture f;setup(&f,0,480000000,120000000,600000000,100000000);
 ticks=47;cost=37;last_lock_ticks=command_ticks=0;f.stuck_update=1;f.pll.config.deadline=200;
 f.pll.config.ops.now_us=clock_hold;f.pll.config.ops.read32=read_hold;f.pll.config.ops.write32=write_hold;
 for(unsigned n=0;f.pll.state==A733_DISPLAY_PLL_PENDING;n++){assert(n<10000);a733_display_pll_poll(&f.pll);++ticks;}
 assert(f.pll.state==A733_DISPLAY_PLL_FAULT_RETAINED&&f.writes==7&&f.retained);
 puts("PASS PLL fractional sample spacing, N hold, costly trigger and retained expiry");
}
'''
DSI = fixture('a733-dsi-phy.c')+r'''
static int clock_hold(void *v,uint64_t *out){struct model *m=v;m->now=us();*out=us();return 1;}
static int read_hold(void *v,uintptr_t a,uint32_t *out){
 struct model *m=v;m->now=us();
 if(a==0x055070e0){if(last_lock_ticks)assert(ticks-last_lock_ticks>=5*24);last_lock_ticks=ticks;}
 return read32(v,a,out);
}
static int write_hold(void *v,uintptr_t a,uint32_t value){
 struct model *m=v;
 if(!m->writes)assert(last_lock_ticks&&ticks-last_lock_ticks>=20*24);
 if(a==0x05507118){write_cost();analog_ticks=ticks;}
 if(a==0x05507058&&analog_ticks)assert(ticks-analog_ticks>=24);
 m->now=us();return write32(v,a,value);
}
int main(void){
 for(unsigned phase=0;phase<3;phase++)for(unsigned costly=0;costly<2;costly++){
  struct a733_dsi_phy p;struct model m;setup(&p,&m,0,4,480000000,80000000);
  ticks=24+(phase==2?23:phase);cost=costly?37:0;last_lock_ticks=analog_ticks=0;
  p.config.ops.now_us=clock_hold;p.config.ops.read32=read_hold;p.config.ops.write32=write_hold;
  for(unsigned n=0;p.state==A733_DSI_PHY_PENDING;n++){assert(n<100000);a733_dsi_phy_poll(&p);++ticks;}
  assert(p.state==A733_DSI_PHY_READY&&analog_ticks);
 }
 struct a733_dsi_phy p;struct model m;setup(&p,&m,0,4,480000000,80000000);
 ticks=47;last_lock_ticks=analog_ticks=0;m.no_lock=1;p.config.deadline=100;
 p.config.ops.now_us=clock_hold;p.config.ops.read32=read_hold;p.config.ops.write32=write_hold;
 for(unsigned n=0;p.state==A733_DSI_PHY_PENDING;n++){assert(n<10000);a733_dsi_phy_poll(&p);++ticks;}
 assert(p.state==A733_DSI_PHY_RETAINED&&!m.writes);
 puts("PASS DSI fractional LOCK/analog minima, costly trigger and retained expiry");
}
'''
HDMI = fixture('a733-hdmi.c')+r'''
static uint64_t pll_ticks, tx_ticks, command_sample;
static int clock_hold(void *v,uint64_t *out){struct model *m=v;m->time=us();*out=us();return 1;}
static int read32_hold(void *v,uintptr_t a,uint32_t *out){
 int r=read32(v,a,out);if(a==A733_HDMI_PHY_BASE+0x40&&(*out&1)&&!pll_ticks)pll_ticks=ticks;return r;
}
static int write32_hold(void *v,uintptr_t a,uint32_t value){
 if(a==A733_HDMI_PHY_BASE+0x20&&(value&0x08000000))assert(pll_ticks&&ticks-pll_ticks>=20*24);
 return write32(v,a,value);
}
static int write8_hold(void *v,uintptr_t a,uint8_t value){
 struct model *m=v;unsigned o=(unsigned)(a-A733_HDMI_BASE);
 if(o==0x3000&&!(value&0x20)){write_cost();reset_ticks=ticks;}
 if(o==0x3000&&(value&0x20)&&!(m->dw[o]&0x20))assert(ticks-reset_ticks>=5*24);
 if(o==0x3026){write_cost();command_ticks=ticks;command_sample=0;}
 if(o==0x3000&&(value&8)&&!(m->dw[o]&8)){write_cost();tx_ticks=ticks;}
 m->time=us();return write8(v,a,value);
}
static int read8_hold(void *v,uintptr_t a,uint8_t *out){
 if(a==A733_HDMI_BASE+0x108&&command_ticks){assert(ticks-(command_sample?command_sample:command_ticks)>=10*24);command_sample=ticks;}
 if(a==A733_HDMI_BASE+0x3004&&tx_ticks&&((struct model*)v)->owner->state==A733_HDMI_PHY){assert(ticks-tx_ticks>=5*24);tx_ticks=ticks;}
 return read8(v,a,out);
}
int main(void){
 for(unsigned phase=0;phase<3;phase++)for(unsigned costly=0;costly<2;costly++){
  setup(A733_HDMI_720P60,A733_HDMI_LIMITED);assert(a733_hdmi_prepare(&live.s,1000000)==1);
  run_to(A733_HDMI_PHY,0);
  ticks=live.m.time*24+(phase==2?23:phase);cost=costly?37:0;
  pll_ticks=tx_ticks=command_ticks=command_sample=reset_ticks=0;
  live.s.io.ops.now_us=clock_hold;live.s.io.ops.read32=read32_hold;live.s.io.ops.write32=write32_hold;
  live.s.io.ops.read8=read8_hold;live.s.io.ops.write8=write8_hold;
  for(unsigned n=0;live.s.state!=A733_HDMI_PREPARED;n++){
   assert(n<200000);live.m.time=us();assert(a733_hdmi_poll(&live.s)>=0);++ticks;
  }
  assert(pll_ticks&&reset_ticks&&command_ticks&&tx_ticks);
 }
 setup(A733_HDMI_720P60,A733_HDMI_LIMITED);assert(a733_hdmi_prepare(&live.s,1000000)==1);run_to(A733_HDMI_PHY,0);
 ticks=live.m.time*24+23;live.s.io.deadline=us()+30;live.m.top_unlock=1;pll_ticks=0;
 live.s.io.ops.now_us=clock_hold;live.s.io.ops.read32=read32_hold;
 for(unsigned n=0;!live.s.io.fault;n++){assert(n<10000);assert(a733_hdmi_poll(&live.s)>=-1);++ticks;}
 assert(live.s.state==A733_HDMI_FAULT_RETAINED&&!(live.m.phy[8]&0x08000000));
 puts("PASS HDMI fractional PLL/reset/I2C/TX minima, costly trigger and retained expiry");
}
'''
SENSOR = fixture('ov5640.c')+r'''
static uint64_t power_ticks,pwdn_ticks,mode_ticks,table_ticks;
static int clock_hold(void *v,uint64_t *out){struct model *m=v;m->now=us();*out=us();return 1;}
static int acquire_hold(void *v,uint64_t d,uint64_t *t){write_cost();((struct model*)v)->now=us();int r=acquire(v,d,t);power_ticks=ticks;return r;}
static int line_hold(void *v,uint64_t t,enum ov5640_line l,int a,uint64_t d){
 if(l==OV5640_PWDN_LINE)assert(ticks-power_ticks>=5000*24);
 else assert(ticks-pwdn_ticks>=1000*24);
 write_cost();((struct model*)v)->now=us();int r=line(v,t,l,a,d);
 if(l==OV5640_PWDN_LINE)pwdn_ticks=ticks;else reset_ticks=ticks;return r;
}
static void bus_hold(void){assert(reset_ticks&&ticks-reset_ticks>=20000*24);if(table_ticks)assert(ticks-table_ticks>=300000*24);if(mode_ticks)assert(ticks-mode_ticks>=500*24);}
static int read_hold(void *v,uint64_t t,uint8_t a,uint16_t r,uint8_t *out,uint64_t d){bus_hold();return read_reg(v,t,a,r,out,d);}
static int write_hold(void *v,uint64_t t,uint8_t a,uint16_t r,uint8_t value,uint64_t d){
 bus_hold();write_cost();((struct model*)v)->now=us();int result=write_reg(v,t,a,r,value,d);
 if(r==0x3c00&&((struct model*)v)->writes<300&&!table_ticks)table_ticks=ticks;
 if(r==0x3019)mode_ticks=ticks;return result;
}
int main(void){
 for(unsigned phase=0;phase<3;phase++)for(unsigned costly=0;costly<2;costly++){
  struct ov5640 s;struct model m;selected=&single;setup(&s,&m,1);
  ticks=24+(phase==2?23:phase);cost=costly?37:0;power_ticks=pwdn_ticks=reset_ticks=mode_ticks=table_ticks=0;
  s.ops.now_us=clock_hold;s.ops.acquire=acquire_hold;s.ops.line=line_hold;s.ops.read=read_hold;s.ops.write=write_hold;
  for(unsigned n=0;s.state!=OV5640_READY;n++){
   assert(n<100000);m.now=us();assert(ov5640_poll(&s)>=0);
   if(s.wait_until&&ticks<(s.wait_until-1)*24)ticks=(s.wait_until-1)*24;else ++ticks;
  }
  assert(mode_ticks&&table_ticks);
 }
 struct ov5640 s;struct model m;selected=&single;setup(&s,&m,1);ticks=47;cost=37;power_ticks=0;
 s.deadline=100;s.ops.now_us=clock_hold;s.ops.acquire=acquire_hold;s.ops.line=line_hold;
 for(unsigned n=0;s.state!=OV5640_FAULT;n++){assert(n<3000);m.now=us();ov5640_poll(&s);++ticks;}
 assert(!m.line_calls&&!m.writes);
 puts("PASS sensor fractional power/PWDN/reset/table/mode minima, costly callbacks and expiry");
}
'''
CAPTURE = fixture('a733-camera-capture.c')+r'''
static unsigned phase_ticks;
static uint64_t csi_effect,epoch_floor,epoch_ticks;
static int sample_clock(void *v,uint64_t *out){
 struct model *m=v;*out=m->now;
 if(m->capture->quiet_from&&m->capture->quiet_from!=epoch_floor){epoch_floor=m->capture->quiet_from;epoch_ticks=epoch_floor*24+phase_ticks;}
 return 1;
}
static void sample_cost(struct model *m){
 uint64_t t=m->now*24+phase_ticks+cost;m->now=t/24;phase_ticks=(unsigned)(t%24);
 if(m->capture->state==A733_CAPTURE_SAMPLE||m->capture->state==A733_CAPTURE_REARM_SAMPLE)csi_effect=t;
}
static int read_cost(void *v,uint64_t token,enum a733_csi_region region,uint16_t off,uint32_t *out,uint64_t d){sample_cost(v);return csi_read(v,token,region,off,out,d);}
static int write_sample_cost(void *v,uint64_t token,enum a733_csi_region region,uint16_t off,uint32_t value,uint64_t d){sample_cost(v);return csi_write(v,token,region,off,value,d);}
static int clock_hold(void *v,uint64_t *out){struct model *m=v;m->now=us();*out=us();return 1;}
int main(void){
 for(unsigned target=0;target<2;target++)for(unsigned phase=0;phase<3;phase++)for(unsigned costly=0;costly<2;costly++){
  model.retained=1;setup(0);phase_ticks=phase==2?23:phase;cost=costly?37:0;csi_effect=epoch_floor=epoch_ticks=0;capture.config.ops.now_us=sample_clock;
  capture.config.csi.read=read_cost;capture.config.csi.write=write_sample_cost;
  assert(drive()==1&&capture.state==A733_CAPTURE_READABLE);
  assert(a733_camera_capture_rearm(&capture,model.now+10000000)==1);
  enum a733_capture_state waiting=target?A733_CAPTURE_REARM_WAIT:A733_CAPTURE_REARM_QUIET;
  while(capture.state!=waiting){assert(a733_camera_capture_poll(&capture)==0);model.now+=1000;}
  /* The actual preceding receiver sample supplied quiet_from. Model its
   * completion within that microsecond, rather than inventing a new epoch. */
  uint64_t origin=epoch_ticks;assert(epoch_floor==capture.quiet_from&&origin>=csi_effect);
  capture.config.ops.now_us=clock_hold;ticks=(capture.quiet_from+capture.quiet_us)*24;model.now=us();
  unsigned calls=model.calls;
  assert(ticks-origin<=capture.quiet_us*24);
  for(unsigned i=0;i<3;i++)assert(a733_camera_capture_poll(&capture)==0);
  assert(model.calls==calls&&capture.state==waiting);
  ticks+=24;model.now=us();assert(ticks-origin>capture.quiet_us*24);
  assert(a733_camera_capture_poll(&capture)==0&&capture.state==(target?A733_CAPTURE_REARM_RECEIVER:A733_CAPTURE_REARM_SAMPLE));
  capture.deadline=model.now;calls=model.calls;assert(a733_camera_capture_poll(&capture)==-1&&calls==model.calls);
 }
 puts("PASS capture actual post-sample quiet epoch, fractional/costly exact-boundary refusal and expiry");
}
'''


def dp_wait():
    return CLOCK+r'''
#include <stdio.h>
static struct {uint64_t deadline;} app;
static uint64_t yields;
static int dp_now(void *v,uint64_t *out){(void)v;*out=us();return *out<app.deadline;}
static void yield_one(void){++yields;++ticks;}
static void delay_tick(void){++yields;ticks=(ticks/24000+1)*24000;}
#define taskYIELD() yield_one()
#define vTaskDelay(n) ((void)(n),delay_tick())
'''+extract('apps/displayport/native.c','wait_io')+r'''
int main(void){
 static const unsigned periods[]={5,10,1000,10000};
 for(unsigned p=0;p<4;p++)for(unsigned phase=0;phase<3;phase++)for(unsigned costly=0;costly<2;costly++){
  ticks=2400+(phase==2?23:phase);ticks+=costly?37:0;uint64_t effect=ticks;app.deadline=100000;yields=0;
  assert(wait_io(NULL,periods[p],app.deadline)==1&&ticks-effect>=periods[p]*24&&yields);
 }
 ticks=2423;app.deadline=us()+5;assert(wait_io(NULL,5,app.deadline)==0);
 ticks=2423;app.deadline=us()+6;assert(wait_io(NULL,5,app.deadline)==0);
 puts("PASS actual DP wait helper fractional minima, costly prior write and unchanged deadline");
}
'''


def video_reset():
    return CLOCK+r'''
#include "a733_video_platform.h"
#include <stdio.h>
#define CCU UINT32_C(0x02002000)
#define VE UINT32_C(0x01c0e000)
#define BIT(n) (UINT32_C(1)<<(n))
static unsigned releases;
static int fail(struct a733_video_unit *u){u->failed=1;return -1;}
static int timely(struct a733_video_unit *u){u->last_now=us();return !u->failed&&us()<u->deadline;}
static int lock(struct a733_video_unit *u){u->locked=1;return timely(u);}
static int unlock(struct a733_video_unit *u){u->locked=0;return timely(u);}
static int held(struct a733_video_unit *u){(void)u;return 1;}
int a733_video_unit_identity(struct a733_video_unit *u,uint64_t *v){(void)u;(void)v;assert(0);return 0;}
static int update(struct a733_video_unit *u,uintptr_t a,uint32_t mask,uint32_t value){
 assert(a==CCU+0xa8c&&mask==BIT(18));
 if(!value){write_cost();reset_ticks=ticks;}
 else {assert(reset_ticks&&ticks-reset_ticks>=10*24);++releases;}
 ticks+=41;u->last_now=us();return timely(u);
}
'''+extract('drivers/video/a733_video_platform.c','prep_step')+extract('drivers/video/a733_video_platform.c','prepare')+r'''
int main(void){
 for(unsigned phase=0;phase<3;phase++)for(unsigned costly=0;costly<2;costly++)for(unsigned expire=0;expire<2;expire++){
  struct a733_video_decoder p={.checked=1,.mapped=1,.version=UINT64_C(0x3332100021321),.step=1};
  p.unit.token=42;p.unit.deadline=100000;ticks=2400+(phase==2?23:phase);cost=costly?37:0;releases=0;
  assert(prepare(&p,42,NULL,p.unit.deadline,0)==0&&p.step==2&&reset_ticks);
  uint64_t target=p.reset_until;ticks=(target-1)*24;
  if(expire)p.unit.deadline=us();
  assert(prepare(&p,42,NULL,p.unit.deadline,0)==(expire?-1:0)&&!releases);
  if(expire){assert(p.unit.failed);continue;}
  ticks+=24;assert(prepare(&p,42,NULL,p.unit.deadline,0)==0&&p.step==3&&releases==1);
 }
 puts("PASS actual video reset preparation, fractional/costly hold and original deadline");
}
'''


def framebuffer_reset():
    return CLOCK+r'''
#include "a733_display_framebuffer.h"
#include <stdio.h>
typedef struct a733_display_framebuffer fb_t;
#define CCU UINT32_C(0x02002000)
static int timely(fb_t *f){f->last_now=us();return us()<f->config.deadline;}
static int update(fb_t *f,uintptr_t a,uint32_t mask,uint32_t value){
 assert(a==CCU+0xa04&&mask==0x10000&&!value);write_cost();reset_ticks=ticks;ticks+=41;f->last_now=us();return 1;
}
static int read32(void *f,uintptr_t a,uint32_t *out){(void)f;(void)a;(void)out;assert(0);return 0;}
int a733_power_domain_check(struct a733_power_domains *d,unsigned n,unsigned *state){(void)d;(void)n;(void)state;assert(0);return 0;}
static int domain_authorize(fb_t *f){(void)f;assert(0);return -1;}
'''+extract('drivers/display/a733_display_framebuffer.c','cold_step')+r'''
int main(void){
 for(unsigned phase=0;phase<3;phase++)for(unsigned costly=0;costly<2;costly++)for(unsigned expire=0;expire<2;expire++){
  fb_t f={.index=2};f.config.deadline=100000;ticks=2400+(phase==2?23:phase);cost=costly?37:0;
  assert(cold_step(&f)==0&&f.index==3&&reset_ticks);uint64_t target=f.until;ticks=(target-1)*24;
  if(expire)f.config.deadline=us();
  assert(cold_step(&f)==(expire?-1:0)&&f.index==3&&!f.domain_requested);
  if(expire)continue;
  ticks+=24;assert(cold_step(&f)==0&&f.index==4&&ticks-reset_ticks>=10*24&&!f.domain_requested);
 }
 puts("PASS actual framebuffer reset phase, fractional/costly hold and original deadline");
}
'''


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--evidence',type=Path,required=True)
    p.add_argument('--dsi-reference',type=Path,required=True)
    p.add_argument('--case',choices=['pll','dsi','hdmi','sensor','capture','dp','video','framebuffer','all'],default='all')
    p.add_argument('--cc',default=str(harness.CLANG))
    a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True)
    cases={
        'pll':(PLL,['drivers/display/a733_display_pll.c','drivers/display/a733_dsi_phy.c','drivers/clock/a733_clock_leases.c']),
        'dsi':(DSI,['drivers/display/a733_dsi_phy.c','drivers/display/a733_dsi.c',str(a.dsi_reference)]),
        'hdmi':(HDMI,['drivers/display/a733_hdmi.c','drivers/display/a733_hdmi_phy.c','drivers/display/a733_tcon_tv.c']),
        'sensor':(SENSOR,['drivers/camera/ov5640.c']),
        'capture':(CAPTURE,['drivers/camera/ov5640.c','drivers/camera/a733_camera_platform.c','drivers/camera/a733_csi_receiver.c','drivers/camera/a733_camera_route.c','drivers/camera/a733_camera_dma.c','drivers/iommu/a733_iommu.c','drivers/camera/a733_camera_capture.c']),
        'dp':(dp_wait(),[]),
        'video':(video_reset(),[]),
        'framebuffer':(framebuffer_reset(),[]),
    }
    commands=[];results={};dependencies=set()
    def run(command):
        r=subprocess.run(command,cwd=ROOT,env=harness.environment(),capture_output=True,text=True)
        commands.append(dict(command=command,returncode=r.returncode,stdout=r.stdout,stderr=r.stderr))
        (a.evidence/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
        assert r.returncode==0,r.stdout+r.stderr
        return r.stdout
    flags=['-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
           '-Wno-misleading-indentation','-Wno-unused-but-set-variable','-Wno-sign-compare',
           '-Wno-return-type','-Wno-unused-variable','-Wno-unused-function',*harness.SANITIZE,
           '-ffunction-sections','-fdata-sections','-Iinclude','-Ichecks','-Ichecks/npu-startup-host']
    for name,(source,units) in cases.items():
        if a.case not in ('all',name):continue
        src=a.evidence/(name+'.c');src.write_text(source);binary=a.evidence/name;objects=[]
        for index,unit in enumerate([str(src),*units]):
            obj=a.evidence/f'{name}-{index}.o';dep=a.evidence/f'{name}-{index}.d'
            run([a.cc,*flags,'-MD','-MF',str(dep),'-c',unit,'-o',str(obj)])
            objects.append(str(obj))
            for item in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]):
                path=Path(item);dependencies.add(path if path.is_absolute() else ROOT/path)
        run([a.cc,*harness.SANITIZE,'-Wl,--gc-sections',*objects,'-o',str(binary)])
        results[name]=run([str(binary)]);print(results[name],end='')
    inputs=[Path(__file__),Path(harness.__file__),a.dsi_reference,*dependencies]
    inputs += [ROOT/u for _,units in cases.values() for u in units if not Path(u).is_absolute()]
    inputs += [ROOT/'checks'/name for name in ['a733-display-pll.c','a733-dsi-phy.c','a733-hdmi.c','ov5640.c','a733-camera-capture.c']]
    inputs += [ROOT/u for u in ('apps/displayport/native.c','drivers/video/a733_video_platform.c','drivers/display/a733_display_framebuffer.c')]
    (a.evidence/'results.json').write_text(json.dumps(dict(passed=True,physical=False,scope=__doc__,results=results,inputs={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}),indent=2)+'\n')

if __name__=='__main__':main()
