/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register sequencing and training adapted from sunxi_edp.c and
 * trilinear_dp14.c, pinned in apps/displayport/sources.json.
 * Copyright (c) 2007-2022 Allwinnertech Co., Ltd. */
#include "a733_dp_internal.h"
#include "a733_dp_regs.h"
#include <string.h>
static int separate(const void *a,size_t n,const void *b,size_t m)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return n<=UINTPTR_MAX-x&&m<=UINTPTR_MAX-y&&(x+n<=y||y+m<=x);}
int a733_dp_fault(struct a733_dp *d,enum a733_dp_error e)
{if(d&&!d->failed){d->failed=1;d->result.error=e;d->result.state=A733_DP_FAILED_RETAINED;d->result.retained=1;}return 0;}
static int live(struct a733_dp *d)
{
 uint64_t now;
 if(!d||d->self!=d||d->failed)return 0;
 if(memcmp(&d->config,&d->sealed,sizeof(d->config))||d->config.ops.admit(d->config.context,0)!=1)return a733_dp_fault(d,A733_DP_AUTHORITY);
 if(d->config.ops.now(d->config.context,&now)!=1||now<d->last_now||now>=d->config.deadline)return a733_dp_fault(d,A733_DP_TIME);
 d->last_now=now;return 1;
}
int a733_dp_read(struct a733_dp *d,uintptr_t a,unsigned n,uint32_t *v)
{if(!live(d))return 0;if(d->config.ops.read(d->config.context,a,n,v)!=1)return a733_dp_fault(d,A733_DP_IO);return live(d);}
int a733_dp_write(struct a733_dp *d,uintptr_t a,unsigned n,uint32_t v)
{if(!live(d))return 0;if(d->config.ops.write(d->config.context,a,n,v)!=1)return a733_dp_fault(d,A733_DP_IO);return live(d);}
int a733_dp_wait(struct a733_dp *d,unsigned us)
{if(!live(d))return 0;if(d->config.ops.wait(d->config.context,us,d->config.deadline)!=1)return a733_dp_fault(d,A733_DP_TIME);return live(d);}
static int rd(struct a733_dp *d,unsigned reg,uint32_t *v){return a733_dp_read(d,A733_DP_BASE+reg,4,v);}
static int wr(struct a733_dp *d,unsigned reg,uint32_t v){return a733_dp_write(d,A733_DP_BASE+reg,4,v);}
static int field(struct a733_dp *d,uintptr_t a,uint32_t mask,uint32_t v)
{uint32_t x,y;if(!a733_dp_read(d,a,4,&x)||!a733_dp_write(d,a,4,(x&~mask)|v)||!a733_dp_read(d,a,4,&y))return 0;return y==((x&~mask)|v)?1:a733_dp_fault(d,A733_DP_STATUS);}
static int f(struct a733_dp *d,unsigned r,uint32_t m,uint32_t v){return field(d,A733_DP_BASE+r,m,v);}
/* One AUX request at a time. READ reply byte count is a separate register from
 * the transaction counter. Unknown/NACK/short replies terminate; only explicit
 * DEFER permits a bounded repeat after the old request/reply is inactive. */
