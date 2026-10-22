/* SPDX-License-Identifier: MIT
 * MMU cache command semantics adapted from pvr_mmu.c and pvr_rogue_fwif.h.
 * Copyright (c) 2023 Imagination Technologies Ltd. See LICENSE.pvr.
 */
#include "pvr_gpu_flush.h"
#include "a733_range.h"
#include "pvr_fw_state.h"

static int outside(const struct pvr_gpu_flush *s, const void *p, size_t n)
{
    return a733_span_excl_nonempty(p,n) &&
        a733_extents_disjoint_prechecked((uintptr_t)s,sizeof(*s),(uintptr_t)p,n);
}
static void put32(unsigned char *p, uint32_t value)
{ for (unsigned i=0;i<4;++i) p[i]=(unsigned char)(value>>(8*i)); }

int pvr_gpu_flush_init(struct pvr_gpu_flush *s, struct pvr_gpu_control *c,
                       const struct pvr_gpu_flush_ops *ops, void *ctx,
                       enum pvr_gpu_flush_level level, uint64_t deadline)
{
    if (!a733_span_excl_nonempty(s,sizeof(*s)) || s->phase!=PVR_GPU_FLUSH_NEW ||
        !outside(s,c,sizeof(*c)) || !outside(s,ops,sizeof(*ops)) ||
        !outside(s,c->queue,sizeof(*c->queue)) ||
        c->phase!=PVR_GPU_CONTROL_READY || c->queue->phase!=PVR_CCB_ACTIVE ||
        !c->queue->epoch || !ops->now || !ops->publish ||
        (unsigned)level>PVR_GPU_FLUSH_CATALOGUE ||
        deadline<=c->last_now || deadline<=c->queue->last_now) return 0;
    struct pvr_gpu_flush result={0};
    result.ops=*ops; result.context=ctx; result.control=c; result.queue=c->queue;
    result.epoch=c->queue->epoch; result.deadline=deadline;
    result.last_now=c->last_now>c->queue->last_now ? c->last_now : c->queue->last_now;
    result.phase=PVR_GPU_FLUSH_PUBLISH;
    put32(result.command,PVR_CCB_MAGIC|102u);
    put32(result.command+8,UINT32_C(0x04000018)|((1u<<((unsigned)level+1))-1u));
    put32(result.command+12,pvr_fw_state_layout(PVR_FW_MMU_SYNC)->address);
    /* The pinned driver uses zero as the sync update value and waits for the
     * command return slot. Reading the initially-zero sync word is no proof. */
    *s=result; return 1;
}
static enum pvr_gpu_flush_result fail(struct pvr_gpu_flush *s, unsigned error)
{
    s->phase=PVR_GPU_FLUSH_FAILED; s->error=error;
    (void)pvr_gpu_control_abort(s->control);
    (void)pvr_ccb_abort(s->queue);
    return PVR_GPU_FLUSH_FAULT;
}
static int clock_now(struct pvr_gpu_flush *s, uint64_t *now)
{
    if (!s->ops.now(s->context,now)) { fail(s,PVR_GPU_FLUSH_IO); return 0; }
    if (*now<s->last_now || *now<s->control->last_now || *now<s->queue->last_now) {
        fail(s,PVR_GPU_FLUSH_CLOCK); return 0;
    }
    s->last_now=*now;
    if (*now>=s->deadline) { fail(s,PVR_GPU_FLUSH_TIMEOUT); return 0; }
    return 1;
}
static int service(struct pvr_gpu_flush *s)
{
    enum pvr_gpu_control_result r=pvr_gpu_control_poll(s->control,4);
    if (r!=PVR_GPU_CONTROL_OK && r!=PVR_GPU_CONTROL_AGAIN) {
        fail(s,PVR_GPU_FLUSH_CONTROL); return 0;
    }
    return 1;
}
enum pvr_gpu_flush_result pvr_gpu_flush_poll(struct pvr_gpu_flush *s)
{
    if (!s) return PVR_GPU_FLUSH_INVALID;
    if (s->phase==PVR_GPU_FLUSH_FAILED) return PVR_GPU_FLUSH_FAULT;
    if (s->phase==PVR_GPU_FLUSH_COMPLETE) return PVR_GPU_FLUSH_OK;
    if (s->phase<PVR_GPU_FLUSH_PUBLISH || s->phase>PVR_GPU_FLUSH_WAIT)
        return PVR_GPU_FLUSH_INVALID;
    if (s->control->phase!=PVR_GPU_CONTROL_READY || s->control->queue!=s->queue ||
        s->queue->phase!=PVR_CCB_ACTIVE || s->queue->epoch!=s->epoch)
        return fail(s,PVR_GPU_FLUSH_CONTROL);
    uint64_t now;
    if (!clock_now(s,&now)) return PVR_GPU_FLUSH_FAULT;
    if (s->phase==PVR_GPU_FLUSH_PUBLISH) {
        if (!s->ops.publish(s->context)) return fail(s,PVR_GPU_FLUSH_IO);
        s->phase=PVR_GPU_FLUSH_SUBMIT;
    } else if (s->phase==PVR_GPU_FLUSH_SUBMIT) {
        enum pvr_ccb_result r=pvr_ccb_submit(s->queue,s->command,now,s->deadline,&s->token);
        if (r==PVR_CCB_AGAIN) {
            if (!service(s)) return PVR_GPU_FLUSH_FAULT;
        } else if (r!=PVR_CCB_OK) return fail(s,PVR_GPU_FLUSH_COMMAND);
        else s->phase=PVR_GPU_FLUSH_WAIT;
    } else {
        if (!service(s)) return PVR_GPU_FLUSH_FAULT;
        enum pvr_ccb_result r=pvr_ccb_collect(s->queue,s->token,&s->status);
        if (r!=PVR_CCB_OK && r!=PVR_CCB_AGAIN) return fail(s,PVR_GPU_FLUSH_COMMAND);
        if (r==PVR_CCB_OK) {
            if (s->status!=1) return fail(s,PVR_GPU_FLUSH_COMMAND);
            s->phase=PVR_GPU_FLUSH_COMPLETE;
        }
    }
    if (!clock_now(s,&now)) return PVR_GPU_FLUSH_FAULT;
    return s->phase==PVR_GPU_FLUSH_COMPLETE ? PVR_GPU_FLUSH_OK : PVR_GPU_FLUSH_AGAIN;
}
