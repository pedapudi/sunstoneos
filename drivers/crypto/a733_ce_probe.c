/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_ce_probe.h"
#define CCU UINT32_C(0x02002000)
#define NS UINT32_C(0x04603000)
static void volatile_zero(uint8_t *p,size_t n){volatile uint8_t *v=p;while(n--)*v++=0;}
void a733_ce_probe_init(struct a733_ce_probe *p,const struct a733_ce_probe_ops *o)
{
    *p=(struct a733_ce_probe){.ops=*o,.stage=A733_CE_PROBE_NEW};
    p->record=(struct a733_ce_probe_record){.magic=A733_CE_PROBE_MAGIC,.version=A733_CE_PROBE_VERSION,
        .bytes=sizeof(struct a733_ce_probe_record),.flags=1u,.step_limit_us=A733_CE_PROBE_STEP_LIMIT_US};
}
static int fail(struct a733_ce_probe *p)
{
    p->record.failed=1;p->record.failure_stage=p->stage+1;p->record.stage=p->stage;
    if(p->ops.detail)p->record.failure_stage|=(p->ops.detail(p->ops.ctx)&0xffffffu)<<8;
    if(p->ops.retain)p->ops.retain(p->ops.ctx);
    return -1;
}
static int words(struct a733_ce_probe *p,uint32_t *clk,uint32_t *bgr,uint32_t *mbus)
{
    return p->ops.read(p->ops.ctx,CCU+0xac0,clk)==1&&p->ops.read(p->ops.ctx,CCU+0xac4,bgr)==1&&
        p->ops.read(p->ops.ctx,CCU+0x5e4,mbus)==1;
}
int a733_ce_probe_step(struct a733_ce_probe *p)
{
    struct a733_ce_probe_record *r=&p->record;
    uint64_t now;
    if(!p||p->stage==A733_CE_PROBE_DONE)return 1;
    if(r->failed||p->ops.now_us(p->ops.ctx,&now)!=1)return r->failed?-1:fail(p);
    int result;
    switch(p->stage) {
    case A733_CE_PROBE_NEW:
        if(!words(p,&r->entry_clk,&r->entry_bgr,&r->entry_mbus))return fail(p);
        /* Found enabled: clock gate on, both resets released, both bus gates open (the board's state at FEL entry). */
        if((r->entry_clk&0x8700001fu)==0x81000000u&&(r->entry_bgr&0x30003u)==0x30003u)r->flags|=2u;
        {p->stage=A733_CE_PROBE_ENTRY;r->stage=A733_CE_PROBE_ENTRY;}return 0;
    case A733_CE_PROBE_ENTRY:
        result=p->ops.bring_up(p->ops.ctx);
        if(result<0)return fail(p);
        if(!result)return 0;
        if(!words(p,&r->clocked_clk,&r->clocked_bgr,&r->clocked_mbus))return fail(p);
        {p->stage=A733_CE_PROBE_CLOCKED;r->stage=A733_CE_PROBE_CLOCKED;}return 0;
    case A733_CE_PROBE_CLOCKED:
        result=p->ops.alloc(p->ops.ctx,&r->descriptor_physical,&r->output_physical);
        if(result<0)return fail(p);
        if(result){p->stage=A733_CE_PROBE_BUFFERS;r->stage=A733_CE_PROBE_BUFFERS;}
        return 0;
    case A733_CE_PROBE_BUFFERS:
        result=p->ops.configure(p->ops.ctx);
        if(result<0)return fail(p);
        if(result){p->stage=A733_CE_PROBE_CONFIGURED;r->stage=A733_CE_PROBE_CONFIGURED;}
        return 0;
    case A733_CE_PROBE_CONFIGURED:
        result=p->ops.request(p->ops.ctx,now+A733_CE_PROBE_STEP_LIMIT_US);
        if(result<0)return fail(p);
        if(result){p->started_us=now;{p->stage=A733_CE_PROBE_REQUESTED;r->stage=A733_CE_PROBE_REQUESTED;}}
        return 0;
    case A733_CE_PROBE_REQUESTED: {
        uint8_t block[A733_CE_PROBE_BYTES];
        result=p->ops.poll(p->ops.ctx,block);
        if(result<0){volatile_zero(block,sizeof(block));return fail(p);}
        if(!result){
            volatile_zero(block,sizeof(block));
            return now-p->started_us>=A733_CE_PROBE_STEP_LIMIT_US?fail(p):0;
        }
        uint8_t any=0;for(unsigned i=0;i<sizeof(block);++i)any|=block[i];
        volatile_zero(block,sizeof(block));
        r->task_us=(uint32_t)(now-p->started_us);r->bytes_returned=A733_CE_PROBE_BYTES;
        r->flags|=4u|(any?8u:0u);
        if(!p->ops.read(p->ops.ctx,NS+12,&r->status_ns)||!p->ops.read(p->ops.ctx,NS+0x18,&r->error_ns)||
           !p->ops.read(p->ops.ctx,NS+0xd0,&r->lpc))return fail(p);
        if(r->lpc&2u)r->flags|=16u;
        {p->stage=A733_CE_PROBE_DONE;r->stage=A733_CE_PROBE_DONE;}return 1;
    }
    default:return fail(p);
    }
}
