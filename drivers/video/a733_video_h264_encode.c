/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "a733_video_h264_encode.h"
#include "a733_range.h"
/* Format data and fixed controls verified against pinned A733
 * H264InitSPS/PPS/EncSliceHeader and h264EncFrame. See sources.json.
 * No opaque library is linked; hardware alone generates entropy/reconstruction. */
const uint8_t a733_h264_encode_sps_pps[22]={
 0,0,0,1,0x67,0x42,0,0x1f,0x89,0x89,0x40,0x50,0x1e,0x88,0,0,0,1,0x68,0xce,0x3c,0x80};
const uint8_t *a733_h264_encode_prefix(unsigned ordinal,size_t *bytes)
{
 static const uint8_t idr[]={0,0,0,1,0x65,0x88,0x80,4,0,0xf};
 static const uint8_t p[]={0,0,0,1,0x41,0x9a,0,0x20,4};
 if(!bytes||ordinal>1)return NULL;*bytes=ordinal?sizeof(p):sizeof(idr);return ordinal?p:idr;
}
static int fail(struct a733_h264_encode_run *s){s->state=A733_H264_ENCODE_FAULT;return -1;}
static int tick(struct a733_h264_encode_run *s)
{uint64_t n;if(s->ops.now_us(s->context,&n)!=1||!a733_deadline_admit(n,&s->last_now,s->deadline))return fail(s);return 1;}
static int rd(struct a733_h264_encode_run *s,uint32_t o,uint32_t *v)
{return s->ops.read(s->context,s->token,o,v,s->deadline)==1&&tick(s)==1?1:fail(s);}
static int wr(struct a733_h264_encode_run *s,uint32_t o,uint32_t v)
{return s->ops.write(s->context,s->token,o,v,s->deadline)==1&&tick(s)==1?1:fail(s);}
static int valid(const struct a733_h264_encode_job *j)
{
 static const size_t sizes[]={462848,462848,4194304,491520,491520,12288,12288,8192,8192,4096};
 if(!j||!j->input[0]||!j->input[1]||j->input[0]==j->input[1])return 0;
 for(unsigned i=0;i<A733_H264_ENCODE_BUFFERS;++i){
  uint64_t a=j->buffers[i].iova,n=j->buffers[i].bytes;
  if(!a||(a&4095)||n!=sizes[i]||a>UINT32_MAX-n)return 0;
  for(unsigned k=0;k<i;++k)if(a<j->buffers[k].iova+j->buffers[k].bytes&&j->buffers[k].iova<a+n)return 0;
 }
 return 1;
}
int a733_h264_encode_begin(struct a733_h264_encode_run *s,const struct a733_h264_encode_ops *o,
    void *v,const struct a733_h264_encode_job *j,uint64_t d)
{
 if(!s||s->state!=A733_H264_ENCODE_NEW||!o||!o->now_us||!o->acquire||!o->prepare||!o->prime||
  !o->read||!o->write||!o->copied||!d||!valid(j))return 0;
 *s=(struct a733_h264_encode_run){.ops=*o,.job=*j,.context=v,.deadline=d,.state=A733_H264_ENCODE_ACQUIRE};return tick(s)==1;
}
static void program(const struct a733_h264_encode_run *s,unsigned index,uint32_t *off,uint32_t *value)
{
 static const uint32_t pairs[][2]={
  {0xa00,0x50003c},
  {0xa04,0x280000},
  {0xa08,0x20001},
  {0xa14,0x28},
  {0xa2c,0x3c0050},
  {0xa30,0x0},
  {0xa34,0x0},
  {0xa38,0x10001000},
  {0xa70,0x0},
  {0xa74,0x0},
  {0xa78,0x12000000},
  {0xa7c,0x1204b000},
  {0xa80,0x1205dc00},
  {0xa84,0x0},
  {0xa88,0x0},
  {0xb04,0x0},
  {0xb08,0x41a},
  {0xb0c,0x0},
  {0xb10,0x8011200},
  {0xb14,0x3},
  {0xb18,0x0},
  {0xb28,0x0},
  {0xb2c,0x10001900},
  {0xb30,0x0},
  {0xb34,0x0},
  {0xb38,0x0},
  {0xb74,0x0},
  {0xc14,0x0},
  {0xc10,0x0},
  {0xc1c,0x0},
  {0xc18,0x0},
  {0xc20,0x0},
  {0xb40,0x14},
  {0xb48,0x0},
  {0xb4c,0x0},
  {0xb70,0xff000000},
  {0xb98,0x0},
  {0xbcc,0x0},
  {0xbd0,0x0},
  {0xbd4,0x0},
  {0xbd8,0x0},
  {0xbdc,0x0},
  {0xbe8,0x0},
  {0xbec,0x0},
  {0xbf0,0x0},
  {0xbf4,0x0},
  {0xb94,0x5},
  {0xcc4,0x0},
  {0xcf8,0x0},
  {0xcfc,0x0},
  {0xb3c,0x0},
  {0xb60,0x0},
  {0xb9c,0x106370},
  {0xba0,0x1053d0},
  {0xba4,0x1058d0},
  {0xba8,0x0},
  {0xbb0,0x1053d0},
  {0xbb4,0x1058d0},
  {0xbb8,0x1062d0},
  {0xbbc,0x1062d0},
  {0xbc0,0x106330},
  {0xbc4,0x106350},
  {0xbf8,0x0},
  {0xbfc,0x0},
  {0xb88,0x50},
  {0xb80,0x101140},
  {0xb84,0x10513f},
  {0xb8c,0x2000000},
 };
 *off=pairs[index][0];*value=pairs[index][1];
 uint32_t input=(uint32_t)s->job.buffers[s->ordinal].iova;
 uint32_t recon=(uint32_t)s->job.buffers[A733_H264_ENCODE_RECON0+s->ordinal].iova;
 switch(*off){
 case 0xa08:*value=s->ordinal?1:0x20001;break;
 case 0xa78:*value=input;break;case 0xa7c:*value=input+307200;break;case 0xa80:*value=input+384000;break;
 case 0xb04:*value=s->ordinal?0x410:0;break;case 0xb08:*value=s->ordinal?0x51a:0x41a;break;
 case 0xb48:*value=s->ordinal?0x180004:0;break;case 0xb4c:*value=s->ordinal?0x800068:0;break;
 case 0xb9c:*value=(uint32_t)(s->job.buffers[A733_H264_ENCODE_MISC].iova>>8);break;
 case 0xba0:*value=(uint32_t)(s->job.buffers[A733_H264_ENCODE_RECON0].iova>>8);break;
 case 0xba4:*value=(uint32_t)((s->job.buffers[A733_H264_ENCODE_RECON0].iova+327680)>>8);break;
 case 0xbb0:*value=recon>>8;break;case 0xbb4:*value=(recon+327680)>>8;break;
 case 0xbb8:*value=(uint32_t)(s->job.buffers[A733_H264_ENCODE_AUX0].iova>>8);break;
 case 0xbbc:*value=(uint32_t)(s->job.buffers[A733_H264_ENCODE_AUX0+s->ordinal].iova>>8);break;
 case 0xbc0:*value=(uint32_t)(s->job.buffers[A733_H264_ENCODE_LINE0].iova>>8);break;
 case 0xbc4:*value=(uint32_t)(s->job.buffers[A733_H264_ENCODE_LINE1].iova>>8);break;
 case 0xb88:*value=(uint32_t)((s->cursor+(s->ordinal?9:10))*8);break;
 case 0xb80:*value=(uint32_t)(s->job.buffers[A733_H264_ENCODE_RING].iova>>8);break;
 case 0xb84:*value=(uint32_t)((s->job.buffers[A733_H264_ENCODE_RING].iova+4194304-1)>>8);break;
 case 0xb8c:*value=4194304*8;break;
 default:break;
 }
}
int a733_h264_encode_next(struct a733_h264_encode_run *s)
{
 if(!s||s->state!=A733_H264_ENCODE_COPIED||s->ordinal)return 0;
 if(tick(s)!=1)return 0;
 size_t cursor=(s->end[0]+63u)&~(size_t)63u;
 /* Preserve source2MiB minimum-free guard, no ring wrap or rewind. */
 if(cursor>4194304-2097152)return fail(s);
 s->cursor=cursor;s->ordinal=1;s->index=0;s->state=A733_H264_ENCODE_PRIME;return 1;
}
int a733_h264_encode_poll(struct a733_h264_encode_run *s)
{
 if(!s||s->state==A733_H264_ENCODE_NEW||s->state==A733_H264_ENCODE_FAULT)return -1;
 if(s->state==A733_H264_ENCODE_COPIED)return 1;
 if(tick(s)!=1)return -1;
 int r;uint64_t old;uint32_t o,v;
 switch(s->state){
 case A733_H264_ENCODE_ACQUIRE:
  old=s->token;r=s->ops.acquire(s->context,&s->job,s->deadline,&s->token);
  if(tick(s)!=1||r<0||r>1||!s->token||(old&&old!=s->token))return fail(s);
  if(r==1)s->state=A733_H264_ENCODE_PREPARE;break;
 case A733_H264_ENCODE_PREPARE:
  r=s->ops.prepare(s->context,s->token,&s->job,s->deadline);if(tick(s)!=1||r<0||r>1)return fail(s);
  if(r==1)s->state=A733_H264_ENCODE_PRIME;break;
 case A733_H264_ENCODE_PRIME:
  r=s->ops.prime(s->context,s->token,s->ordinal,s->cursor,s->deadline);if(tick(s)!=1||r<0||r>1)return fail(s);
  if(r==1)s->state=A733_H264_ENCODE_RESET_READ;break;
 case A733_H264_ENCODE_RESET_READ:
  if(rd(s,0x804,&s->value)!=1)return -1;s->state=A733_H264_ENCODE_RESET_HIGH;break;
 case A733_H264_ENCODE_RESET_HIGH:
  if(wr(s,0x804,s->value|0x1000000)!=1)return -1;s->state=A733_H264_ENCODE_RESET_REREAD;break;
 case A733_H264_ENCODE_RESET_REREAD:
  if(rd(s,0x804,&s->value)!=1)return -1;s->state=A733_H264_ENCODE_RESET_LOW;break;
 case A733_H264_ENCODE_RESET_LOW:
  if(wr(s,0x804,s->value&~0x1000000u)!=1)return -1;s->state=A733_H264_ENCODE_ENABLE_READ;break;
 case A733_H264_ENCODE_ENABLE_READ:
  if(rd(s,0x800,&s->value)!=1)return -1;s->state=A733_H264_ENCODE_ENABLE6;break;
 case A733_H264_ENCODE_ENABLE6:
  if(wr(s,0x800,s->value|0x40)!=1)return -1;s->state=A733_H264_ENCODE_ENABLE_REREAD;break;
 case A733_H264_ENCODE_ENABLE_REREAD:
  if(rd(s,0x800,&s->value)!=1)return -1;s->state=A733_H264_ENCODE_ENABLE7;break;
 case A733_H264_ENCODE_ENABLE7:
  if(wr(s,0x800,s->value|0x80)!=1)return -1;s->state=A733_H264_ENCODE_ENABLE_CHECK;break;
 case A733_H264_ENCODE_ENABLE_CHECK:
  if(rd(s,0x800,&v)!=1)return -1;if((v&0xc0)!=0xc0)return fail(s);s->state=A733_H264_ENCODE_PROGRAM;break;
 case A733_H264_ENCODE_PROGRAM:
  program(s,s->index,&o,&v);if(wr(s,o,v)!=1)return -1;
  if(++s->index==68)s->state=A733_H264_ENCODE_STALE;break;
 case A733_H264_ENCODE_STALE:
  if(rd(s,0xb1c,&s->status)!=1)return -1;s->state=A733_H264_ENCODE_CLEAR;break;
 case A733_H264_ENCODE_CLEAR:
  if(wr(s,0xb1c,s->status&15)!=1)return -1;s->state=A733_H264_ENCODE_CLEAN;break;
 case A733_H264_ENCODE_CLEAN:
  if(rd(s,0xb1c,&v)!=1)return -1;if(v&15)return fail(s);
  s->state=s->ordinal?A733_H264_ENCODE_HEADER_READY:A733_H264_ENCODE_TRIGGER;break;
 case A733_H264_ENCODE_HEADER_READY:
  if(rd(s,0xb1c,&v)!=1)return -1;if(v&15)return fail(s);
  if(v&0x200)s->state=A733_H264_ENCODE_HEADER_DATA;break;
 case A733_H264_ENCODE_HEADER_DATA:
  if(wr(s,0xb20,0xf)!=1)return -1;s->state=A733_H264_ENCODE_HEADER_COMMAND;break;
 case A733_H264_ENCODE_HEADER_COMMAND:
  if(wr(s,0xb18,0x601)!=1)return -1;s->state=A733_H264_ENCODE_TRIGGER;break;
 case A733_H264_ENCODE_TRIGGER:
  if(wr(s,0xb18,8)!=1)return -1;s->state=A733_H264_ENCODE_WAIT;break;
 case A733_H264_ENCODE_WAIT:
  if(rd(s,0xb1c,&s->status)!=1)return -1;if(!(s->status&15))break;
  if((s->status&15)!=1)return fail(s);s->state=A733_H264_ENCODE_COUNT;break;
 case A733_H264_ENCODE_COUNT:
  if(rd(s,0xb90,&v)!=1)return -1;
  if(v<=(s->cursor+(s->ordinal?9:10))*8+(s->ordinal?6:0)||v%8||v/8>4194304||
     v/8-s->cursor>1048576-(s->ordinal?0:22))return fail(s);
  s->end[s->ordinal]=v/8;s->state=A733_H264_ENCODE_COPY;break;
 case A733_H264_ENCODE_COPY:
  r=s->ops.copied(s->context,s->token,s->ordinal,s->cursor,s->end[s->ordinal],s->deadline);
  if(tick(s)!=1||r<0||r>1)return fail(s);if(r==1){s->state=A733_H264_ENCODE_COPIED;return 1;}break;
 default:return fail(s);
 }
 return 0;
}
