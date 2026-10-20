/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_video_jpeg_encode.h"
#include <string.h>
/* Register values follow the pinned A733 JpegEncEncode/ENCPP instruction trace,
 * not the similarly named decoder block. All offsets are relative to ENC VE
 * resource0x01c10000: TOP800, ENCPP a00, JPEG b00. See sources.json. */
static int fail(struct a733_jpeg_encode_run *s)
{s->state=A733_JPEG_ENCODE_FAULT;return -1;}
static int tick(struct a733_jpeg_encode_run *s)
{uint64_t n;if(s->ops.now_us(s->context,&n)!=1||n<s->last_now||n>=s->deadline)return fail(s);s->last_now=n;return 1;}
static int rd(struct a733_jpeg_encode_run *s,uint32_t o,uint32_t *v)
{return s->ops.read(s->context,s->token,o,v,s->deadline)==1&&tick(s)==1?1:fail(s);}
static int wr(struct a733_jpeg_encode_run *s,uint32_t o,uint32_t v)
{return s->ops.write(s->context,s->token,o,v,s->deadline)==1&&tick(s)==1?1:fail(s);}
static int valid(const struct a733_jpeg_encode_job *j)
{
    if(!j||!j->input||j->input_bytes!=A733_JPEG_ENCODE_RAW_BYTES||
       j->input_capacity!=A733_JPEG_ENCODE_DMA_INPUT_BYTES||j->output_capacity!=A733_JPEG_ENCODE_CAPACITY||
       !j->input_iova||!j->output_iova||(j->input_iova&4095)||(j->output_iova&4095)||
       j->input_iova>UINT32_MAX-j->input_capacity||j->output_iova>UINT32_MAX-j->output_capacity)return 0;
    return j->input_iova+j->input_capacity<=j->output_iova||j->output_iova+j->output_capacity<=j->input_iova;
}
int a733_jpeg_encode_begin(struct a733_jpeg_encode_run *s,const struct a733_jpeg_encode_ops *o,
    void *v,const struct a733_jpeg_encode_job *j,uint64_t d)
{
    if(!s||s->state!=A733_JPEG_ENCODE_NEW||!o||!o->now_us||!o->acquire||!o->prepare||
       !o->read||!o->write||!o->copied||!valid(j)||!d)return 0;
    *s=(struct a733_jpeg_encode_run){.job=*j,.ops=*o,.context=v,.deadline=d,.state=A733_JPEG_ENCODE_ACQUIRE};
    return tick(s)==1;
}
static void programmed(const struct a733_jpeg_encode_run *s,unsigned i,uint32_t *o,uint32_t *v)
{
    static const uint32_t pairs[][2]={
      {0xa00,0x0050003c},{0xa04,0x00280000},{0xa08,0x00020001},{0xa14,0x28},
      {0xa2c,0x003c0050},{0xa30,0},{0xa34,0},{0xa38,0x10001000},
      {0xa70,0},{0xa74,0},{0xa78,0},{0xa7c,0},{0xa80,0},{0xa84,0},{0xa88,0},
      {0xb18,0x10000},{0xb14,0xf},{0xb88,A733_JPEG_ENCODE_HEADER_BYTES*8},
      {0xb80,0},{0xb84,0},{0xb8c,A733_JPEG_ENCODE_CAPACITY*8},
      {0xb04,0xc0720080},{0xb08,0xc0000000},{0xbe0,0}};
    *o=pairs[i][0];*v=pairs[i][1];
    if(*o==0xa78)*v=(uint32_t)s->job.input_iova;
    if(*o==0xa7c)*v=(uint32_t)s->job.input_iova+307200;
    if(*o==0xa80)*v=(uint32_t)s->job.input_iova+384000;
    if(*o==0xb80)*v=(uint32_t)(s->job.output_iova>>8);
    if(*o==0xb84)*v=(uint32_t)((s->job.output_iova+s->job.output_capacity-1)>>8);
}
int a733_jpeg_encode_poll(struct a733_jpeg_encode_run *s)
{
    if(!s||s->state==A733_JPEG_ENCODE_NEW||s->state==A733_JPEG_ENCODE_FAULT)return -1;
    if(s->state==A733_JPEG_ENCODE_COPIED)return 1;
    if(tick(s)!=1)return -1;
    int r;uint32_t o,v;uint64_t old;
    switch(s->state){
    case A733_JPEG_ENCODE_ACQUIRE:
        old=s->token;r=s->ops.acquire(s->context,&s->job,s->deadline,&s->token);
        if(tick(s)!=1||r<0||(r!=0&&r!=1)||!s->token||(old&&old!=s->token))return fail(s);
        if(r==1)s->state=A733_JPEG_ENCODE_PREPARE;break;
    case A733_JPEG_ENCODE_PREPARE:
        r=s->ops.prepare(s->context,s->token,&s->job,s->deadline);
        if(tick(s)!=1||r<0||(r!=0&&r!=1))return fail(s);
        if(r==1)s->state=A733_JPEG_ENCODE_RESET_READ;break;
    case A733_JPEG_ENCODE_RESET_READ:
        if(rd(s,0x804,&s->value)!=1)return -1;s->state=A733_JPEG_ENCODE_RESET_HIGH;break;
    case A733_JPEG_ENCODE_RESET_HIGH:
        if(wr(s,0x804,s->value|0x1000000)!=1)return -1;s->state=A733_JPEG_ENCODE_RESET_REREAD;break;
    case A733_JPEG_ENCODE_RESET_REREAD:
        if(rd(s,0x804,&s->value)!=1)return -1;s->state=A733_JPEG_ENCODE_RESET_LOW;break;
    case A733_JPEG_ENCODE_RESET_LOW:
        if(wr(s,0x804,s->value&~0x1000000u)!=1)return -1;s->state=A733_JPEG_ENCODE_ENABLE_READ;break;
    case A733_JPEG_ENCODE_ENABLE_READ:
        if(rd(s,0x800,&s->value)!=1)return -1;s->state=A733_JPEG_ENCODE_ENABLE6;break;
    case A733_JPEG_ENCODE_ENABLE6:
        if(wr(s,0x800,s->value|0x40)!=1)return -1;s->state=A733_JPEG_ENCODE_ENABLE_REREAD;break;
    case A733_JPEG_ENCODE_ENABLE_REREAD:
        if(rd(s,0x800,&s->value)!=1)return -1;s->state=A733_JPEG_ENCODE_ENABLE7;break;
    case A733_JPEG_ENCODE_ENABLE7:
        if(wr(s,0x800,s->value|0x80)!=1)return -1;s->state=A733_JPEG_ENCODE_ENABLE_CHECK;break;
    case A733_JPEG_ENCODE_ENABLE_CHECK:
        if(rd(s,0x800,&v)!=1)return -1;if((v&0xc0)!=0xc0)return fail(s);s->state=A733_JPEG_ENCODE_PROGRAM;break;
    case A733_JPEG_ENCODE_PROGRAM:
        programmed(s,s->index,&o,&v);if(wr(s,o,v)!=1)return -1;
        if(++s->index==24){s->index=0;s->state=A733_JPEG_ENCODE_QUANT;}break;
    case A733_JPEG_ENCODE_QUANT:
        if(wr(s,0xbe4,a733_jpeg_encode_quant[s->index])!=1)return -1;
        if(++s->index==128)s->state=A733_JPEG_ENCODE_STALE;break;
    case A733_JPEG_ENCODE_STALE:
        if(rd(s,0xb1c,&s->status)!=1)return -1;s->state=A733_JPEG_ENCODE_CLEAR;break;
    case A733_JPEG_ENCODE_CLEAR:
        if(wr(s,0xb1c,s->status&15)!=1)return -1;s->state=A733_JPEG_ENCODE_CLEAN;break;
    case A733_JPEG_ENCODE_CLEAN:
        if(rd(s,0xb1c,&v)!=1)return -1;if(v&15)return fail(s);s->state=A733_JPEG_ENCODE_TRIGGER;break;
    case A733_JPEG_ENCODE_TRIGGER:
        if(wr(s,0xb18,0x10008)!=1)return -1;s->state=A733_JPEG_ENCODE_WAIT;break;
    case A733_JPEG_ENCODE_WAIT:
        if(rd(s,0xb1c,&s->status)!=1)return -1;if(!(s->status&15))break;
        if((s->status&15)!=1)return fail(s);s->state=A733_JPEG_ENCODE_COUNT;break;
    case A733_JPEG_ENCODE_COUNT:
        if(rd(s,0xb90,&v)!=1)return -1;
        if(v<=A733_JPEG_ENCODE_HEADER_BYTES*8||v>(A733_JPEG_ENCODE_CAPACITY-2)*8||v%8)return fail(s);
        s->jpeg_bytes=v/8+2;s->state=A733_JPEG_ENCODE_COPY;break;
    case A733_JPEG_ENCODE_COPY:
        r=s->ops.copied(s->context,s->token,s->jpeg_bytes,s->deadline);
        if(tick(s)!=1||r<0||(r!=0&&r!=1))return fail(s);
        if(r==1){s->state=A733_JPEG_ENCODE_COPIED;return 1;}break;
    default:return fail(s);
    }
    return 0;
}
