/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fixed PCM subset adapted from Allwinner dw_avp.c and dw_fc.c, Linux
 * 2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f.
 * Copyright (C) 2016 Allwinner.
 * Licensed under GNU General Public License version 2, as is, without warranty. */
#include "a733_hdmi_audio.h"
int a733_hdmi_audio_edid(const uint8_t *e,size_t n)
{
    if(!e||n<128||n>512||n%128||n!=(size_t)(e[126]+1)*128)return 0;
    unsigned found=0;
    for(size_t b=0;b<n;b+=128){
        unsigned sum=0;for(unsigned j=0;j<128;++j)sum+=e[b+j];if(sum&255)return 0;
        if(!b||e[b]!=2)continue;
        unsigned end=e[b+2];if(end<4||end>127)return 0;
        /* CTA Basic Audio guarantees 2-channel LPCM, 32/44.1/48kHz,16-bit. */
        if(e[b+3]&0x40)found=1;
        for(unsigned p=4;p<end;){
            unsigned tag=e[b+p]>>5,len=e[b+p++]&31;
            if(len>end-p)return 0;
            if(tag==1){
                if(len%3)return 0;
                for(unsigned j=0;j<len;j+=3)
                    if((e[b+p+j]>>3)==1&&(e[b+p+j]&7)>=1&&
                       (e[b+p+j+1]&4)&&(e[b+p+j+2]&1))found=1;
            }
            p+=len;
        }
    }
    return found;
}
static int field(struct a733_hdmi_audio *a,unsigned off,unsigned mask,unsigned value)
{
    const struct a733_hdmi_step s={A733_HDMI_BASE+off,mask,value,1};
    return a733_hdmi_field(&a->hdmi->io,&s);
}
static int live(struct a733_hdmi_audio *a)
{return a&&a->self==a&&!a->failed&&a->hdmi&&a->hdmi->self==a->hdmi&&
    a->hdmi->state==A733_HDMI_ACTIVE&&a733_hdmi_tick(&a->hdmi->io)==1;}
static int fail(struct a733_hdmi_audio *a)
{if(a&&a->self==a)a->failed=1;return -1;}
int a733_hdmi_audio_init(struct a733_hdmi_audio *a,struct a733_hdmi *h)
{
    if(!a||a->self||!h||h->self!=h)return 0;
    a->self=a;a->hdmi=h;return 1;
}
int a733_hdmi_audio_prepare(struct a733_hdmi_audio *a)
{
    if(!live(a))return fail(a);if(a->ready)return 1;
    struct a733_hdmi_io *io=&a->hdmi->io;
    if(!a->stage){
        if(!a733_hdmi_audio_edid(a->hdmi->edid,a->hdmi->edid_size)||
           field(a,0x1063,0xf0,0xf0)!=1||field(a,0x3102,12,12)!=1||
           field(a,0x3104,16,16)!=1||a733_hdmi_write(io,A733_HDMI_BASE+0x103,1,0x1b)!=1)return fail(a);
        uint32_t old;
        if(a733_hdmi_read(io,A733_HDMI_BASE+0x3100,1,&old)!=1||
           a733_hdmi_write(io,A733_HDMI_BASE+0x3100,1,old|0x80)!=1)return fail(a);
        a->until=io->now+5;a->stage=1;return 0;
    }
    if(a->stage==1){
        if(io->now<a->until)return 0;
        /* MC software reset request is a command, not an RMW/readback fence. */
        uint32_t old;
        if(a733_hdmi_read(io,A733_HDMI_BASE+0x4002,1,&old)!=1||
           a733_hdmi_write(io,A733_HDMI_BASE+0x4002,1,old&~8u)!=1)return fail(a);
        a->stage=2;return 0;
    }
    static const struct {uint16_t off;uint8_t mask,value;} settings[]={
        {0x3205,0xff,0},{0x3202,0x0f,0},{0x3201,0xff,0x18},{0x3200,0xff,0},
        {0x3100,0x2f,0x21},{0x3101,0xff,16},{0x3103,7,4},{0x3206,7,4},
        {0x1063,1,0},{0x1065,0xff,0xee},{0x1066,0xff,0xff},
        {0x1067,0x31,1},{0x1068,0xff,0},{0x1069,0x7f,0},
        {0x106a,0xff,0x42},{0x106b,0xff,0x86},{0x106c,0xff,0x31},
        {0x106d,0xff,0x75},{0x106e,0x3f,2},{0x106f,0xff,0xd2},
        {0x1025,0x7f,0x10},{0x1026,0x37,0},{0x1027,0xff,0},{0x1028,0x1f,0},
        {0x4001,8,0}
    };
    if(a->stage==3){
        if(io->now<a->until)return 0;
        a->stage=4;
    }
    /* The source triggers hardware CTS calculation after all N bytes, then
     * waits 5 ms before sampler/packet programming. */
    if(a->stage==2&&a->index==4){
        if(field(a,0x3205,0xff,0)!=1)return fail(a);
        a->until=io->now+5000;a->stage=3;return 0;
    }
    if(a->index<sizeof(settings)/sizeof(settings[0])){
        const unsigned i=a->index;
        if(field(a,settings[i].off,settings[i].mask,settings[i].value)!=1)return fail(a);
        ++a->index;return 0;
    }
    a->ready=1;return 1;
}
int a733_hdmi_audio_mute(struct a733_hdmi_audio *a,unsigned mute)
{
    if(mute>1||!live(a)||!a->ready)return fail(a);
    return field(a,0x1063,0xf0,mute?0xf0:0)==1?1:fail(a);
}
