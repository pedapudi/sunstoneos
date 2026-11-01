/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FUSB302 register sequence follows drivers/usb/a733_typec.c and the pinned
 * onsemi data sheet. Linux sequence copyright 2016-2017 Google, Inc. */
#include "diagnostic.h"
static int io(struct vbus_diagnostic *s,unsigned reg,unsigned read,unsigned value)
{s->value=(uint8_t)value;return s->ops.io(s->context,reg,read,&s->value);}
static int setting(struct vbus_diagnostic *s,unsigned reg,unsigned value,unsigned mask)
{
    int r=io(s,reg,s->substep,value);if(r!=1)return r;
    if(!s->substep){s->substep=1;return 0;}s->substep=0;
    return (s->value&mask)==(value&mask)?1:-1;
}
void vbus_begin(struct vbus_diagnostic *s,const struct vbus_ops *ops,void *context,unsigned mode,uint64_t deadline)
{*s=(struct vbus_diagnostic){.ops=*ops,.context=context,.mode=mode,.deadline=deadline};}
void vbus_stop(struct vbus_diagnostic *s,unsigned failure)
{
    if(!s->failure)s->failure=failure;
    if(!s->off_at)s->off_at=s->ops.now(s->context);
    /* GPIO shutdown is independent of I2C completion or transaction health. */
    if(!s->ops.output(s->context,0)){s->failure=0x30;s->done=2;return;}
    s->enabled=0;
    s->state=7;s->step=0;s->substep=0;
}
int vbus_step(struct vbus_diagnostic *s)
{
    static const uint8_t setup[][3]={{0x0b,0x0f,0x0f},{2,0,255},{3,0xb0,0xf7},
        {6,0x24,0x2f},{7,0,0x73},{8,6,0xef},{9,0,0x5f},{0xa,255,255},{0xe,255,255},{0xf,1,1}};
    uint64_t now=s->ops.now(s->context);int r=0;
    if(s->done)return s->done==1?1:-1;
    if(s->last&&now>=s->last&&now-s->last>s->max_gap)s->max_gap=now-s->last;
    if(now<s->last||now>=s->deadline||(s->last&&now-s->last>2000)){
        if(s->state!=7)vbus_stop(s,0x10);
    }
    s->last=now;
    if(s->enabled&&!s->rose&&now-s->on_at>=275000)vbus_stop(s,0x16);
    if(s->enabled&&now-s->on_at>=(uint64_t)s->mode*1000000)vbus_stop(s,0);
    switch(s->state){
    case 0:
        if(!s->ops.output(s->context,0)){s->done=2;s->failure=0x30;break;}
        if(s->mode!=0&&s->mode!=1&&s->mode!=10){vbus_stop(s,0x11);break;}
        r=io(s,1,1,0);if(r==1){if(s->value<0x80||s->value>0x82)vbus_stop(s,0x12);else s->state=1;}break;
    case 1:
        r=io(s,0xc,s->step!=0,1);
        if(r==1){if(!s->step)s->step=1;else if(!(s->value&1)){s->state=2;s->step=0;}}break;
    case 2:
        r=setting(s,setup[s->step][0],setup[s->step][1],setup[s->step][2]);
        if(r==1&&++s->step==sizeof(setup)/sizeof(*setup)){s->state=3;s->step=0;}break;
    case 3: /* Select CC1 then CC2 at the source Rd/open boundary. */
        r=setting(s,2,s->step?0xc8:0xc4,255);
        if(r==1){s->state=4;s->substep=0;}break;
    case 4:
        r=setting(s,4,38,127);if(r==1){s->until=now+100;s->state=5;}break;
    case 5:
        if(now<s->until)break;
        r=io(s,0x40,1,0);
        if(r==1){
            if(!(s->value&0x20)){vbus_stop(s,0x13);break;}
            if(s->enabled&&!(s->value&0x80)&&s->rose){vbus_stop(s,0x14);break;}
            if(s->enabled&&(s->value&0x80)&&!s->rose){s->rose=1;s->rise_us=now-s->on_at;}
            ++s->cc_samples;
            if(!s->step){s->step=1;s->state=3;break;}
            s->step=0;s->state=6;
        }break;
    case 6:
        r=io(s,0x41,1,0);
        if(r==1){
            if(s->value&0x03){vbus_stop(s,0x15);break;}
            if(!s->enabled){
                s->state=8;break;
            }
            if(!s->rose&&now-s->on_at>=275000){vbus_stop(s,0x16);break;}
            if(now-s->on_at>=(uint64_t)s->mode*1000000){vbus_stop(s,0);break;}
            s->state=3;
        }break;
    case 8: /* Fresh VBUSOK-low observation immediately precedes enable. */
        r=io(s,0x40,1,0);
        if(r==1){
            if(s->value&0x80){vbus_stop(s,0x17);break;}
            if(!s->mode){vbus_stop(s,0);break;}
            if(now+((uint64_t)s->mode*1000000)+700000>=s->deadline){vbus_stop(s,0x18);break;}
            if(!s->ops.output(s->context,1)){vbus_stop(s,0x30);break;}
            s->on_at=s->ops.now(s->context);s->enabled=1;s->state=3;
        }break;
    case 7:
        if(now-s->off_at>=650000){s->failure=s->failure?s->failure:0x19;s->done=2;break;}
        r=io(s,0x40,1,0);
        if(r==1&&!(s->value&0x80)){s->fell=1;s->fall_us=now-s->off_at;s->done=1;}
        break;
    default:vbus_stop(s,0x1f);break;
    }
    if(r<0){if(s->state==7){s->done=2;if(!s->failure)s->failure=0x20;}else vbus_stop(s,0x20);}
    return s->done?(s->done==1?1:-1):0;
}
