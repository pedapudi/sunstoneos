/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Native control for OmniVision OV5647 cameras.
 * Based on Samsung S5K6AAFX SXGA 1/6" 1.3M CMOS Image Sensor driver
 * Copyright (C) 2011 Sylwester Nawrocki <s.nawrocki@samsung.com>
 * Based on Omnivision OV7670 Camera Driver
 * Copyright (C) 2006-7 Jonathan Corbet <corbet@lwn.net>
 * Copyright (C) 2016, Synopsys, Inc.
 */
#include "camera_sensor.h"
#include <string.h>
struct regval { uint16_t reg; uint8_t value; };
static const struct regval vga[] = {
	{0x0100, 0x00},
	{0x0103, 0x01},
	{0x3035, 0x11},
	{0x3036, 0x46},
	{0x303c, 0x11},
	{0x3821, 0x07},
	{0x3820, 0x41},
	{0x370c, 0x03},
	{0x3612, 0x59},
	{0x3618, 0x00},
	{0x5000, 0x06},
	{0x5003, 0x08},
	{0x5a00, 0x08},
	{0x3000, 0xff},
	{0x3001, 0xff},
	{0x3002, 0xff},
	{0x301d, 0xf0},
	{0x3a18, 0x00},
	{0x3a19, 0xf8},
	{0x3c01, 0x80},
	{0x3b07, 0x0c},
	{0x380c, 0x07},
	{0x380d, 0x3c},
	{0x3814, 0x35},
	{0x3815, 0x35},
	{0x3708, 0x64},
	{0x3709, 0x52},
	{0x3808, 0x02},
	{0x3809, 0x80},
	{0x380a, 0x01},
	{0x380b, 0xe0},
	{0x3800, 0x00},
	{0x3801, 0x10},
	{0x3802, 0x00},
	{0x3803, 0x00},
	{0x3804, 0x0a},
	{0x3805, 0x2f},
	{0x3806, 0x07},
	{0x3807, 0x9f},
	{0x3630, 0x2e},
	{0x3632, 0xe2},
	{0x3633, 0x23},
	{0x3634, 0x44},
	{0x3620, 0x64},
	{0x3621, 0xe0},
	{0x3600, 0x37},
	{0x3704, 0xa0},
	{0x3703, 0x5a},
	{0x3715, 0x78},
	{0x3717, 0x01},
	{0x3731, 0x02},
	{0x370b, 0x60},
	{0x3705, 0x1a},
	{0x3f05, 0x02},
	{0x3f06, 0x10},
	{0x3f01, 0x0a},
	{0x3a08, 0x01},
	{0x3a09, 0x2e},
	{0x3a0a, 0x00},
	{0x3a0b, 0xfb},
	{0x3a0d, 0x02},
	{0x3a0e, 0x01},
	{0x3a0f, 0x58},
	{0x3a10, 0x50},
	{0x3a1b, 0x58},
	{0x3a1e, 0x50},
	{0x3a11, 0x60},
	{0x3a1f, 0x28},
	{0x4001, 0x02},
	{0x4004, 0x02},
	{0x4000, 0x09},
	{0x3000, 0x00},
	{0x3001, 0x00},
	{0x3002, 0x00},
	{0x3017, 0xe0},
	{0x301c, 0xfc},
	{0x3636, 0x06},
	{0x3016, 0x08},
	{0x3827, 0xec},
	{0x3018, 0x44},
	{0x3035, 0x21},
	{0x3106, 0xf5},
	{0x3034, 0x1a},
	{0x301c, 0xf8},
	{0x4800, 0x34},
	{0x3503, 0x03},
	{0x0100, 0x01},
};
static const struct regval quiet[]={{0x3000,0x0f},{0x3001,0xff},{0x3002,0xe4},
    {0x4800,0x25},{0x4202,0x0f},{0x300d,1}};
