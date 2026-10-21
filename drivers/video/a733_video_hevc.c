/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Adapted from Linux Cedrus cedrus_h265.c, pinned in apps/hevc/sources.json.
 * Copyright (C) 2013 Jens Kuske <jenskuske@gmail.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 */
#include "a733_video_hevc.h"
#include <string.h>
static int fail(struct a733_hevc_run *r){r->state=HEVC_FAILED;return -1;}
static int write_reg(struct a733_hevc_run *r,uint32_t off,uint32_t value)
{return r->ops.write(r->context,r->token,off,value,r->deadline)==1?1:fail(r);}
static int read_reg(struct a733_hevc_run *r,uint32_t off,uint32_t *value)
{return r->ops.read(r->context,r->token,off,value,r->deadline)==1?1:fail(r);}
static const uint32_t required[HEVC_BUFFERS]={A733_HEVC_INPUT_SLOT,A733_HEVC_INPUT_SLOT,
 A733_HEVC_OUTPUT_SLOT,A733_HEVC_OUTPUT_SLOT,A733_HEVC_MV_SLOT,A733_HEVC_MV_SLOT,
 A733_HEVC_NEIGHBOR_SLOT,A733_HEVC_ENTRY_SLOT};
int a733_hevc_begin(struct a733_hevc_run *r,const struct a733_hevc_ops *ops,void *ctx,
 const struct a733_hevc_job *job,uint64_t deadline)
{
 if(!r||!ops||!ops->now_us||!ops->acquire||!ops->prepare||!ops->read||!ops->write||!ops->copied||
 !job||!job->input||!job->input_bytes||job->input_bytes>524288||!deadline)return 0;
 for(unsigned i=0;i<HEVC_BUFFERS;++i){uint64_t a=job->buffers[i].iova,n=job->buffers[i].bytes;
  if(!a||a%4096||n!=required[i]||a>UINT64_C(0x100000000)-n)return 0;
  for(unsigned j=0;j<i;++j)if(a<job->buffers[j].iova+job->buffers[j].bytes&&job->buffers[j].iova<a+n)return 0;
 }
 for(unsigned i=0;i<2;++i){const struct a733_hevc_picture *p=&job->plan.pictures[i];
  if(p->offset>job->input_bytes||!p->bytes||p->bytes>262144||p->bytes>job->input_bytes-p->offset||
   !p->header_bits||p->header_bits>=p->bytes*8||p->type!=(i?1u:2u)||
   (i?(!p->poc||p->poc>32767||p->nal_type!=1):(p->poc||(p->nal_type!=19&&p->nal_type!=20))))return 0;
 }
 if(job->plan.pictures[0].offset+job->plan.pictures[0].bytes>job->plan.pictures[1].offset)return 0;
 memset(r,0,sizeof(*r));r->job=*job;r->ops=*ops;r->context=ctx;r->deadline=deadline;r->state=HEVC_ACQUIRE;return 1;
}
/* Cedrus cedrus_h265_setup order: bitstream window from the slot start, CTB
 * and tile origin, CTB count, status clear, INIT_SWDEC, then FLUSH_BITS over
 * the slice header in at most 32-bit steps, each waiting for VLD idle. The
 * end address is the slot's last byte, as in the effective A733 library. */
static int initial(struct a733_hevc_run *r)
{
 unsigned pic=r->picture;uint64_t input=r->job.buffers[pic].iova;
 uint32_t off,value;
 switch(r->index++){
 case 0:off=0x544;value=0;break;
 case 1:off=0x548;value=r->job.plan.pictures[pic].bytes*8;break;
 case 2:off=0x540;value=UINT32_C(0x70000000)|(uint32_t)(input>>8);break;
 case 3:off=0x54c;value=(uint32_t)((input+262144-1)>>8);break;
 case 4:off=0x52c;value=0;break;
 case 5:off=0x568;value=0;break;
 case 6:off=0x56c;value=0;break;
 case 7:off=0x53c;value=0;break;
 case 8:off=0x538;value=7;break;
 case 9:off=0x534;value=7;r->state=HEVC_FLUSH;r->index=0;break;
 default:return fail(r);
 }
 return write_reg(r,off,value)<0?-1:0;
}
/* Parameter, control and linear NV12 output registers in the order and with
 * the fixed values of the effective A733 libawh265 trace for the IDR picture
 * (docs/hevc-native.md). Frame records occupy SRAM 0x400 + 0x20 * slot with
 * POC twice, MV address twice, luma and chroma, all addresses shifted by 8;
 * slot n is picture n and is also the output index. The P picture's list0
 * holds slot 0. Scaling, rotation and secondary output stay disabled. */
