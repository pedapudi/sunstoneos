/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "diagnostic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct model { uint64_t now,on_at,off_at,peak;unsigned output,writes,delay,off_delay,stuck_rise,stuck_fall,accessory,io_fault;uint8_t regs[256]; };
static uint64_t now(void *p){return ((struct model *)p)->now;}
static int output(void *p,unsigned on)
{struct model *m=p;if(on&&!m->output){m->on_at=m->now;++m->writes;}
 if(!on&&m->output){m->now+=m->off_delay;m->off_at=m->now;m->peak=m->now-m->on_at;}m->output=on;return 1;}
static int io(void *p,unsigned reg,unsigned read,uint8_t *value)
{
    struct model *m=p;if(m->io_fault)return -1;
    if(!m->delay){m->delay=1;return 0;}m->delay=0;
    if(read){
        *value=m->regs[reg];
        if(reg==0xc)*value=0; /* reset completion occurs after transaction */
        if(reg==0x40){
            *value=m->accessory?0:0x20;
            if((m->output&&!m->stuck_rise&&m->now-m->on_at>=20000)||
               (!m->output&&m->off_at&&(m->stuck_fall||m->now-m->off_at<30000)))*value|=0x80;
        }
    }else m->regs[reg]=*value;
    return 1;
}
static void run(unsigned mode,unsigned scenario)
{
    struct model m={0};m.regs[1]=0x81;m.regs[0x41]=0x28;
    m.off_delay=scenario==7?700000:0;
    m.stuck_rise=scenario==1;m.stuck_fall=scenario==2;m.accessory=scenario==3;
    const struct vbus_ops ops={now,io,output};struct vbus_diagnostic s;
    vbus_begin(&s,&ops,&m,mode,14000000);
    unsigned injected=0;
    for(unsigned i=0;i<200000&&!s.done;i++){
        m.now+=100;
        if(s.enabled&&!injected&&((scenario>=4&&scenario<=6)||scenario==8||scenario==9)){injected=1;
            if(scenario==4)m.now+=2100;
            if(scenario==5)m.io_fault=1;
            if(scenario==6)s.deadline=m.now;
            if(scenario==8)m.regs[0x41]|=2; /* OVRTEMP */
            if(scenario==9)m.regs[0x41]|=1; /* VCONNOCP */
        }
        vbus_step(&s);
    }
    assert(s.done);assert(!m.output);
    if(!scenario){assert(s.done==1&&!s.failure&&s.fell);assert(m.writes==(mode!=0));
        if(mode){assert(s.rose&&s.rise_us>=20000&&s.fall_us>=30000);assert(m.peak<=((uint64_t)mode*1000000)+100);}}
    else {assert(s.failure);if(scenario==3)assert(!m.writes);if(scenario>=4&&scenario<=6)assert(injected);}
    if(scenario==8||scenario==9)assert(s.failure==0x15&&injected&&m.writes==1);
    if(scenario==7)assert(s.failure==0x19&&s.done==2&&m.now-s.off_at>=650000);
    if(scenario==1)assert(s.failure==0x16);
    if(scenario==2)assert(s.failure==0x19&&s.done==2);
    if(scenario==4||scenario==6)assert(s.failure==0x10);
    if(scenario==5)assert(s.failure==0x20&&s.done==2);
}
int main(void)
{run(0,0);run(1,0);run(10,0);for(unsigned i=1;i<=9;++i)run(1,i);puts("PASS VBUS actual state machine: control,1s,10s,delayed rise/fall,stuck rise/fall,accessory,I2C fault,service gap,session deadline,delayed GPIO shutdown,STATUS1 RX/TX empty,thermal/VCONN faults");}