static const struct regval on[]={{0x4800,4},{0x4202,0},{0x300d,0}};
static const struct regval off[]={{0x4800,0x25},{0x4202,0x0f},{0x300d,1}};
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
struct local { uint64_t receiver; unsigned phase; uint8_t saved,preparing; };
static struct local *local(struct camera_sensor *s){return (void *)s->private.bytes;}
static int enumerate(unsigned i,unsigned format,unsigned vtotal,struct camera_mode *m)
{
    if(i!=0 || format!=CAMERA_SBGGR10 || (vtotal && vtotal!=504))return -1;
    *m=(struct camera_mode){25000000,137500000,55000000,640,480,1852,504,2,0,0x2b,0,0,CAMERA_SBGGR10};return 1;
}
static int bind(struct camera_sensor *s)
{
    memset(s->private.bytes,0,sizeof(s->private.bytes));local(s)->preparing=1;s->state=CAMERA_SENSOR_CONFIGURE;return 1;
}
static int start(struct camera_sensor *s,uint64_t receiver,uint64_t deadline)
{
    if(!receiver || !deadline)return -1;
    local(s)->receiver=receiver;s->deadline=deadline;s->state=CAMERA_SENSOR_RECEIVER;s->index=0;return 1;
}
static int stop(struct camera_sensor *s,uint64_t deadline)
{
    if(!deadline)return -1;
    s->deadline=deadline;s->state=CAMERA_SENSOR_STOPPING;s->index=0;return 1;
}
static int close_sensor(struct camera_sensor *s,uint64_t deadline)
{
    if(!deadline)return -1;
    s->deadline=deadline;s->state=CAMERA_SENSOR_RELEASING;return 1;
}
static int read_reg(struct camera_sensor *s,uint16_t reg,uint8_t *v)
{return s->ops.read(s->context,s->token,s->bus.i2c_address,reg,v,s->deadline);}
static int write_reg(struct camera_sensor *s,uint16_t reg,uint8_t v)
{return s->ops.write(s->context,s->token,s->bus.i2c_address,reg,v,s->deadline);}
static int poll_sensor(struct camera_sensor *s)
{
    if(s->state==CAMERA_SENSOR_FAULT)return -1;
    if(s->state==CAMERA_SENSOR_READY || s->state==CAMERA_SENSOR_STREAMING || s->state==CAMERA_SENSOR_CLOSED)return 1;
    if(camera_sensor_time(s)!=1)return -1;
    if(s->wait_until){if(s->now<s->wait_until)return 0;s->wait_until=0;}
    int r=-1;uint8_t v;struct local *l=local(s);
    switch(s->state){
    case CAMERA_SENSOR_CONFIGURE:
        if(l->preparing){
            if(s->index==COUNT(quiet)){s->state=CAMERA_SENSOR_READY;s->index=0;return 1;}
            const struct regval *e=&quiet[s->index++];r=write_reg(s,e->reg,e->value);break;
        }
        if(s->index<COUNT(vga)){
            const struct regval *e=&vga[s->index++];r=write_reg(s,e->reg,e->value);
            /* Software reset completion has no readable self-clear contract in
             * the pinned driver. Hold 5 ms before the next register access. */
            if(r==1 && e->reg==0x0103){
                if(camera_sensor_time(s)!=1 || UINT64_MAX-s->now<=5000)return camera_sensor_fault(s);
                s->wait_until=s->now+5001;
            }break;
        }
        switch(l->phase++){
        case 0:r=read_reg(s,0x4814,&l->saved);break;
        case 1:r=write_reg(s,0x4814,(uint8_t)(l->saved&~0xc0u));break;
        case 2:r=write_reg(s,0x380e,1);break;
        case 3:r=write_reg(s,0x380f,0xf8);break;
        case 4:r=write_reg(s,0x503d,s->test_pattern?0x80:0);break;
        case 5:r=read_reg(s,0x0100,&v);if(r==1 && !(v&1))r=-1;break;
        case 6:r=read_reg(s,0x503d,&v);if(r==1 && v!=(s->test_pattern?0x80:0))r=-1;break;
        default:s->state=CAMERA_SENSOR_STARTING;s->index=0;return 0;
        }break;
    case CAMERA_SENSOR_RECEIVER:
        r=s->ops.receiver(s->context,s->token,l->receiver,&s->mode,s->deadline);
        if(r==0)return camera_sensor_time(s)==1?0:-1;
        if(r==1){
            if(s->ops.before_stream && s->ops.before_stream(s->context,s->deadline)!=1)return camera_sensor_fault(s);
            l->preparing=0;l->phase=0;s->index=0;s->state=CAMERA_SENSOR_CONFIGURE;
        }break;
    case CAMERA_SENSOR_STARTING:
    case CAMERA_SENSOR_STOPPING: {
        int starting=s->state==CAMERA_SENSOR_STARTING;
        const struct regval *e=&(starting?on:off)[s->index%COUNT(on)];
        if(s->index<COUNT(on)){
            r=write_reg(s,e->reg,e->value);
        }else{r=read_reg(s,e->reg,&v);if(r==1 && v!=e->value)r=-1;}
        if(r==1 && ++s->index==2*COUNT(on)){s->state=starting?CAMERA_SENSOR_STREAMING:CAMERA_SENSOR_READY;}
        break;}
    case CAMERA_SENSOR_RELEASING:
        r=s->ops.release(s->context,s->token,s->deadline);
        if(r==1){s->token=0;s->state=CAMERA_SENSOR_CLOSED;}break;
    default:return camera_sensor_fault(s);
    }
    if((r!=1 && !(r==0 && s->state==CAMERA_SENSOR_RELEASING)) || camera_sensor_time(s)!=1)return camera_sensor_fault(s);
    return s->state==CAMERA_SENSOR_READY || s->state==CAMERA_SENSOR_STREAMING || s->state==CAMERA_SENSOR_CLOSED?1:0;
}
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
const struct camera_sensor_driver camera_sensor_ov5647={"ov5647",0x36,{0x300a,0x300b},{0x56,0x47},0,0,20000,1,enumerate,bind,start,stop,close_sensor,poll_sensor,control,control_query};
