/* SPDX-License-Identifier: MIT
 * Shutdown semantics adapted from pvr_power.c, pvr_fw_startstop.c,
 * pvr_rogue_fwif.h and pvr_rogue_cr_defs.h.
 * Copyright (c) 2023 Imagination Technologies Ltd. See LICENSE.pvr.
 */
#include "pvr_gpu_stop.h"
#include <string.h>

enum { POLL, WRITE, FENCE };
static int range(const void *p, size_t n)
{ return p && n && n <= UINTPTR_MAX - (uintptr_t)p; }
static int outside(const struct pvr_gpu_stop *s, const void *p, size_t n)
{
    return range(p, n) && ((uintptr_t)s + sizeof(*s) <= (uintptr_t)p ||
                          (uintptr_t)p + n <= (uintptr_t)s);
}
static void add(struct pvr_gpu_stop *s, unsigned kind, unsigned off, unsigned width,
                uint64_t mask, uint64_t value)
{ s->steps[s->count++] = (struct pvr_gpu_stop_step){kind,off,width,mask,value}; }
int pvr_gpu_stop_init(struct pvr_gpu_stop *s, struct pvr_gpu_control *c,
                      const void *image, size_t bytes, const struct pvr_gpu_stop_ops *ops,
                      void *ctx, uint64_t deadline)
{
    struct pvr_fw_plan p;
    if (!range(s,sizeof(*s)) || s->phase != PVR_GPU_STOP_NEW ||
        !outside(s,c,sizeof(*c)) || !outside(s,ops,sizeof(*ops)) ||
        c->phase != PVR_GPU_CONTROL_READY || !outside(s,c->queue,sizeof(*c->queue)) ||
        c->queue->phase != PVR_CCB_ACTIVE || !deadline ||
        !ops->now || !ops->read || !ops->write || !ops->sync_write || !ops->sync_read || !ops->isolate ||
        !pvr_fw_requirements(image,bytes,PVR_FW_A733_BVNC,&p) || !outside(s,image,bytes) ||
        p.device_info_counts[0] != 1 || p.device_info_counts[1] != 1 || p.device_info_counts[2] != 2)
        return 0;
    const unsigned char *f = (const unsigned char *)image + p.device_info_offset + 48;
    if (f[8] & 0x80) return 0;
    for (unsigned i=9;i<16;++i) if (f[i]) return 0;
    struct pvr_gpu_stop result = {0};
    result.ops=*ops; result.control=c; result.context=ctx; result.deadline=deadline;
    result.last_now=c->last_now; result.phase=PVR_GPU_STOP_CLEAR_IDLE;
    add(&result,POLL,0x3c8,4,0x78,0x78);
    add(&result,WRITE,0xb40,4,0,0); add(&result,WRITE,0xb30,4,0,0);
    add(&result,WRITE,0xb48,4,0,0); add(&result,WRITE,0xb38,4,0,0);
    add(&result,POLL,0x1358,4,0xff,0); add(&result,POLL,0x1350,4,0xff,0);
    if (!(f[69/8] & (1u<<(69%8)))) add(&result,POLL,0x1320,4,0xfffffff,0);
    add(&result,POLL,0x1338,4,0xffff,0);
    add(&result,POLL,0x3870,8,UINT64_C(0x800003ff03ffffff),0);
    add(&result,POLL,0x3898,4,0xff,0xff);
    add(&result,POLL,0x3c8,4,0x78,0x78); add(&result,POLL,0x3c8,4,4,4);
    add(&result,WRITE,0x100,8,0,f[32/8]&1 ? UINT64_C(0xffeffffffffffc3d) : UINT64_C(0x00e7fffffffffc3d));
    add(&result,FENCE,0x100,8,0,0);
    *s=result; return 1;
}
static enum pvr_gpu_stop_result fail(struct pvr_gpu_stop *s, unsigned error)
{
    s->phase=PVR_GPU_STOP_FAILED; s->error=error;
    (void)pvr_gpu_control_abort(s->control);
    return PVR_GPU_STOP_FAULT;
}
static int clock_now(struct pvr_gpu_stop *s, uint64_t *now)
{
    if (!s->ops.now(s->context,now)) { fail(s,PVR_GPU_STOP_IO); return 0; }
    if (*now<s->last_now) { fail(s,PVR_GPU_STOP_CLOCK); return 0; }
    s->last_now=*now;
    if (*now>=s->deadline) { fail(s,PVR_GPU_STOP_TIMEOUT); return 0; }
    return 1;
}
static void put32(unsigned char *p, uint32_t value)
{ for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(value>>(8*i)); }
enum pvr_gpu_stop_result pvr_gpu_stop_poll(struct pvr_gpu_stop *s)
{
    if (!s) return PVR_GPU_STOP_INVALID;
    if (s->phase==PVR_GPU_STOP_FAILED) return PVR_GPU_STOP_FAULT;
    if (s->phase==PVR_GPU_STOP_STOPPED) return PVR_GPU_STOP_OK;
    if (s->phase<PVR_GPU_STOP_CLEAR_IDLE || s->phase>PVR_GPU_STOP_ISOLATE) return PVR_GPU_STOP_INVALID;
    int retired=s->phase>=PVR_GPU_STOP_REGISTERS;
    if (s->control->phase!=(retired ? PVR_GPU_CONTROL_RETIRED : PVR_GPU_CONTROL_READY) ||
        s->control->queue->phase!=(retired ? PVR_CCB_RETIRED : PVR_CCB_ACTIVE))
        return fail(s,PVR_GPU_STOP_CONTROL);
    uint64_t now, value=0;
    if (!clock_now(s,&now)) return PVR_GPU_STOP_FAULT;
    switch(s->phase) {
    case PVR_GPU_STOP_CLEAR_IDLE: case PVR_GPU_STOP_CLEAR_OFF:
        if (!s->ops.sync_write(s->context,0)) return fail(s,PVR_GPU_STOP_IO);
        ++s->phase; break;
    case PVR_GPU_STOP_SEND_IDLE: case PVR_GPU_STOP_SEND_OFF: {
        unsigned which=s->phase==PVR_GPU_STOP_SEND_OFF;
        unsigned char command[PVR_CCB_COMMAND_BYTES]={0};
        put32(command,PVR_CCB_MAGIC|107u); put32(command+8,which ? 1u : 2u); put32(command+12,1);
        enum pvr_ccb_result r=pvr_ccb_submit(s->control->queue,command,now,s->deadline,&s->tokens[which]);
        if (r==PVR_CCB_AGAIN) {
            enum pvr_gpu_control_result progress=pvr_gpu_control_poll(s->control,4);
            if (progress!=PVR_GPU_CONTROL_OK && progress!=PVR_GPU_CONTROL_AGAIN)
                return fail(s,PVR_GPU_STOP_CONTROL);
            return PVR_GPU_STOP_AGAIN;
        }
        if (r!=PVR_CCB_OK) return fail(s,PVR_GPU_STOP_COMMAND);
        ++s->phase; break;
    }
    case PVR_GPU_STOP_WAIT_IDLE: case PVR_GPU_STOP_WAIT_OFF: {
        unsigned which=s->phase==PVR_GPU_STOP_WAIT_OFF;
        enum pvr_gpu_control_result r=pvr_gpu_control_poll(s->control,4);
        if (r==PVR_GPU_CONTROL_AGAIN) return PVR_GPU_STOP_AGAIN;
        if (r!=PVR_GPU_CONTROL_OK) return fail(s,PVR_GPU_STOP_CONTROL);
        uint32_t ack=0;
        if (!s->ops.sync_read(s->context,&ack)) return fail(s,PVR_GPU_STOP_IO);
        if (!ack) return PVR_GPU_STOP_AGAIN;
        s->acknowledgement[which]=ack;
        if (!clock_now(s,&now)) return PVR_GPU_STOP_FAULT;
        enum pvr_ccb_result cr=pvr_ccb_collect(s->control->queue,s->tokens[which],&s->command_status[which]);
        if (cr!=PVR_CCB_OK && cr!=PVR_CCB_AGAIN) return fail(s,PVR_GPU_STOP_COMMAND);
        if (cr==PVR_CCB_OK && s->command_status[which]!=1) return fail(s,PVR_GPU_STOP_COMMAND);
        ++s->phase; break;
    }
    case PVR_GPU_STOP_RETIRE:
        if (pvr_gpu_control_retire(s->control)!=PVR_GPU_CONTROL_OK) return fail(s,PVR_GPU_STOP_CONTROL);
        ++s->phase; break;
    case PVR_GPU_STOP_REGISTERS: {
        const struct pvr_gpu_stop_step *op=&s->steps[s->index];
        if (op->kind==WRITE) {
            if (!s->ops.write(s->context,op->offset,op->width,op->value)) return fail(s,PVR_GPU_STOP_IO);
        } else {
            if (!s->ops.read(s->context,op->offset,op->width,&value)) return fail(s,PVR_GPU_STOP_IO);
            if (op->kind==POLL && (value&op->mask)!=op->value) return PVR_GPU_STOP_AGAIN;
        }
        if (++s->index==s->count) ++s->phase;
        break;
    }
    case PVR_GPU_STOP_ISOLATE:
        if (!s->ops.isolate(s->context)) return fail(s,PVR_GPU_STOP_IO);
        s->isolation_observed=1;
        if (!clock_now(s,&now)) return PVR_GPU_STOP_FAULT;
        s->phase=PVR_GPU_STOP_STOPPED; return PVR_GPU_STOP_OK;
    default: return fail(s,PVR_GPU_STOP_CONTROL);
    }
    return PVR_GPU_STOP_AGAIN;
}