static int program(struct a733_hevc_run *r)
{
 const struct a733_hevc_picture *p=&r->job.plan.pictures[r->picture];
 uint32_t off,value;unsigned index=r->index++;
 static const uint32_t offsets[]={0x500,0x504,0x508,0x50c,0x510,0x514,0x518,0x520,0x524,0x528,
  0x530,0xe8,0xec,0xc4,0xc8,0xcc,0x550,0x554,0x558,0x580,0x564,0x560};
 if(index<sizeof(offsets)/sizeof(offsets[0])){
  uint32_t values[]={p->nal_type|64,p->sps,640|(480u<<16),p->pcm,p->pps0,p->pps1,UINT32_C(0x40000000),
   p->slice0,p->slice1,p->slice2,UINT32_C(0x80000007),UINT32_C(0x40000000),0x44,76800,640|(320u<<16),0,
   0,0,0,0,(uint32_t)(r->job.buffers[HEVC_ENTRY].iova>>8),(uint32_t)(r->job.buffers[HEVC_NEIGHBOR].iova>>8)};
  off=offsets[index];value=values[index];
 }else{
  index-=(unsigned)(sizeof(offsets)/sizeof(offsets[0]));
  unsigned frames=r->picture+1;
  if(index<frames*7){unsigned slot=index/7,word=index%7;
   if(!word){off=0x5e0;value=0x400+slot*0x20;}
   else{uint32_t poc=r->job.plan.pictures[slot].poc;uint64_t output=r->job.buffers[HEVC_OUTPUT0+slot].iova,mv=r->job.buffers[HEVC_MV0+slot].iova;
    uint32_t data[]={poc,poc,(uint32_t)(mv>>8),(uint32_t)(mv>>8),(uint32_t)(output>>8),(uint32_t)((output+A733_HEVC_CHROMA_OFFSET)>>8)};
    off=0x5e4;value=data[word-1];}
  }else{
   index-=frames*7;
   if(index==0){off=0x55c;value=r->picture;}
   else if(r->picture&&index==1){off=0x5e0;value=0xc00;}
   else if(r->picture&&index==2){off=0x5e4;value=0;}
   else{r->state=HEVC_TRIGGER;return 0;}
  }
 }
 return write_reg(r,off,value)<0?-1:0;
}
int a733_hevc_poll(struct a733_hevc_run *r)
{
 if(!r||r->state==HEVC_FAILED)return -1;
 if(r->state==HEVC_COPIED)return 1;
 uint64_t now;if(r->ops.now_us(r->context,&now)!=1||now<r->last_now||now>=r->deadline)return fail(r);r->last_now=now;
 uint32_t value;int result;
 switch(r->state){
 case HEVC_ACQUIRE:{uint64_t token=r->token;result=r->ops.acquire(r->context,&r->job,r->deadline,&token);
  if(r->token&&token!=r->token)return fail(r);if(token)r->token=token;
  if(result<0||(result!=0&&result!=1))return fail(r);if(result==1){if(!r->token)return fail(r);r->state=HEVC_IDENTIFY;}return 0;}
 /* Only the compound identity of the effective A733 trace is admitted. */
 case HEVC_IDENTIFY:
  if(read_reg(r,r->index?0xe4:0xe0,&value)<0)return -1;
  if(!r->index){r->identity=value;r->index=1;return 0;}
  if((r->identity&0xfffff)!=0x33310||(value&0xfffff)!=0x21320)return fail(r);r->index=0;r->state=HEVC_PREPARE;return 0;
 case HEVC_PREPARE:result=r->ops.prepare(r->context,r->token,&r->job,r->deadline);
  if(result<0||(result!=0&&result!=1))return fail(r);if(result==1)r->state=HEVC_CHECK;return 0;
 case HEVC_CHECK:if(read_reg(r,0x538,&value)<0)return -1;
  if(value&(A733_HEVC_BUSY_MASK|14))return fail(r);r->state=HEVC_INIT;r->index=0;return 0;
 case HEVC_INIT:return initial(r);
 case HEVC_FLUSH:{unsigned bits=r->job.plan.pictures[r->picture].header_bits-r->flushed;
  if(!bits){r->state=HEVC_PROGRAM;r->index=0;return 0;}if(bits>32)bits=32;r->flush_bits=bits;
  if(write_reg(r,0x534,3|(bits<<8))<0)return -1;r->state=HEVC_FLUSH_WAIT;return 0;}
 case HEVC_FLUSH_WAIT:if(read_reg(r,0x538,&value)<0)return -1;if(value&14)return fail(r);
  if(value&256)return 0;r->flushed+=r->flush_bits;r->state=HEVC_FLUSH;return 0;
 case HEVC_PROGRAM:return program(r);
 case HEVC_TRIGGER:if(write_reg(r,0x534,8)<0)return -1;r->state=HEVC_WAIT;return 0;
 case HEVC_WAIT:if(read_reg(r,0x538,&value)<0)return -1;if(value&14)return fail(r);
  if(!(value&1)||(value&A733_HEVC_BUSY_MASK))return 0;r->status=value;r->state=HEVC_COUNT;return 0;
 case HEVC_COUNT:if(read_reg(r,0x53c,&value)<0)return -1;if(value!=80)return fail(r);r->state=HEVC_DISABLE;return 0;
 case HEVC_DISABLE:if(write_reg(r,0x530,UINT32_C(0x80000000))<0)return -1;r->state=HEVC_ACK_STATUS;return 0;
 case HEVC_ACK_STATUS:if(write_reg(r,0x538,r->status&7)<0)return -1;r->state=HEVC_COPY;return 0;
 case HEVC_COPY:result=r->ops.copied(r->context,r->token,r->picture,r->deadline);
  if(result<0||(result!=0&&result!=1))return fail(r);if(result==1){if(r->ops.now_us(r->context,&now)!=1||now<r->last_now||now>=r->deadline)return fail(r);r->last_now=now;r->state=HEVC_COPIED;return 1;}return 0;
 case HEVC_COPIED:return 1;
 default:return fail(r);
 }
}
int a733_hevc_next(struct a733_hevc_run *r)
{if(!r||r->state!=HEVC_COPIED||r->picture)return 0;r->picture=1;r->index=r->flushed=r->flush_bits=0;r->state=HEVC_CHECK;return 1;}
