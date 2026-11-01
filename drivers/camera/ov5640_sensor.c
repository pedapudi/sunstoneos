/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "camera_sensor.h"
#include "ov5640.h"
#include <string.h>
_Static_assert(sizeof(struct ov5640)<=CAMERA_SENSOR_PRIVATE_BYTES,"sensor private storage");
static struct ov5640 *native(struct camera_sensor *s){return (void *)s->private.bytes;}
static int now(void *p,uint64_t *n){struct camera_sensor *s=p;return s->ops.now_us(s->context,n);}
static int acquire(void *p,uint64_t d,uint64_t *t){struct camera_sensor *s=p;return s->ops.acquire(s->context,d,t);}
static int line(void *p,uint64_t t,enum ov5640_line l,int a,uint64_t d){struct camera_sensor *s=p;return s->ops.line(s->context,t,(enum camera_sensor_line)l,a,d);}
static int read_reg(void *p,uint64_t t,uint8_t a,uint16_t r,uint8_t *v,uint64_t d){struct camera_sensor *s=p;return s->ops.read(s->context,t,a,r,v,d);}
static int write_reg(void *p,uint64_t t,uint8_t a,uint16_t r,uint8_t v,uint64_t d)
{
    struct camera_sensor *s=p;
    if(native(s)->state==OV5640_STARTING && r==0x4202 && !v && s->ops.before_stream &&
       s->ops.before_stream(s->context,d)!=1)return -1;
    return s->ops.write(s->context,t,a,r,v,d);
}
static int receiver(void *p,uint64_t t,uint64_t rt,const struct camera_mode *m,uint64_t d){struct camera_sensor *s=p;return s->ops.receiver(s->context,t,rt,m,d);}
static int release(void *p,uint64_t t,uint64_t d){struct camera_sensor *s=p;return s->ops.release(s->context,t,d);}
static int block(void *p,uint64_t t,uint8_t a,uint16_t r,const uint8_t *v,unsigned n,uint64_t d){struct camera_sensor *s=p;return s->ops.write_block(s->context,t,a,r,v,n,d);}
static void sync_state(struct camera_sensor *s)
{
    struct ov5640 *o=native(s);s->state=(enum camera_sensor_state)o->state;
    s->token=o->token;s->mode=o->mode;s->index=o->index;s->wait_until=o->wait_until;s->now=o->now;
}
static int enumerate(unsigned i,unsigned f,unsigned v,struct camera_mode *m)
{return ov5640_mode_get(m,(enum ov5640_size)i,(enum ov5640_format)f,v);}
static int bind(struct camera_sensor *s)
{
    struct ov5640_ops o={now,acquire,line,read_reg,write_reg,receiver,release,s->ops.write_block?block:NULL};
    if(ov5640_init(native(s),&o,s,s->test_pattern)!=1 || ov5640_select(native(s),&s->mode)!=1)return -1;
    /* The class has completed the identical reference power holds and both ID
     * reads. Continue at configuration without replaying any hardware effect. */
    native(s)->token=s->token;native(s)->deadline=s->deadline;native(s)->now=s->now;
    native(s)->state=OV5640_CONFIGURE;sync_state(s);return 1;
}
static int start(struct camera_sensor *s,uint64_t t,uint64_t d){int r=ov5640_start(native(s),t,d);sync_state(s);return r;}
static int stop(struct camera_sensor *s,uint64_t d){int r=ov5640_stop(native(s),d);sync_state(s);return r;}
static int close_sensor(struct camera_sensor *s,uint64_t d){int r=ov5640_close(native(s),d);sync_state(s);return r;}
static int poll_sensor(struct camera_sensor *s){int r=ov5640_poll(native(s));sync_state(s);return r;}
static int control(struct camera_sensor *s,enum camera_sensor_control c,int v)
{
    if(s->state!=CAMERA_SENSOR_NEW || c!=CAMERA_SENSOR_TEST_PATTERN || (v!=0 && v!=1))return -1;
    s->test_pattern=(uint8_t)v;return 1;
}
static int control_query(enum camera_sensor_control c,struct camera_sensor_control_info *info)
{
    if(c!=CAMERA_SENSOR_TEST_PATTERN)return 0;
    *info=(struct camera_sensor_control_info){0,1,1,0,1};return 1;
}
const struct camera_sensor_driver camera_sensor_ov5640={"ov5640",0x3c,{0x300a,0x300b},{0x56,0x40},5000,1000,20000,10,enumerate,bind,start,stop,close_sensor,poll_sensor,control,control_query};
