#include "a733_twi.h"
#include <stddef.h>
#define ENABLE 0x40u
#define FLAG 8u
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
int a733_twi_divider(uint32_t hz,uint32_t *divider,uint32_t *rate)
{
    if(!hz || !divider || !rate || !separate(divider,sizeof(*divider),rate,sizeof(*rate)))return 0;
    uint32_t best=0,word=0;
    for(unsigned n=0;n<8;n++)for(unsigned m=0;m<16;m++) {
        uint32_t d=(1u<<n)*(m+1u)*10u;
        if((uint64_t)hz>UINT64_C(100000)*d)continue;
        if(!best || d<best){best=d;word=(m<<3)|n;}
    }
    if(!best || hz<best)return 0;
    *divider=word;*rate=hz/best;return 1;
}
int a733_twi_init(struct a733_twi *s,const struct a733_twi_ops *o,void *ctx,uint32_t divider)
{return a733_twi_init_controller(s,o,ctx,divider,A733_TWI_MAIN11,2);}
int a733_twi_init_controller(struct a733_twi *s,const struct a733_twi_ops *o,void *ctx,
    uint32_t divider,enum a733_twi_controller controller,unsigned register_bytes)
{
    if(!s || s->self || s->state!=A733_TWI_NEW || !o || !ctx || divider>127 ||
       (controller!=A733_TWI_MAIN11 && controller!=A733_TWI_R1) ||
       (register_bytes!=1 && register_bytes!=2) ||
       !separate(s,sizeof(*s),o,sizeof(*o)) || !separate(s,sizeof(*s),ctx,1) ||
       !o->now || !o->authenticate || !o->enter || !o->leave || !o->retain || !o->read || !o->write || !o->current)return 0;
    *s=(struct a733_twi){.self=s,.ops=*o,.context=ctx,.divider=divider,.state=A733_TWI_READY,
        .base=controller==A733_TWI_MAIN11?A733_TWI11_BASE:UINT32_C(0x07084000),
        .register_bytes=(uint8_t)register_bytes};return 1;
}
static int fail(struct a733_twi *s)
{
    s->state=A733_TWI_FAULT;
    s->ops.retain(s->context);
    return -1;
}
static int tick(struct a733_twi *s)
{
    uint64_t n;
    if(s->ops.now(s->context,&n)!=1 || n<s->last_now || n>=s->deadline)return 0;
    s->last_now=n;return 1;
}
static int rd(struct a733_twi *s,unsigned off,uint32_t *v)
{return tick(s) && s->ops.read(s->context,s->base+off,v)==1 && tick(s);}
static int wr(struct a733_twi *s,unsigned off,uint32_t v)
{return tick(s) && s->ops.write(s->context,s->base+off,v)==1 && tick(s);}
int a733_twi_request_block(struct a733_twi *s,uint8_t address,uint16_t reg,
    unsigned reading,const uint8_t *data,unsigned length,uint64_t d)
{
    if(!s || s->self!=s || s->state!=A733_TWI_READY || s->entered || reading>1 ||
       !length || length>sizeof(s->block) || (!reading && (!data || !separate(s,sizeof(*s),data,length))) ||
       address<8 || address>0x77 || (s->register_bytes==1 && reg>255) || !d || d<=s->last_now ||
       s->ops.current(s->context)!=1)return -1;
    s->deadline=d;s->address=address;s->reg=reg;s->reading=(uint8_t)reading;
    s->length=(uint8_t)length;
    for(unsigned i=0;i<length;i++)s->block[i]=reading?0:data[i];
    s->index=0;s->phase=0;s->received=0;s->last_status=0;s->state=A733_TWI_BEGIN;return 0;
}
int a733_twi_request(struct a733_twi *s,uint8_t address,uint16_t reg,unsigned reading,uint8_t value,uint64_t d)
{return a733_twi_request_block(s,address,reg,reading,&value,1,d);}
static int idle(struct a733_twi *s)
{
    uint32_t v;
    return rd(s,0xc,&v) && (v&0xfd)==ENABLE && rd(s,0x10,&v) && v==0xf8 &&
        rd(s,0x14,&v) && v==s->divider && rd(s,0x18,&v) && !v &&
        rd(s,0x1c,&v) && !v && rd(s,0x20,&v) && (v&0x35)==0x30 &&
        rd(s,0x200,&v) && !(v&0x80000003) && rd(s,0x214,&v) && !(v&0xffff0000) &&
        rd(s,0x218,&v) && !(v&0x01000100) &&
        rd(s,0x210,&v) && !(v&3) && rd(s,0x21c,&v) && !(v&0x007f007f);
}
int a733_twi_poll(struct a733_twi *s)
{
    if(!s || s->self!=s || s->state==A733_TWI_FAULT)return -1;
    if(s->ops.current(s->context)!=1)return -1;
    if(s->entered)return fail(s);
    if(s->state==A733_TWI_COMPLETE)return 1;
    if(s->state==A733_TWI_READY || s->state==A733_TWI_NEW)return -1;
    s->entered=1;int result=0;uint32_t v;
    if(!tick(s))goto fault;
    switch(s->state) {
    case A733_TWI_BEGIN:
        v=(uint32_t)s->ops.authenticate(s->context);
        if(v==0)break;
        if(v!=1)goto fault;
        int admission=s->ops.enter(s->context,s->deadline);
        if(!admission)break;
        if(admission!=1)goto fault;
        s->held=1;
        if(!idle(s))goto fault;
        s->state=A733_TWI_SEND;break;
    case A733_TWI_SEND:
        if(s->ops.authenticate(s->context)!=1)goto fault;
        if(s->index==0 || (s->reading && s->index==2u+s->register_bytes)) {
            if(!wr(s,0xc,ENABLE|FLAG|0x20))goto fault;
        } else if(s->reading && s->index>=4u+s->register_bytes) {
            if(!wr(s,0xc,ENABLE|FLAG|(s->index==3u+s->register_bytes+s->length?0:4u)))goto fault; /* ACK all but last byte. */
        } else {
            v=s->index==1 ? (uint32_t)s->address<<1 :
                s->index<2u+s->register_bytes ?
                  (s->reg>>(8u*(1u+s->register_bytes-s->index)))&255 :
                s->reading ? ((uint32_t)s->address<<1)|1u : s->block[s->index-2u-s->register_bytes];
            if(!wr(s,8,v) || !wr(s,0xc,ENABLE|FLAG))goto fault;
        }
        s->state=A733_TWI_WAIT;break;
    case A733_TWI_WAIT:
        if(!rd(s,0xc,&v))goto fault;
        if(!(v&FLAG))break;
        if((v&0xf5)!=(ENABLE|((s->reading&&s->index>=4u+s->register_bytes&&
            s->index<3u+s->register_bytes+s->length)?4u:0u)))goto fault;
        if(!rd(s,0x10,&v))goto fault;
        s->last_status=v;
        unsigned expected=s->index==0 ? 0x08 : s->index==1 ? 0x18 :
            s->index<2u+s->register_bytes ? 0x28 : !s->reading ? 0x28 :
            s->index==2u+s->register_bytes ? 0x10 : s->index==3u+s->register_bytes ? 0x40 :
            s->index==3u+s->register_bytes+s->length ? 0x58 : 0x50;
        if(v!=expected)goto fault; /* Includes arbitration loss: never STOP. */
        if(s->reading && s->index>=4u+s->register_bytes) {
            if(!rd(s,8,&v) || v>255)goto fault;
            s->received=(uint8_t)v;s->block[s->index-4u-s->register_bytes]=(uint8_t)v;
        }
        s->state=++s->index==(s->reading?4u:2u)+s->register_bytes+s->length?A733_TWI_STOP:A733_TWI_SEND;break;
    case A733_TWI_STOP:
        if(s->ops.authenticate(s->context)!=1 || !wr(s,0xc,ENABLE|FLAG|0x10))goto fault;
        s->state=A733_TWI_STOP_WAIT;break;
    case A733_TWI_STOP_WAIT:
        if(!rd(s,0xc,&v))goto fault;
        if(v&0x10)break;
        if((v&0xfd)!=ENABLE)goto fault;
        if(!rd(s,0x10,&v))goto fault;
        if(v!=0xf8)break;
        if(!rd(s,0x20,&v) || (v&0x35)!=0x30 || s->ops.authenticate(s->context)!=1)goto fault;
        s->ops.leave(s->context);s->held=0;
        if(!tick(s) || s->ops.authenticate(s->context)!=1)goto fault;
        s->state=A733_TWI_COMPLETE;result=1;break;
    default:goto fault;
    }
    if(!tick(s))goto fault;
    s->entered=0;return result;
fault:
    s->entered=0;return fail(s);
}
int a733_twi_take_block(struct a733_twi *s,uint8_t *value,unsigned length)
{
    if(!s || s->self!=s || s->state!=A733_TWI_COMPLETE || s->entered || length!=s->length ||
       (s->reading && (!value || !separate(s,sizeof(*s),value,length))) || s->ops.current(s->context)!=1)return -1;
    if(!tick(s) || s->ops.authenticate(s->context)!=1)return fail(s);
    if(s->reading)for(unsigned i=0;i<length;i++)value[i]=s->block[i];
    s->received=0;s->state=A733_TWI_READY;return 1;
}
int a733_twi_take(struct a733_twi *s,uint8_t *value)
{return a733_twi_take_block(s,value,1);}
int a733_twi_retain(struct a733_twi *s)
{
    if(!s || s->self!=s || s->state==A733_TWI_FAULT || s->entered || s->ops.current(s->context)!=1)return -1;
    return fail(s);
}