static int aux(struct a733_dp *d,unsigned cmd,unsigned address,uint8_t *data,unsigned n)
{
 if(!n||n>16||address>0xfffff)return a733_dp_fault(d,A733_DP_ARGUMENT);
 for(unsigned attempt=0;attempt<8;attempt++){
  uint32_t s,irq,code,count,before,received;uint64_t end;
  if(!rd(d,TR_AUX_STATUS,&s)||s&(AUX_REQUEST_IN_PROGRESS|AUX_REPLY_IN_PROGRESS))return a733_dp_fault(d,A733_DP_AUX);
  if(!rd(d,TR_INTERRUPT_CAUSE,&irq)||!rd(d,TR_AUX_REPLY_COUNT,&before)||!wr(d,TR_AUX_ADDRESS,address))return 0;
  if(!(cmd&0x100))for(unsigned i=0;i<n;i++)if(!wr(d,TR_AUX_WRITE_FIFO,data[i]))return 0;
  if(!wr(d,TR_AUX_COMMAND,cmd|(n-1)))return 0;
  ++d->result.aux_requests;end=d->last_now+2000;
  unsigned completed=0;
  for(unsigned poll=0;poll<128;poll++){
   if(!rd(d,TR_AUX_STATUS,&s)||!rd(d,TR_INTERRUPT_STATE,&irq)||!rd(d,TR_AUX_REPLY_COUNT,&received))return 0;
   d->result.aux_status=s;
   if((s&AUX_REPLY_ERROR)||(irq&REPLY_TIMEOUT))return a733_dp_fault(d,A733_DP_AUX);
   if((uint8_t)received==(uint8_t)(before+1)&&(s&AUX_REPLY_RECEIVED)&&!(s&(AUX_REQUEST_IN_PROGRESS|AUX_REPLY_IN_PROGRESS))){completed=1;break;}
   if(d->last_now>=end||!a733_dp_wait(d,20))return a733_dp_fault(d,A733_DP_AUX);
  }
  if(!completed)return a733_dp_fault(d,A733_DP_AUX);
  if(!rd(d,TR_AUX_REPLY_CODE,&code)||!rd(d,TR_REPLY_DATA_COUNT,&count))return 0;
  d->result.aux_reply=code;d->result.aux_count=count;
  if(code==NATIVE_AUX_DEFER||code==I2C_AUX_DEFER){++d->result.aux_defers;if(!a733_dp_wait(d,400))return 0;continue;}
  if(code!=AUX_ACK)return a733_dp_fault(d,A733_DP_AUX);
  if(cmd&0x100){
   if(count!=n)return a733_dp_fault(d,A733_DP_AUX);
   for(unsigned i=0;i<n;i++){if(!rd(d,TR_AUX_REPLY_DATA_FIFO,&s))return 0;data[i]=(uint8_t)s;}
  }else if(count)return a733_dp_fault(d,A733_DP_AUX);
  return 1;
 }
 return a733_dp_fault(d,A733_DP_AUX);
}
static int dpcd_read(struct a733_dp *d,unsigned a,uint8_t *p,unsigned n){return aux(d,TR_DP_CMD_REQUEST_READ,a,p,n);}
static int dpcd_write(struct a733_dp *d,unsigned a,const uint8_t *p,unsigned n)
{uint8_t b[16];if(n>16)return 0;memcpy(b,p,n);return aux(d,TR_DP_CMD_REQUEST_WRITE,a,b,n);}
static int checksum(const uint8_t *p){unsigned s=0;for(unsigned i=0;i<128;i++)s+=p[i];return !(s&255);}
static int dtd720(const uint8_t *p)
{
 unsigned h=p[2]|((p[4]&0xf0)<<4),hb=p[3]|((p[4]&15)<<8);
 unsigned v=p[5]|((p[7]&0xf0)<<4),vb=p[6]|((p[7]&15)<<8);
 unsigned hf=p[8]|((p[11]&0xc0)<<2),hs=p[9]|((p[11]&0x30)<<4);
 unsigned vf=(p[10]>>4)|((p[11]&12)<<2),vs=(p[10]&15)|((p[11]&3)<<4);
 return (p[0]|(p[1]<<8))==7425&&h==1280&&hb==370&&v==720&&vb==30&&hf==110&&hs==40&&vf==5&&vs==5&&(p[17]&0x9e)==0x1e;
}
static int edid(struct a733_dp *d)
{
 static const uint8_t header[]={0,255,255,255,255,255,255,0};
 unsigned supports=0,blocks=1;
 for(unsigned block=0;block<blocks;block++){
  uint8_t offset=(uint8_t)(block*128);if(!aux(d,TR_DP_CMD_REQUEST_I2C_WRITE,0x50,&offset,1))return 0;
  for(unsigned i=0;i<128;i+=16)if(!aux(d,TR_DP_CMD_REQUEST_I2C_READ,0x50,d->result.edid+block*128+i,16))return 0;
  const uint8_t *p=d->result.edid+block*128;if(!checksum(p))return a733_dp_fault(d,A733_DP_EDID);
  d->result.edid_bytes+=128;
  if(!block){
   if(memcmp(p,header,8)||p[18]!=1||p[19]>4||!(p[20]&0x80))return a733_dp_fault(d,A733_DP_EDID);
   for(unsigned i=54;i+18<=126;i+=18)supports|=dtd720(p+i);
   if(p[126])blocks=2;
  }else if(p[0]==2){
   unsigned end=p[2];if(end&& (end<4||end>127))return a733_dp_fault(d,A733_DP_EDID);
   if(end)for(unsigned i=4;i<end;){unsigned len=p[i]&31,tag=p[i]>>5;if(i+1+len>end)return a733_dp_fault(d,A733_DP_EDID);if(tag==2)for(unsigned j=1;j<=len;j++)supports|=(p[i+j]&127)==4;i+=1+len;}
   if(end)for(unsigned i=end;i+18<=127;i+=18)supports|=dtd720(p+i);
  }
 }
 return supports?1:a733_dp_fault(d,A733_DP_EDID);
}
static int trained(const uint8_t *s,unsigned eq)
{for(unsigned i=0;i<4;i++)if(((s[i/2]>>((i&1)*4))&(eq?7:1))!=(eq?7:1))return 0;return !eq||(s[2]&1);}
static int training(struct a733_dp *d,unsigned eq)
{
 uint8_t pattern=(uint8_t)((eq?2:1)|0x20);unsigned same=0;
 /* Manual15.4.5.1 requires the receiver in training mode before source. */
 if(!dpcd_write(d,0x102,&pattern,1)||!f(d,TR_SCRAMBLING_DISABLE,1,1)||!f(d,TR_TRAINING_PATTERN_SET,7,eq?2:1))return 0;
 for(unsigned attempt=0;attempt<(eq?5u:10u);attempt++){
  if(eq)++d->result.eq_attempts;else ++d->result.cr_attempts;
  if(!a733_dp_cadence_levels(d,d->result.training)||!dpcd_write(d,0x103,d->result.training,4))return 0;
  unsigned interval=d->result.dpcd[14]&127;
  if(interval>4)return a733_dp_fault(d,A733_DP_SINK);
  if(!a733_dp_wait(d,interval?interval*4000:(eq?400:100))||!dpcd_read(d,0x202,d->result.link_status,6))return 0;
  if(trained(d->result.link_status,eq))return 1;
  if(eq&&!trained(d->result.link_status,0))return a733_dp_fault(d,A733_DP_TRAINING);
  uint8_t next[4];unsigned changed=0;
  for(unsigned i=0;i<4;i++){
   unsigned req=(d->result.link_status[4+i/2]>>((i&1)*4))&15,v=req&3,p=(req>>2)&3;
   if(v+p>3)p=3-v;
   next[i]=(uint8_t)(v|(p<<3)|(v==3?4:0)|(p==3-v?0x20:0));
   changed|=(next[i]&3)!=(d->result.training[i]&3);
  }
  same=changed?0:same+1;if(!eq&&same>=5)return a733_dp_fault(d,A733_DP_TRAINING);
  memcpy(d->result.training,next,4);
 }
 return a733_dp_fault(d,A733_DP_TRAINING);
}
int a733_dp_init(struct a733_dp *d,const struct a733_dp_config *c)
{
 if(!d||!c||!separate(d,sizeof(*d),c,sizeof(*c))||d->self||!c->owner||!c->deadline||c->polarity<1||c->polarity>2||c->apb_hz<10000000||c->apb_hz>400000000||c->apb_hz%1000000||!c->ops.admit||!c->ops.read||!c->ops.write||!c->ops.barrier||!c->ops.now||!c->ops.wait)return 0;
 d->self=d;d->config=*c;d->sealed=*c;d->result.retained=1;return 1;
}
int a733_dp_prepare(struct a733_dp *d)
{
 uint32_t x;
 if(!d||d->self!=d||d->entered||d->result.state!=A733_DP_NEW||!live(d))return 0;
 d->entered=1;d->result.state=A733_DP_PREPARING;
 d->result.stage=1;
 if(!rd(d,TR_VIDEO_STREAM_ENABLE,&x)||x&1||!rd(d,TR_TRANSMITTER_OUTPUT_EN,&x)||x&1)goto bad;
 if(!a733_dp_cadence_prepare(d))goto bad;
 d->phy_ready=1;d->result.stage=2;
 if(!a733_dp_write(d,A733_DP_PAD_BASE,4,1)||!a733_dp_write(d,A733_DP_PAD_BASE+4,4,1)||!a733_dp_write(d,A733_DP_PAD_BASE+0x18,4,1)||
    !wr(d,TR_INTERRUPT_MASK,0x7fff)||!f(d,TR_CAPBILITY_CONFIG,3,0)||!wr(d,TR_AUX_CLOCK_DIVIDER,d->config.apb_hz/1000000)||
    !wr(d,TR_AUX_REPLY_TIMEOUT_INTV,440)||!wr(d,TR_INPUT_SOURCE_ENABLE,1)||!a733_dp_wait(d,500))goto bad;
 d->result.stage=3;
 if(!dpcd_read(d,0,d->result.dpcd,16))goto bad;
 if(d->result.dpcd[0]<0x11||d->result.dpcd[0]>0x14||d->result.dpcd[1]<6||(d->result.dpcd[2]&31)!=4)goto badsink;
 if(!edid(d))goto bad;
 uint8_t power=1,bw[2]={6,4},coding[2]={0,1};
 if(d->result.dpcd[2]&0x80)bw[1]|=0x80;
 if(!dpcd_write(d,0x600,&power,1)||!a733_dp_wait(d,1000)||!dpcd_write(d,0x100,bw,2)||!dpcd_write(d,0x107,coding,2)||
    !wr(d,TR_LINK_BW_SET,6)||!wr(d,TR_LANE_COUNT_SET,4)||!wr(d,TR_ENHANCED_FRAME_EN,(bw[1]>>7)&1)||!wr(d,TR_TRANSMITTER_OUTPUT_EN,1))goto bad;
 d->result.stage=4;if(!training(d,0)||!training(d,1))goto bad;
 uint8_t clear=0;
 if(!dpcd_write(d,0x102,&clear,1)||!wr(d,TR_TRAINING_PATTERN_SET,0)||!wr(d,TR_SCRAMBLING_DISABLE,0))goto bad;
 d->result.stage=5;
 const struct {unsigned r,v;} video[]={
  {TR_SRC0_MAIN_STREAM_HTOTAL,1650},{TR_SRC0_MAIN_STREAM_VTOTAL,750},{TR_SRC0_MAIN_STREAM_POLAR,3},
  {TR_SRC0_MAIN_STREAM_HSW,40},{TR_SRC0_MAIN_STREAM_VSW,5},{TR_SRC0_MAIN_STREAM_HRES,1280},{TR_SRC0_MAIN_STREAM_VRES,720},
  {TR_SRC0_MAIN_STREAM_HSTART,260},{TR_SRC0_MAIN_STREAM_VSTART,25},{TR_SRC0_MAIN_STREAM_MISC0,0x20},{TR_SRC0_MAIN_STREAM_MISC1,0},
  {TR_SRC0_MVID,74250},{TR_SRC0_NVID,162000},{TR_SRC0_USER_PIXEL_CNT,1},{TR_SRC0_USER_DATA_CNT,960},
  {TR_SRC0_MAIN_STREAM_INTERLACE,0},{TR_SRC0_SECOND_DATA_WINDOW,283},{TR_SRC0_TU_CONFIG,0x00160040},
  {TR_SEC0_AUDIO_ENABLE,0},{TR_SECOND_STREAM_ENABLE,0}};
 for(unsigned i=0;i<sizeof(video)/sizeof(*video);i++)if(!wr(d,video[i].r,video[i].v))goto bad;
 d->result.state=A733_DP_READY;d->entered=0;return 1;
 badsink:a733_dp_fault(d,A733_DP_SINK);
 bad:if(!d->failed)a733_dp_fault(d,A733_DP_STATUS);d->entered=0;return 0;
}
int a733_dp_start(struct a733_dp *d)
{
 if(!d||d->self!=d||d->entered||d->result.state!=A733_DP_READY||!live(d))return 0;
 d->entered=1;
 if(d->config.ops.admit(d->config.context,1)!=1){d->entered=0;return a733_dp_fault(d,A733_DP_SOURCE);}
 if(!dpcd_read(d,0x202,d->result.link_status,6)||!trained(d->result.link_status,1)||!wr(d,TR_VIDEO_STREAM_ENABLE,1)||d->config.ops.barrier(d->config.context)!=1){d->entered=0;return a733_dp_fault(d,A733_DP_STATUS);}
 d->stream_started=1;d->result.state=A733_DP_ACTIVE;d->entered=0;return 1;
}
int a733_dp_service(struct a733_dp *d)
{
 if(!d||d->self!=d||d->entered||d->result.state!=A733_DP_ACTIVE||!live(d))return 0;
 d->entered=1;
 int ok=rd(d,TR_SRC0_USER_FRAME_STATUS,&d->result.frame_status)&&rd(d,TR_SRC0_USER_FIFO_STATUS,&d->result.fifo_status)&&
  a733_dp_read(d,A733_DP_TV_BASE+0xfc,4,&d->result.tv_status)&&dpcd_read(d,0x202,d->result.link_status,6)&&trained(d->result.link_status,1)&&
 !(d->result.frame_status&3)&&!(d->result.fifo_status&3)&&!(d->result.tv_status&0x60000000);
 d->entered=0;return ok?1:a733_dp_fault(d,A733_DP_STATUS);
}
int a733_dp_stop(struct a733_dp *d)
{
 if(!d||d->self!=d||d->entered||d->failed||d->result.state!=A733_DP_ACTIVE||!live(d))return 0;
 d->entered=1;
 int ok=wr(d,TR_VIDEO_STREAM_ENABLE,0)&&wr(d,TR_SECOND_STREAM_ENABLE,0)&&wr(d,TR_TRANSMITTER_OUTPUT_EN,0)&&
  d->config.ops.barrier(d->config.context)==1;
 if(ok){d->stream_started=0;d->result.state=A733_DP_STOPPED_RETAINED;d->result.stage=6;}
 d->entered=0;return ok?1:a733_dp_fault(d,A733_DP_STATUS);
}
int a733_dp_copy_result(struct a733_dp *d,struct a733_dp_result *out)
{if(!d||d->self!=d||!out||d->entered||!separate(d,sizeof(*d),out,sizeof(*out)))return 0;*out=d->result;return 1;}
