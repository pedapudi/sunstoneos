/* SPDX-License-Identifier: GPL-2.0-only */
#include "camera_sensor.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
struct model {
    uint64_t now,acquired,pwdn,reset;
    uint8_t regs[65536];
    unsigned calls,reads,writes,armed,hook,release;
    unsigned bad_read,zero_write,wrong_id,deny_receiver;
};
static int now(void *p,uint64_t *n){*n=((struct model *)p)->now;return 1;}
static int acquire(void *p,uint64_t d,uint64_t *t)
{struct model *m=p;assert(m->now<d);m->acquired=m->now;*t=31;++m->calls;return 1;}
static int line(void *p,uint64_t t,enum camera_sensor_line l,int asserted,uint64_t d)
{
    struct model *m=p;assert(t==31 && m->now<d && !asserted);++m->calls;
    if(l==CAMERA_SENSOR_PWDN_LINE){assert(m->now-m->acquired>5000);m->pwdn=m->now;}
    else {assert(m->now-m->pwdn>1000);m->reset=m->now;}return 1;
}
static int read_reg(void *p,uint64_t t,uint8_t a,uint16_t r,uint8_t *v,uint64_t d)
{
    struct model *m=p;assert(t==31 && (a==0x3c || a==0x36) && m->now<d);++m->calls;++m->reads;
    assert(m->now-m->reset>20000);
    if(m->bad_read)return -1;
    *v=m->regs[r];return 1;
}
static int write_reg(void *p,uint64_t t,uint8_t a,uint16_t r,uint8_t v,uint64_t d)
{
    struct model *m=p;assert(t==31 && (a==0x3c || a==0x36) && m->now<d);++m->calls;++m->writes;
    if(a==0x36 && (r==0x0103 || r==0x0100))assert(m->armed && m->hook);
    if(m->zero_write)return 0;
    if(r==0x4202 && !v)assert(m->armed && m->hook);
    m->regs[r]=v;return 1;
}
static int receiver(void *p,uint64_t t,uint64_t r,const struct camera_mode *mode,uint64_t d)
{
    struct model *m=p;assert(t==31 && r==41 && mode->width==640 && m->now<d);++m->calls;
    if(m->deny_receiver)return 0;
    m->armed=1;return 1;
}
static int release(void *p,uint64_t t,uint64_t d)
{struct model *m=p;assert(t==31 && m->now<d);++m->calls;++m->release;return 1;}
static int hook(void *p,uint64_t d)
{struct model *m=p;assert(m->armed && m->now<d);++m->hook;return 1;}
static unsigned dispatched;
static int custom_control(struct camera_sensor *s,enum camera_sensor_control c,int v)
{(void)s;assert(c==CAMERA_SENSOR_GAIN && v==77);++dispatched;return 1;}
static const struct camera_sensor_bus_ops ops={now,acquire,line,read_reg,write_reg,receiver,release,NULL,hook};
static struct camera_sensor_bus bus_for(unsigned second)
{
    return (struct camera_sensor_bus){.lanes=2,.i2c_address=second?0x36:0x3c,
        .xclk_hz=second?25000000:24000000,.reset={0,0,0,1},
        .power_hold_us=5000,.reset_hold_us=1000,.bus_hold_us=20000};
}
static int drive(struct camera_sensor *s,struct model *m)
{
    for(unsigned i=0;i<2000;i++){
        unsigned before=m->calls;int r=camera_sensor_poll(s);assert(m->calls-before<=1);
        if(r)return r;
        m->now+=1000;if(s->wait_until>m->now)m->now=s->wait_until;
    }assert(0);return -1;
}
int main(void)
{
    struct camera_sensor_control_info info;
    assert(camera_sensor_control_query(&camera_sensor_ov5640,CAMERA_SENSOR_TEST_PATTERN,&info)==1 && info.before_open);
    assert(camera_sensor_control_query(&camera_sensor_ov5640,CAMERA_SENSOR_GAIN,&info)==0);
    assert(camera_sensor_control_query(&camera_sensor_ov5647,CAMERA_SENSOR_EXPOSURE,&info)==0);
    for(unsigned second=0;second<2;second++){
        const struct camera_sensor_driver *driver=second?&camera_sensor_ov5647:&camera_sensor_ov5640;
        struct camera_mode mode;assert(camera_sensor_mode(driver,second?0:3,second?CAMERA_SBGGR10:CAMERA_UYVY,0,&mode)==1);
        /* Padding is caller-owned and must never affect negotiation. */
        memset((uint8_t *)&mode+offsetof(struct camera_mode,format)+1,0xa5,
            sizeof(mode)-offsetof(struct camera_mode,format)-1);
        struct camera_sensor_bus bus=bus_for(second);
        struct camera_sensor s={0};struct model m={.now=1};m.regs[0x300a]=0x56;m.regs[0x300b]=second?0x47:0x40;
        assert(camera_sensor_init(&s,&ops,&m,&bus,camera_sensor_drivers,camera_sensor_driver_count,&mode,1)==1);
        assert(camera_sensor_control_set(&s,CAMERA_SENSOR_EXPOSURE,100)==-1 && !m.calls);
        assert(camera_sensor_control_set(&s,CAMERA_SENSOR_TEST_PATTERN,1)==1 && !m.calls);
        assert(camera_sensor_open(&s,10000000)==1 && drive(&s,&m)==1 && s.driver==driver);
        if(second)assert(!m.regs[0x0100] && !m.hook && m.writes==6);
        assert(camera_sensor_start(&s,41,10000000)==1);
        if(second){m.deny_receiver=1;unsigned before=m.writes;assert(camera_sensor_poll(&s)==0 && m.writes==before);m.deny_receiver=0;}
        assert(drive(&s,&m)==1 && s.state==CAMERA_SENSOR_STREAMING && m.hook==1);
        assert(camera_sensor_stop(&s,10000000)==1 && drive(&s,&m)==1);
        assert(camera_sensor_close(&s,10000000)==1 && drive(&s,&m)==1 && !s.token && m.release==1);
    }
    for(unsigned failure=0;failure<4;failure++){
        struct camera_sensor s={0};struct model m={.now=1};struct camera_sensor_bus bus=bus_for(1);
        struct camera_mode mode;assert(camera_sensor_mode(&camera_sensor_ov5647,0,CAMERA_SBGGR10,0,&mode)==1);
        m.regs[0x300a]=0x56;m.regs[0x300b]=failure==0?0:0x47;m.bad_read=failure==1;m.zero_write=failure==2;
        if(failure==3){bus.bus_hold_us=1;assert(camera_sensor_init(&s,&ops,&m,&bus,camera_sensor_drivers,camera_sensor_driver_count,&mode,0)==-1 && !m.calls);continue;}
        assert(camera_sensor_init(&s,&ops,&m,&bus,camera_sensor_drivers,camera_sensor_driver_count,&mode,0)==1);
        assert(camera_sensor_open(&s,10000000)==1 && drive(&s,&m)==-1 && s.state==CAMERA_SENSOR_FAULT && s.token==31);
        unsigned before=m.calls;assert(camera_sensor_poll(&s)==-1 && m.calls==before && !m.release);
        if(failure<2)assert(!m.writes);
    }
    /* Same-address nonmatching ID advances once; a transport error never does. */
    struct camera_sensor_driver wrong=camera_sensor_ov5640;wrong.id_value[0]=0xab;
    const struct camera_sensor_driver *drivers[]={&wrong,&camera_sensor_ov5640};
    struct camera_sensor s={0};struct model m={.now=1};struct camera_sensor_bus bus=bus_for(0);struct camera_mode mode;
    m.regs[0x300a]=0x56;m.regs[0x300b]=0x40;assert(camera_sensor_mode(&camera_sensor_ov5640,3,CAMERA_UYVY,0,&mode)==1);
    assert(camera_sensor_init(&s,&ops,&m,&bus,drivers,2,&mode,0)==1 && camera_sensor_open(&s,10000000)==1);
    assert(drive(&s,&m)==1 && s.candidate==1 && s.driver==&camera_sensor_ov5640);
    s.driver=&wrong;wrong.control=custom_control;
    assert(camera_sensor_control_set(&s,CAMERA_SENSOR_GAIN,77)==1 && dispatched==1);
    puts("PASS sensor ID selection, mode negotiation, power holds, receiver-before-stream ordering, controls, stop/readback and terminal bus faults");
}
