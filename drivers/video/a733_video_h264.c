/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2013 Jens Kuske <jenskuske@gmail.com>
 * Copyright (c) 2018 Bootlin */
/* Fixed operation adapted from Linux Cedrus70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56
 * cedrus_h264.c (Jens Kuske/Bootlin), and checked against pinned A733 instructions.
 * Source/license identities are recorded in apps/h264/sources.json. */
#include "a733_video_h264.h"
#include "a733_storage_digest.h"
#include <string.h>
static const uint8_t slice_hash[32]={0x70,0x21,0x22,0x42,0x78,0x37,0x2c,0xeb,
    0x05,0xbe,0x72,0x79,0x65,0x79,0xc4,0xb4,0x12,0xe6,0xf7,0x75,0x82,0x80,0xd8,0x55,
    0xa5,0xd0,0x7f,0x12,0x25,0x64,0x9d,0x69};
static int fail(struct a733_h264_run *r){r->state=A733_H264_FAULT;return -1;}
static int live(struct a733_h264_run *r)
{
    uint64_t now;
    if(r->ops.now_us(r->context,&now)!=1||now<r->last_now||now>=r->deadline)return fail(r);
    r->last_now=now;return 1;
}
static int read_reg(struct a733_h264_run *r,uint32_t off,uint32_t *out)
{return live(r)==1&&r->ops.read(r->context,r->token,off,out,r->deadline)==1&&live(r)==1?1:fail(r);}
static int write_reg(struct a733_h264_run *r,uint32_t off,uint32_t value)
{return live(r)==1&&r->ops.write(r->context,r->token,off,value,r->deadline)==1&&live(r)==1?1:fail(r);}
int a733_h264_begin(struct a733_h264_run *r,const struct a733_h264_ops *o,void *v,
                    const struct a733_h264_job *j,uint64_t d)
{
    static const uint64_t capacity[]={4096,4096,4096,0x21000,4096,0xc000};
    if(!r||r->state!=A733_H264_NEW||!o||!j||!j->input||!d||!o->now_us||
       !o->acquire||!o->prepare||!o->read||!o->write||!o->copied)return 0;
    for(unsigned i=0;i<A733_H264_BUFFERS;++i){
        const struct a733_h264_span *s=&j->buffers[i];
        if(!s->iova||(s->iova&4095)||s->bytes!=capacity[i]||
           s->iova>UINT64_C(0x100000000)-s->bytes||
           (i==A733_H264_NEIGHBOR&&(s->iova&16383)))return 0;
        for(unsigned k=0;k<i;++k)if(s->iova<j->buffers[k].iova+j->buffers[k].bytes&&
            j->buffers[k].iova<s->iova+s->bytes)return 0;
    }
    *r=(struct a733_h264_run){.ops=*o,.job=*j,.context=v,.deadline=d,.state=A733_H264_ACQUIRE};
    return 1;
}
/* Program provenance: SPS/PPS/SHS/QP/VLD and frameSRAM match the preserved
 * libawh264 instruction trace. Initial full frame-list clearing, primary-only
 * buffers and offset0/INIT/FLUSH come from Cedrus. SDROT_CTRL0 disables scaling;
 * no secondary-output addresses or scaling triggers are programmed.
 */
