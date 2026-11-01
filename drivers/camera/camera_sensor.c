/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "camera_sensor.h"
#include <string.h>

int camera_sensor_fault(struct camera_sensor *s)
{s->state=CAMERA_SENSOR_FAULT;return -1;}
int camera_sensor_time(struct camera_sensor *s)
{
    uint64_t now;
    if(s->state==CAMERA_SENSOR_FAULT || s->ops.now_us(s->context,&now)!=1 ||
       now<s->now || now>=s->deadline)return camera_sensor_fault(s);
    s->now=now;return 1;
}
int camera_sensor_mode(const struct camera_sensor_driver *d,unsigned i,unsigned f,unsigned v,struct camera_mode *m)
{return d && d->enumerate && i<d->mode_count && m?d->enumerate(i,f,v,m):-1;}
static int same_mode(const struct camera_mode *a,const struct camera_mode *b)
{
    return a->xclk_hz==b->xclk_hz && a->link_hz==b->link_hz && a->pixel_hz==b->pixel_hz &&
        a->width==b->width && a->height==b->height && a->htotal==b->htotal && (!a->vtotal || a->vtotal==b->vtotal) &&
        a->lanes==b->lanes && a->virtual_channel==b->virtual_channel && a->csi_data_type==b->csi_data_type &&
        a->bytes_per_pixel==b->bytes_per_pixel && a->size==b->size && a->format==b->format;
}
int camera_sensor_init(struct camera_sensor *s,const struct camera_sensor_bus_ops *ops,void *context,
    const struct camera_sensor_bus *bus,const struct camera_sensor_driver *const *drivers,size_t count,
    const struct camera_mode *mode,int bars)
{
    if(!s || s->initialized || !ops || !bus || !drivers || !count || count>16 || !mode ||
       !ops->now_us || !ops->acquire || !ops->line || !ops->read || !ops->write ||
       !ops->receiver || !ops->release || (bars!=0 && bars!=1) || bus->port>1 ||
       bus->parser!=bus->port || !bus->lanes || bus->lanes>4 || bus->virtual_channel>3 ||
       !bus->i2c_address || bus->i2c_address>0x7f || mode->xclk_hz!=bus->xclk_hz ||
       mode->lanes!=bus->lanes || mode->virtual_channel!=bus->virtual_channel ||
       !bus->power_hold_us || !bus->reset_hold_us || !bus->bus_hold_us)return -1;
    int supported=0;struct camera_mode negotiated=*mode;
    for(size_t i=0;i<count;i++){
        const struct camera_sensor_driver *d=drivers[i];struct camera_mode derived;
        if(!d || !d->mode_count || d->mode_count>256 || !d->enumerate || !d->bind || !d->start || !d->stop || !d->close || !d->poll || !d->control || !d->control_query)return -1;
        if(d->address==bus->i2c_address && (bus->power_hold_us<d->power_hold_us ||
           bus->reset_hold_us<d->reset_hold_us || bus->bus_hold_us<d->bus_hold_us))return -1;
        if(d->address==bus->i2c_address && camera_sensor_mode(d,mode->size,mode->format,mode->vtotal,&derived)==1 &&
           same_mode(mode,&derived)){supported=1;negotiated=derived;}
    }
    if(!supported)return -1;
    *s=(struct camera_sensor){.ops=*ops,.context=context,.bus=*bus,.drivers=drivers,
        .driver_count=count,.mode=negotiated,.test_pattern=(uint8_t)bars,.initialized=1};return 1;
}
int camera_sensor_open(struct camera_sensor *s,uint64_t d)
{
    if(!s || !s->initialized || s->state!=CAMERA_SENSOR_NEW || !d)return -1;
    s->deadline=d;s->state=CAMERA_SENSOR_ACQUIRE;return 1;
}
static int hold(struct camera_sensor *s,uint32_t us)
{
    if(camera_sensor_time(s)!=1 || UINT64_MAX-s->now<=us)return camera_sensor_fault(s);
    s->wait_until=s->now+us+1;return 0;
}
int camera_sensor_start(struct camera_sensor *s,uint64_t t,uint64_t d)
{return s && s->driver && s->state==CAMERA_SENSOR_READY?s->driver->start(s,t,d):-1;}
int camera_sensor_stop(struct camera_sensor *s,uint64_t d)
{return s && s->driver && s->state==CAMERA_SENSOR_STREAMING?s->driver->stop(s,d):-1;}
int camera_sensor_close(struct camera_sensor *s,uint64_t d)
{return s && s->driver && s->state==CAMERA_SENSOR_READY?s->driver->close(s,d):-1;}
int camera_sensor_control_set(struct camera_sensor *s,enum camera_sensor_control control,int value)
{
    if(!s || !s->initialized || s->state==CAMERA_SENSOR_FAULT || s->state==CAMERA_SENSOR_CLOSED)return -1;
    const struct camera_sensor_driver *driver=s->driver;
    if(!driver){
        if(s->state!=CAMERA_SENSOR_NEW)return -1;
        for(size_t i=0;i<s->driver_count;i++){
            struct camera_mode mode;
            if(s->drivers[i]->address==s->bus.i2c_address &&
               camera_sensor_mode(s->drivers[i],s->mode.size,s->mode.format,s->mode.vtotal,&mode)==1 &&
               same_mode(&s->mode,&mode)){driver=s->drivers[i];break;}
        }
    }
    return driver?driver->control(s,control,value):-1;
}
int camera_sensor_poll(struct camera_sensor *s)
{
    if(!s || !s->initialized || s->state==CAMERA_SENSOR_NEW || s->state==CAMERA_SENSOR_FAULT)return -1;
    if(s->driver)return s->driver->poll(s);
    if(camera_sensor_time(s)!=1)return -1;
    if(s->wait_until){if(s->now<s->wait_until)return 0;s->wait_until=0;}
    int r;uint64_t token=s->token;uint8_t value;
    switch(s->state){
    case CAMERA_SENSOR_ACQUIRE:
        r=s->ops.acquire(s->context,s->deadline,&token);
        if(s->token && s->token!=token)return camera_sensor_fault(s);
        s->token=token;
        if(r<0 || r>1 || (r==1 && !token) || camera_sensor_time(s)!=1)return camera_sensor_fault(s);
        if(!r)return 0;
        s->state=CAMERA_SENSOR_POWER_WAIT;return hold(s,s->bus.power_hold_us);
    case CAMERA_SENSOR_POWER_WAIT:s->state=CAMERA_SENSOR_PWDN;return 0;
    case CAMERA_SENSOR_PWDN:
        r=s->ops.line(s->context,s->token,CAMERA_SENSOR_PWDN_LINE,0,s->deadline);
        if(r!=1 || camera_sensor_time(s)!=1)return camera_sensor_fault(s);
        s->state=CAMERA_SENSOR_RESET_WAIT;return hold(s,s->bus.reset_hold_us);
    case CAMERA_SENSOR_RESET_WAIT:s->state=CAMERA_SENSOR_RESET;return 0;
    case CAMERA_SENSOR_RESET:
        if(s->bus.reset.present &&
           (s->ops.line(s->context,s->token,CAMERA_SENSOR_RESET_LINE,0,s->deadline)!=1 ||
            camera_sensor_time(s)!=1))return camera_sensor_fault(s);
        s->state=CAMERA_SENSOR_BUS_WAIT;return hold(s,s->bus.bus_hold_us);
    case CAMERA_SENSOR_BUS_WAIT:s->state=CAMERA_SENSOR_IDENTIFY;return 0;
    case CAMERA_SENSOR_IDENTIFY:
        while(s->candidate<s->driver_count && s->drivers[s->candidate]->address!=s->bus.i2c_address)++s->candidate;
        if(s->candidate==s->driver_count)return camera_sensor_fault(s);
        {const struct camera_sensor_driver *d=s->drivers[s->candidate];
        r=s->ops.read(s->context,s->token,s->bus.i2c_address,d->id_register[s->index],&value,s->deadline);
        if(r!=1 || camera_sensor_time(s)!=1)return camera_sensor_fault(s);
        if(value!=d->id_value[s->index]){++s->candidate;s->index=0;return 0;}
        if(++s->index!=2)return 0;
        struct camera_mode derived;
        if(camera_sensor_mode(d,s->mode.size,s->mode.format,s->mode.vtotal,&derived)!=1 ||
           !same_mode(&derived,&s->mode))return camera_sensor_fault(s);
        s->driver=d;s->index=0;
        if(d->bind(s)!=1)return camera_sensor_fault(s);
        return 0;}
    default:return camera_sensor_fault(s);
    }
}

int camera_sensor_control_query(const struct camera_sensor_driver *driver,enum camera_sensor_control control,
    struct camera_sensor_control_info *info)
{
    return driver && driver->control_query && info?driver->control_query(control,info):-1;
}
