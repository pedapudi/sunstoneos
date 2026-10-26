/* SPDX-License-Identifier: MIT
 * Firmware protocol follows RGXForcedIdleRequest, RGXPostClockSpeedChange
 * and RGXCancelForcedIdleRequest in the pinned BXM driver. Command IDs
 * and memory layouts use the selected native pvr_rogue_fwif.h ABI; see
 * docs/designs/gpu-performance.md.
 * Copyright (c) Imagination Technologies Ltd. All Rights Reserved.
 * Permission notice: drivers/gpu/LICENSE.pvr.
 */
#include "pvr_gpu_clock.h"
#include "a733_gpu_opp.h"
#include "a733_range.h"

static int fail(struct pvr_gpu_clock *s,unsigned error)
{
    s->error=error;s->phase=PVR_GPU_CLOCK_FAILED;
    (void)pvr_gpu_control_abort(s->control);
    return -1;
}
static int now(struct pvr_gpu_clock *s,uint64_t *t)
{
    if(!s->ops.now(s->context,t) || *t<s->last_now || *t>=s->limit ||
        (s->phase!=PVR_GPU_CLOCK_HELD && *t>=s->deadline))return fail(s,PVR_GPU_CLOCK_TIME);
    s->last_now=*t;return 1;
}
int pvr_gpu_clock_hold(struct pvr_gpu_clock *s,struct pvr_gpu_control *c,
    const struct pvr_gpu_clock_ops *ops,void *ctx,uint64_t deadline,uint64_t limit)
{
    if(!s || !c || !ops || (s->phase!=PVR_GPU_CLOCK_NEW && s->phase!=PVR_GPU_CLOCK_READY) ||
        c->phase!=PVR_GPU_CONTROL_READY || !c->queue || c->queue->phase!=PVR_CCB_ACTIVE ||
        !deadline || deadline>limit || !ops->now || !ops->sync_write || !ops->sync_read ||
        !ops->power_state || !ops->publish ||
        !a733_ranges_disjoint(s,sizeof(*s),c,sizeof(*c)) ||
        !a733_ranges_disjoint(s,sizeof(*s),c->queue,sizeof(*c->queue)) ||
        !a733_ranges_disjoint(s,sizeof(*s),ops,sizeof(*ops)))return 0;
    const uint64_t last=s->last_now>c->last_now ? s->last_now : c->last_now;
    *s=(struct pvr_gpu_clock){.control=c,.ops=*ops,.context=ctx,.deadline=deadline,
        .limit=limit,.last_now=last,.phase=PVR_GPU_CLOCK_CLEAR_IDLE};
    return 1;
}
int pvr_gpu_clock_resume(struct pvr_gpu_clock *s,uint32_t rate,uint64_t deadline)
{
    if(!s || s->phase!=PVR_GPU_CLOCK_HELD || !deadline || deadline>s->limit ||
        (rate!=A733_GPU_CORE_HZ && rate!=A733_GPU_LOW_HZ))return 0;
    s->rate=rate;s->deadline=deadline;s->phase=PVR_GPU_CLOCK_PUBLISH;return 1;
}
static void put32(unsigned char *p,uint32_t v)
{for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(8*i));}
static int send(struct pvr_gpu_clock *s,unsigned which,uint64_t t)
{
    unsigned char command[PVR_CCB_COMMAND_BYTES]={0};
    put32(command,PVR_CCB_MAGIC|(which==1 ? 204u : 107u));
    put32(command+8,which==1 ? s->rate : 2u);
    if(which!=1)put32(command+12,which==0 ? 1u : 2u);
    enum pvr_ccb_result r=pvr_ccb_submit(s->control->queue,command,t,s->deadline,&s->token[which]);
    if(r==PVR_CCB_AGAIN)return 0;
    if(r!=PVR_CCB_OK)return fail(s,PVR_GPU_CLOCK_COMMAND);
    s->published|=1u<<which;++s->phase;return 0;
}
static int settle(struct pvr_gpu_clock *s,unsigned i)
{
    enum pvr_ccb_result r=pvr_ccb_settle(s->control->queue,s->token[i],&s->status[i]);
    if(r==PVR_CCB_AGAIN)return 0;
    if(r!=PVR_CCB_OK || s->status[i]>1)return fail(s,PVR_GPU_CLOCK_COMMAND);
    return 1;
}
int pvr_gpu_clock_poll(struct pvr_gpu_clock *s)
{
    if(!s || !s->control || s->phase==PVR_GPU_CLOCK_NEW)return -1;
    if(s->phase==PVR_GPU_CLOCK_FAILED)return -1;
    if(s->phase==PVR_GPU_CLOCK_READY)return 1;
    if(s->control->phase!=PVR_GPU_CONTROL_READY || s->control->queue->phase!=PVR_CCB_ACTIVE)
        return fail(s,PVR_GPU_CLOCK_CONTROL);
    uint64_t t;
    if(now(s,&t)<0)return -1;
    enum pvr_gpu_control_result cr=pvr_gpu_control_poll(s->control,4);
    if(cr!=PVR_GPU_CONTROL_OK && cr!=PVR_GPU_CONTROL_AGAIN)return fail(s,PVR_GPU_CLOCK_CONTROL);
    if(now(s,&t)<0)return -1;
    switch(s->phase) {
    case PVR_GPU_CLOCK_HELD:return 1;
    case PVR_GPU_CLOCK_CLEAR_IDLE:case PVR_GPU_CLOCK_CLEAR_CANCEL:
        if(!s->ops.sync_write(s->context,0))return fail(s,PVR_GPU_CLOCK_IO);
        ++s->phase;break;
    case PVR_GPU_CLOCK_SEND_IDLE:return send(s,0,t);
    case PVR_GPU_CLOCK_NOTIFY:return send(s,1,t);
    case PVR_GPU_CLOCK_SEND_CANCEL:return send(s,2,t);
    case PVR_GPU_CLOCK_PUBLISH:
        if(!s->ops.publish(s->context,s->rate))return fail(s,PVR_GPU_CLOCK_IO);
        ++s->phase;break;
    case PVR_GPU_CLOCK_WAIT_IDLE:case PVR_GPU_CLOCK_WAIT_CANCEL: {
        uint32_t ack=0;
        if(!s->ops.sync_read(s->context,&ack))return fail(s,PVR_GPU_CLOCK_IO);
        if(now(s,&t)<0)return -1;
        if(!ack)return 0;
        if(ack!=1)return fail(s,PVR_GPU_CLOCK_ACK);
        unsigned cancel=s->phase==PVR_GPU_CLOCK_WAIT_CANCEL;
        if(!cancel) {
            uint32_t state;
            if(!s->ops.power_state(s->context,&state))return fail(s,PVR_GPU_CLOCK_IO);
            if(state!=2)return fail(s,PVR_GPU_CLOCK_STATE);
        }
        /* The fresh cancellation ack follows the notification in the same
         * ordered KCCB. Keep each identity until its read offset has passed. */
        unsigned i=cancel ? 1u : 0u;
        for(;i<=(cancel ? 2u : 0u);++i) {
            if(!(s->published&(1u<<i)))continue;
            int r=settle(s,i);if(r!=1)return r;
            s->published&=~(1u<<i);
        }
        if(now(s,&t)<0)return -1;
        s->phase=cancel ? PVR_GPU_CLOCK_READY : PVR_GPU_CLOCK_HELD;
        return 1;
    }
    default:return fail(s,PVR_GPU_CLOCK_STATE);
    }
    return now(s,&t)<0 ? -1 : 0;
}
