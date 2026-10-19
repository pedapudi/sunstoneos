/* SPDX-License-Identifier: MIT
 * Client queue layout, padding and KICK semantics adapted from pvr_cccb.c,
 * pvr_cccb.h and pvr_rogue_fwif.h. Copyright (c) 2023 Imagination Technologies Ltd.
 * See LICENSE.pvr for retained terms.
 */
#include "pvr_cccb.h"
#include <string.h>

static int range(const void *p, size_t n)
{ return p && n && n <= UINTPTR_MAX-(uintptr_t)p; }
static int disjoint(const void *a, size_t an, const void *b, size_t bn)
{
    return range(a,an) && range(b,bn) &&
        ((uintptr_t)a+an<=(uintptr_t)b || (uintptr_t)b+bn<=(uintptr_t)a);
}
static int outside(const struct pvr_cccb *s, const void *p, size_t n)
{
    return disjoint(s,sizeof(*s),p,n) && disjoint(s->queue,sizeof(*s->queue),p,n);
}
static void put32(unsigned char *p, uint32_t v)
{ for (unsigned i=0;i<4;++i) p[i]=(unsigned char)(v>>(8*i)); }
static uint32_t get32(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int type_valid(uint32_t type)
{
    uint32_t id=type&0x7fff;
    return (id>=201 && id<=211 && type==(PVR_CCB_MAGIC|PVR_CCCB_TASK|id)) ||
           (id>=212 && id<=219 && type==(PVR_CCB_MAGIC|id));
}

int pvr_cccb_init(struct pvr_cccb *s, const struct pvr_cccb_ops *ops, void *ctx,
                  struct pvr_ccb *q, uint32_t size, uint32_t fw, uint64_t deadline)
{
    if (!disjoint(s,sizeof(*s),q,sizeof(*q)) ||
        !disjoint(s,sizeof(*s),ops,sizeof(*ops)) || s->phase!=PVR_CCCB_NEW ||
        !q->epoch || (q->phase!=PVR_CCB_PREPARED && q->phase!=PVR_CCB_ACTIVE) ||
        !ops->now || !ops->read || !ops->write || !ops->barrier ||
        size<256 || size>PVR_CCCB_MAX_BYTES || (size&(size-1)) || !fw || (fw&7) ||
        deadline<=q->last_now) return 0;
    struct pvr_cccb value={0};
    value.ops=*ops; value.context=ctx; value.queue=q; value.epoch=q->epoch;
    value.size=size; value.firmware_context=fw; value.deadline=deadline;
    value.last_now=q->last_now; value.phase=PVR_CCCB_FORMAT;
    *s=value; return 1;
}

static int begin_valid(struct pvr_cccb *s, const struct pvr_cccb_command *cmd,
                   unsigned count, const uint32_t *cleanup, unsigned cleanups,
                   uint64_t deadline)
{
    if (!range(s,sizeof(*s)) || s->phase!=PVR_CCCB_IDLE || !count ||
        count>PVR_CCCB_MAX_COMMANDS || cleanups>PVR_CCCB_MAX_CLEANUPS ||
        !outside(s,cmd,count*sizeof(*cmd)) ||
        (cleanups && !outside(s,cleanup,cleanups*sizeof(*cleanup))) ||
        s->queue->phase!=PVR_CCB_ACTIVE || s->queue->epoch!=s->epoch ||
        deadline<=s->last_now || deadline<=s->queue->last_now || s->serial==UINT64_MAX)
        return 0;
    uint32_t total=0;
    for (unsigned i=0;i<count;++i) {
        if (!type_valid(cmd[i].type) || (cmd[i].bytes&7) ||
            cmd[i].bytes>s->size/2 ||
            (cmd[i].bytes && !outside(s,cmd[i].payload,cmd[i].bytes))) return 0;
        total+=PVR_CCCB_HEADER_BYTES+cmd[i].bytes;
    }
    if (total+PVR_CCCB_HEADER_BYTES>s->size/2) return 0;
    for (unsigned i=0;i<cleanups;++i) if (!cleanup[i] || (cleanup[i]&3)) return 0;
    return 1;
}

int pvr_cccb_begin(struct pvr_cccb *s, const struct pvr_cccb_command *cmd,
                   unsigned count, const uint32_t *cleanup, unsigned cleanups,
                   uint64_t deadline)
{
    if (!begin_valid(s, cmd, count, cleanup, cleanups, deadline)) return 0;
    uint32_t total = 0;
    for (unsigned i = 0; i < count; ++i) total += PVR_CCCB_HEADER_BYTES + cmd[i].bytes;
    memcpy(s->commands,cmd,count*sizeof(*cmd));
    memset(s->kick,0,sizeof(s->kick));
    put32(s->kick,PVR_CCB_MAGIC|101u);
    put32(s->kick+8,s->firmware_context); put32(s->kick+16,s->size-1);
    put32(s->kick+20,cleanups);
    for (unsigned i=0;i<cleanups;++i) put32(s->kick+24+4*i,cleanup[i]);
    s->count=count; s->total=total; s->index=0; s->payload_offset=0;
    s->writing_payload=0; s->status=0; s->deadline=deadline; ++s->serial;
    s->phase=PVR_CCCB_SPACE; return 1;
}

static enum pvr_cccb_result fail(struct pvr_cccb *s, unsigned error)
{
    s->error=error; s->phase=PVR_CCCB_FAILED;
    (void)pvr_ccb_abort(s->queue); return PVR_CCCB_FAULT;
}
static int clock_now(struct pvr_cccb *s, uint64_t *now)
{
    if (s->ops.now(s->context,now)!=1) { fail(s,PVR_CCCB_IO); return 0; }
    if (*now<s->last_now || *now<s->queue->last_now) {
        fail(s,PVR_CCCB_CLOCK); return 0;
    }
    s->last_now=*now;
    if (*now>=s->deadline) { fail(s,PVR_CCCB_TIMEOUT); return 0; }
    return 1;
}
static int read32(struct pvr_cccb *s, uint32_t off, uint32_t *value)
{
    unsigned char b[4];
    if (s->ops.read(s->context,PVR_CCCB_CONTROL,off,b,4)!=1) return 0;
    *value=get32(b); return 1;
}
static int space(struct pvr_cccb *s)
{
    uint32_t mask, read;
    if (s->ops.barrier(s->context,PVR_CCB_ACQUIRE)!=1 ||
        !read32(s,12,&mask) || !read32(s,4,&read)) { fail(s,PVR_CCCB_IO); return 0; }
    if (mask!=s->size-1 || read>mask || (read&7) ||
        ((read-s->read)&mask)>((s->write-s->read)&mask)) {
        fail(s,PVR_CCCB_CURSOR); return 0;
    }
    s->read=read;
    uint32_t remaining=s->size-s->write, required=s->total;
    if (required+PVR_CCCB_HEADER_BYTES>remaining) required+=remaining;
    if (((read-s->write+mask)&mask)<required) return 1;
    s->cursor=s->write; s->phase=PVR_CCCB_WRITE; return 1;
}
static int write_step(struct pvr_cccb *s)
{
    const struct pvr_cccb_command *cmd=&s->commands[s->index];
    uint32_t bytes;
    const void *data;
    unsigned char header[PVR_CCCB_HEADER_BYTES]={0};
    if (!s->writing_payload) {
        uint32_t remaining=s->size-s->cursor;
        if (remaining<cmd->bytes+2*PVR_CCCB_HEADER_BYTES) {
            put32(header,PVR_CCB_MAGIC|221u);
            put32(header+4,remaining-PVR_CCCB_HEADER_BYTES);
            if (s->ops.write(s->context,PVR_CCCB_COMMANDS,s->cursor,header,sizeof(header))!=1)
                return 0;
            s->cursor=0; return 1;
        }
        put32(header,cmd->type); put32(header+4,cmd->bytes);
        put32(header+8,cmd->external_reference); put32(header+12,cmd->internal_reference);
        data=header; bytes=sizeof(header);
    } else {
        bytes=cmd->bytes-s->payload_offset;
        if (bytes>PVR_CCCB_CHUNK_BYTES) bytes=PVR_CCCB_CHUNK_BYTES;
        data=(const unsigned char *)cmd->payload+s->payload_offset;
    }
    if (s->ops.write(s->context,PVR_CCCB_COMMANDS,s->cursor,data,bytes)!=1) return 0;
    s->cursor+=bytes;
    if (s->writing_payload) s->payload_offset+=bytes;
    else s->writing_payload=1;
    if (s->payload_offset==cmd->bytes) {
        s->writing_payload=0; s->payload_offset=0;
        if (++s->index==s->count) s->phase=PVR_CCCB_PUBLISH;
    }
    return 1;
}

enum pvr_cccb_result pvr_cccb_poll(struct pvr_cccb *s)
{
    if (!s) return PVR_CCCB_INVALID;
    if (s->phase==PVR_CCCB_FAILED) return PVR_CCCB_FAULT;
    if (s->phase==PVR_CCCB_IDLE) return PVR_CCCB_OK;
    if (s->phase==PVR_CCCB_STAGED) return PVR_CCCB_AGAIN;
    if (s->phase<PVR_CCCB_FORMAT || s->phase>PVR_CCCB_WAIT) return PVR_CCCB_INVALID;
    if (s->queue->epoch!=s->epoch ||
        (s->queue->phase!=PVR_CCB_ACTIVE &&
         !(s->phase==PVR_CCCB_FORMAT && s->queue->phase==PVR_CCB_PREPARED)))
        return fail(s,PVR_CCCB_QUEUE);
    uint64_t now;
    if (!clock_now(s,&now)) return PVR_CCCB_FAULT;
    switch (s->phase) {
    case PVR_CCCB_FORMAT: {
        unsigned char control[PVR_CCCB_CONTROL_BYTES]={0};
        put32(control+12,s->size-1);
        if (s->ops.write(s->context,PVR_CCCB_CONTROL,0,control,sizeof(control))!=1 ||
            s->ops.barrier(s->context,PVR_CCB_RELEASE)!=1) return fail(s,PVR_CCCB_IO);
        s->phase=PVR_CCCB_IDLE; break;
    }
    case PVR_CCCB_SPACE:
        if (!space(s)) return PVR_CCCB_FAULT;
        break;
    case PVR_CCCB_WRITE:
        if (!write_step(s)) return fail(s,PVR_CCCB_IO);
        break;
    case PVR_CCCB_PUBLISH:
        if (s->ops.barrier(s->context,PVR_CCB_RELEASE)!=1) return fail(s,PVR_CCCB_IO);
        put32(s->kick+12,s->cursor);
        s->phase=s->paired ? PVR_CCCB_STAGED : PVR_CCCB_SUBMIT; break;
    case PVR_CCCB_SUBMIT: {
        enum pvr_ccb_result r=pvr_ccb_submit(s->queue,s->kick,now,s->deadline,&s->token);
        if (r==PVR_CCB_OK) { s->write=s->cursor; s->phase=PVR_CCCB_WAIT; }
        else if (r!=PVR_CCB_AGAIN) return fail(s,PVR_CCCB_QUEUE);
        break;
    }
    case PVR_CCCB_WAIT: {
        enum pvr_ccb_result r=pvr_ccb_collect(s->queue,s->token,&s->status);
        if (r==PVR_CCB_OK) {
            if (s->status!=1) return fail(s,PVR_CCCB_COMMAND);
            s->phase=PVR_CCCB_IDLE;
        } else if (r!=PVR_CCB_AGAIN) return fail(s,PVR_CCCB_QUEUE);
        break;
    }
    default: return PVR_CCCB_INVALID;
    }
    if (!clock_now(s,&now)) return PVR_CCCB_FAULT;
    return s->phase==PVR_CCCB_IDLE ? PVR_CCCB_OK : PVR_CCCB_AGAIN;
}
enum pvr_cccb_result pvr_cccb_abort(struct pvr_cccb *s)
{
    if (!s || s->phase==PVR_CCCB_NEW) return PVR_CCCB_INVALID;
    if (s->phase==PVR_CCCB_FAILED) return PVR_CCCB_FAULT;
    return fail(s,PVR_CCCB_QUEUE);
}

static int pair_fault(struct pvr_cccb_pair *p)
{
    p->phase = 4;
    pvr_cccb_abort(p->geometry); pvr_cccb_abort(p->fragment);
    return -1;
}
int pvr_cccb_pair_reusable(const struct pvr_cccb_pair *p)
{
    if(!range(p,sizeof(*p)) || p->phase!=3 || p->status!=1 || !p->geometry || !p->fragment)return 0;
    const struct pvr_cccb *g=p->geometry,*f=p->fragment;
    return range(g,sizeof(*g)) && range(f,sizeof(*f)) && g!=f &&
        g->phase==PVR_CCCB_IDLE && f->phase==PVR_CCCB_IDLE && g->status==1 && f->status==1 &&
        g->paired==1 && f->paired==1 && g->serial && g->serial==f->serial && g->serial<UINT64_MAX &&
        g->queue==f->queue && g->queue && g->queue->phase==PVR_CCB_ACTIVE &&
        p->epoch==g->epoch && g->epoch==f->epoch && g->epoch==g->queue->epoch &&
        g->context==f->context && g->ops.now==f->ops.now &&
        g->firmware_context<=UINT32_MAX-152 && g->firmware_context+152==f->firmware_context;
}
int pvr_cccb_pair_begin(struct pvr_cccb_pair *p, struct pvr_cccb *g, struct pvr_cccb *f,
    const struct pvr_cccb_command *gc, unsigned gn, const struct pvr_cccb_command *fc, unsigned fn,
    const uint32_t *gr, unsigned grn, const uint32_t *fr, unsigned frn, uint64_t deadline)
{
    if (!range(p, sizeof(*p)) || !range(g, sizeof(*g)) || !range(f, sizeof(*f)) ||
        !disjoint(p, sizeof(*p), g, sizeof(*g)) || !disjoint(p, sizeof(*p), f, sizeof(*f)) ||
        !disjoint(g, sizeof(*g), f, sizeof(*f)) ||
        !((!p->phase && !g->serial && !f->serial && !g->paired && !f->paired) ||
          (p->geometry==g && p->fragment==f && pvr_cccb_pair_reusable(p))) ||
        g->queue != f->queue || g->epoch != f->epoch ||
        g->ops.now != f->ops.now || g->context != f->context ||
        g->firmware_context > UINT32_MAX - 152 || g->firmware_context + 152 != f->firmware_context ||
        !begin_valid(g, gc, gn, gr, grn, deadline) || !begin_valid(f, fc, fn, fr, frn, deadline) ||
        !disjoint(p, sizeof(*p), g->queue, sizeof(*g->queue)) ||
        !disjoint(p, sizeof(*p), gc, gn * sizeof(*gc)) ||
        !disjoint(p, sizeof(*p), fc, fn * sizeof(*fc)) ||
        (grn && !disjoint(p, sizeof(*p), gr, grn * sizeof(*gr))) ||
        (frn && !disjoint(p, sizeof(*p), fr, frn * sizeof(*fr)))) return 0;
    for (unsigned i = 0; i < gn; ++i)
        if (gc[i].bytes && (!disjoint(p, sizeof(*p), gc[i].payload, gc[i].bytes) ||
            !outside(f, gc[i].payload, gc[i].bytes))) return 0;
    for (unsigned i = 0; i < fn; ++i)
        if (fc[i].bytes && (!disjoint(p, sizeof(*p), fc[i].payload, fc[i].bytes) ||
            !outside(g, fc[i].payload, fc[i].bytes))) return 0;
    if (!outside(f, gc, gn * sizeof(*gc)) || !outside(g, fc, fn * sizeof(*fc)) ||
        (grn && !outside(f, gr, grn * sizeof(*gr))) ||
        (frn && !outside(g, fr, frn * sizeof(*fr)))) return 0;
    /* All validation precedes admission; these calls perform no callbacks. */
    if (!pvr_cccb_begin(g, gc, gn, gr, grn, deadline)) return 0;
    if (!pvr_cccb_begin(f, fc, fn, fr, frn, deadline)) {
        pvr_cccb_abort(g); pvr_cccb_abort(f); return 0;
    }
    g->paired = f->paired = 1;
    p->geometry = g; p->fragment = f; p->epoch = g->epoch;
    p->deadline = deadline; p->last_now = g->last_now > f->last_now ? g->last_now : f->last_now;
    p->phase = 1; return 1;
}
int pvr_cccb_pair_poll(struct pvr_cccb_pair *p)
{
    if (!p || !p->phase) return -1;
    if (p->phase == 3) return 1;
    if (p->phase == 4) return -1;
    struct pvr_cccb *g = p->geometry, *f = p->fragment;
    struct pvr_ccb *q = g->queue;
    uint64_t now;
    if (q->phase != PVR_CCB_ACTIVE || q->epoch != p->epoch ||
        g->phase == PVR_CCCB_FAILED || f->phase == PVR_CCCB_FAILED ||
        g->ops.now(g->context, &now) != 1 || now < p->last_now ||
        now < g->last_now || now < f->last_now || now < q->last_now || now >= p->deadline)
        return pair_fault(p);
    p->last_now = now;
    if (p->phase == 1) {
        if (g->phase != PVR_CCCB_STAGED) {
            if (pvr_cccb_poll(g) == PVR_CCCB_FAULT) return pair_fault(p);
        } else if (f->phase != PVR_CCCB_STAGED) {
            if (pvr_cccb_poll(f) == PVR_CCCB_FAULT) return pair_fault(p);
        } else {
            memset(p->kick, 0, sizeof(p->kick));
            put32(p->kick, PVR_CCB_MAGIC | 117u);
            memcpy(p->kick + 8, g->kick + 8, 32);
            memcpy(p->kick + 40, f->kick + 8, 32);
            enum pvr_ccb_result r = pvr_ccb_submit(q, p->kick, now, p->deadline, &p->token);
            if (r == PVR_CCB_OK) {
                g->write = g->cursor; f->write = f->cursor; p->phase = 2;
            } else if (r != PVR_CCB_AGAIN) return pair_fault(p);
        }
    } else {
        enum pvr_ccb_result r = pvr_ccb_collect(q, p->token, &p->status);
        if (r == PVR_CCB_OK) {
            if (p->status != 1) return pair_fault(p);
            g->status = f->status = p->status;
            g->phase = f->phase = PVR_CCCB_IDLE; p->phase = 3;
        } else if (r != PVR_CCB_AGAIN) return pair_fault(p);
    }
    if (g->ops.now(g->context, &now) != 1 || now < p->last_now ||
        now < g->last_now || now < f->last_now || now < q->last_now || now >= p->deadline)
        return pair_fault(p);
    p->last_now = now;
    return p->phase == 3 ? 1 : 0;
}