static int program(struct a733_h264_run *r)
{
    const struct a733_h264_span *s=r->job.buffers;
    uint32_t off,value,n=r->index;
    static const uint32_t offsets[]={0xec,0xc4,0xc8,0x250,0x254,0x240,0x22c,0x260,
        0x24c,0x200,0x204,0x208,0x20c,0x21c,0x220,0x234,0x238,0x23c,0x230,0x2e0};
    uint32_t values[]={r->format,512,0x00200040,(uint32_t)s[A733_H264_PICTURE_INFO].iova,
        (uint32_t)s[A733_H264_NEIGHBOR].iova,0,0,0,0,0x000d0201,0,0x1220,0x100,
        0x0100001a,0x407,0,A733_H264_SLICE_BYTES*8,
        (uint32_t)(s[A733_H264_INPUT].iova+s[A733_H264_INPUT].bytes-1),
        ((uint32_t)s[A733_H264_INPUT].iova&0x0ffffff0)|
        ((uint32_t)s[A733_H264_INPUT].iova>>28)|0x70000000,0x400};
    if(n<sizeof(offsets)/sizeof(offsets[0])){off=offsets[n];value=values[n];}
    else{
        n-=sizeof(offsets)/sizeof(offsets[0]);
        if(n>=18*8)return 0;
        off=0x2e4;value=0;
        if(n==3)value=(uint32_t)s[A733_H264_Y].iova;
        if(n==4)value=(uint32_t)s[A733_H264_C].iova;
        if(n==5)value=(uint32_t)s[A733_H264_MV].iova;
        if(n==6)value=(uint32_t)s[A733_H264_MV].iova+1024;
    }
    if(write_reg(r,off,value)!=1)return -1;
    ++r->index;return 1;
}
int a733_h264_poll(struct a733_h264_run *r)
{
    if(!r||r->state==A733_H264_NEW)return -1;
    if(r->state==A733_H264_FAULT)return -1;
    if(r->state==A733_H264_COPIED)return 1;
    if(live(r)!=1)return -1;
    int result;uint32_t value;
    switch(r->state){
    case A733_H264_ACQUIRE:{
        uint64_t previous=r->token;
        result=r->ops.acquire(r->context,&r->job,r->deadline,&r->token);
        if(!r->token||(previous&&previous!=r->token)||(result!=0&&result!=1)||live(r)!=1)return fail(r);
        if(result)r->state=A733_H264_HASH;
        break;}
    case A733_H264_HASH:{
        uint8_t hash[32];storage_digest_bytes(r->job.input,A733_H264_SLICE_BYTES,hash);
        if(memcmp(hash,slice_hash,32)||live(r)!=1)return fail(r);
        r->state=A733_H264_IDENTIFY;r->index=0;break;}
    case A733_H264_IDENTIFY:
        if(read_reg(r,r->index==0?0xf0:r->index==1?0xe0:0xe4,&value)!=1)return -1;
        if(!r->index&&value&&value!=0x16670040)return fail(r);
        if(r->index==1)r->identity_high=value&0xfffff;
        if(++r->index==3){
            r->version=((uint64_t)r->identity_high<<32)|(value&0xfffff);
            if(r->version!=UINT64_C(0x3332100021321)&&r->version!=UINT64_C(0x3331000021320)&&
               r->version!=UINT64_C(0x3052000012011))return fail(r);
            r->state=A733_H264_PREPARE;r->index=0;
        }break;
    case A733_H264_PREPARE:
        result=r->ops.prepare(r->context,r->token,&r->job,r->deadline);
        if((result!=0&&result!=1)||live(r)!=1)return fail(r);
        if(result)r->state=A733_H264_CHECK;
        break;
    case A733_H264_CHECK:{
        static const uint32_t offsets[]={0,0x1c,0xb0,0xec,0x228,0x5c,0x804};
        if(read_reg(r,offsets[r->index],&value)!=1)return -1;
        if((r->index==0&&(value&0x2c00101f)!=1)||
           (r->index==1&&(value&0x100))||(r->index==2&&(value&0x80000000))||
           (r->index==4&&(value&(A733_H264_BUSY_MASK|15)))||
           (r->index==5&&(value&0x10))||(r->index==6&&(value&0x11)))return fail(r);
        if(r->index==3)r->format=(value&~0x77u)|0x20;
        if(++r->index==7){r->index=0;r->state=A733_H264_PROGRAM;}break;}
    case A733_H264_PROGRAM:
        result=program(r);if(result<0)return -1;
        if(!result)r->state=A733_H264_INIT;
        break;
    case A733_H264_INIT:
        if(write_reg(r,0x224,7)!=1)return -1;
        r->state=A733_H264_FLUSH;break;
    case A733_H264_FLUSH:
        /* The independently constructed header is24bits including NAL65.
         * Cedrus flush includes that NAL header, with EP detection enabled. */
        if(write_reg(r,0x224,(24u<<8)|3)!=1)return -1;
        r->state=A733_H264_HEADER_WAIT;break;
    case A733_H264_HEADER_WAIT:
        if(read_reg(r,0x228,&value)!=1)return -1;
        if(value&0xe)return fail(r);
        if(!(value&0x100))r->state=A733_H264_TRIGGER;
        break;
    case A733_H264_TRIGGER:
        if(write_reg(r,0x224,8)!=1)return -1;
        r->state=A733_H264_WAIT;break;
    case A733_H264_WAIT:
        if(read_reg(r,0x228,&r->status)!=1)return -1;
        if(r->status&0xe)return fail(r);
        if((r->status&1)&&!(r->status&A733_H264_BUSY_MASK))r->state=A733_H264_DISABLE;
        break;
    case A733_H264_DISABLE:
        if(write_reg(r,0x220,0x400)!=1)return -1;
        r->state=A733_H264_ACK;break;
    case A733_H264_ACK:
        if(write_reg(r,0x228,r->status&7)!=1)return -1;
        r->state=A733_H264_COPY;break;
    case A733_H264_COPY:
        result=r->ops.copied(r->context,r->token,r->deadline);
        if((result!=0&&result!=1)||live(r)!=1)return fail(r);
        if(result){r->state=A733_H264_COPIED;return 1;}
        break;
    default:return fail(r);
    }
    return 0;
}
