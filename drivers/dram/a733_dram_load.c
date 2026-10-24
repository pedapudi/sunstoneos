/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Counter sequence adapted from __nsi_pmu_timer_store_v2(), nsi_pmu_disable(),
 * nsi_pmu_clear() and nsi_pmu_enable() in sunxi-nsi.c and the latency read in
 * sunxi-nsi-bw.c.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
#include "a733_dram.h"
#include "a733_range.h"
#include <string.h>
#ifndef SUNSTONE_GPU_DISPLAY_DRAM_LOAD
#error "The latency window and copy load belong to the load images only"
#endif
#define PORT A733_NSI_PORT(A733_DRAM_DE_PORT)
#define ENABLE (PORT+0xc0)
#define CLEAR (PORT+0xc4)
#define CYCLE (PORT+0xc8)
#define READS (PORT+0xcc)    /* Then write requests, read data, write data. */
#define LATENCY (PORT+0xdc)  /* Read latency sum. */
#define MIN_DATA 1024u       /* 16 KiB in port 2's 16-byte units. */
int a733_dram_window_address(uintptr_t a)
{return !(a&3)&&a>=ENABLE&&a<=LATENCY;}
static void bump(struct a733_dram_window *w,unsigned i){if(w->count[i]<UINT16_MAX)++w->count[i];}
int a733_dram_window_init(struct a733_dram_window *w,const struct a733_dram_window_ops *o,
    uint64_t counter_hz,uint32_t fabric_hz,uint64_t end,const struct a733_cpu_clock_report *r)
{
    if(!w||w->initialized||!o||!r||!o->io.read||!o->io.write||!o->enter||!o->leave||!o->counter||
       !counter_hz||counter_hz%20||counter_hz>UINT32_MAX||!fabric_hz||fabric_hz%20||fabric_hz>600000000u)return 0;
    *w=(struct a733_dram_window){.ops=*o,.period=counter_hz/20,.phase_a=2*counter_hz,.end=end,
        .counter_hz=counter_hz,.cycles=fabric_hz/20,.fabric_hz=fabric_hz,.initialized=1};
    for(unsigned d=0;d<A733_CPU_DOMAINS;++d){
        uint64_t mhz=r->domain[d].decoded?r->domain[d].hz/1000000:0;
        w->mhz[d]=(uint16_t)(mhz>UINT16_MAX?0:mhz);
    }
    atomic_init(&w->phase,A733_DRAM_IDLE);return 1;
}
static void set_phase(struct a733_dram_window *w,unsigned phase)
{atomic_store_explicit(&w->phase,phase,memory_order_release);}
/* One counter sample; 0 when it is earlier than the latest one. */
static int sample(struct a733_dram_window *w,uint64_t *t)
{*t=w->ops.counter(w->ops.io.context);return a733_monotonic_advance(*t,&w->last);}
static void span(struct a733_dram_window *w,uint64_t ticks)
{if(ticks>w->ticks_max)w->ticks_max=ticks>UINT16_MAX?UINT16_MAX:(uint16_t)ticks;}
/* After a failed access or enter the port's state is unknown. Ending the
 * phases stops the copy tasks, and every later step and abandon returns at
 * once, so nothing calls enter, the I/O pair or the counter again. */
static int fail(struct a733_dram_window *w)
{w->failed=1;w->early=1;set_phase(w,A733_DRAM_ENDED);return -1;}
void a733_dram_window_abandon(struct a733_dram_window *w)
{
    if(!w||!w->initialized)return;
    unsigned phase=atomic_load_explicit(&w->phase,memory_order_relaxed);
    if(phase==A733_DRAM_ENDED)return;
    if(w->in_flight){w->in_flight=0;bump(w,A733_DRAM_ABANDONED);}
    /* The copy load ran until now: record it, so the phase B length is the
     * time the per-core pass counts cover. */
    uint64_t t;
    if(phase==A733_DRAM_PHASE_B&&sample(w,&t))w->b_end=t;
    w->early=1;set_phase(w,A733_DRAM_ENDED);
}
/* Period, disable, clear, then enable with the saved word: six accesses.
 * The accessor's trailing DSB SY completes the enable write before the
 * counter read that gives t1. */
static int start(struct a733_dram_window *w,uint64_t t0)
{
    const struct a733_dram_io *io=&w->ops.io;
    uint32_t enable,clear;
    int r=w->ops.enter(io->context);
    if(r!=1)return r==0?0:fail(w);
    if(io->write(io->context,CYCLE,w->cycles)!=1||io->read(io->context,ENABLE,&enable)!=1||
       io->write(io->context,ENABLE,enable&~1u)!=1||io->read(io->context,CLEAR,&clear)!=1||
       io->write(io->context,CLEAR,clear|1u)!=1||io->write(io->context,ENABLE,enable|1u)!=1)return fail(w);
    int timely=sample(w,&w->t1);
    w->ops.leave(io->context);
    w->enable=enable;w->t0=t0;bump(w,A733_DRAM_STARTED);
    if(!timely){bump(w,A733_DRAM_LATE);return 0;}
    span(w,w->t1-t0);w->in_flight=1;
    return 0;
}
/* t is the step's pending sample. */
static int collect(struct a733_dram_window *w,unsigned phase,uint64_t t)
{
    const struct a733_dram_io *io=&w->ops.io;
    uint32_t v[6];
    uint64_t end;
    int r=w->ops.enter(io->context);
    if(r!=1)return r==0?0:fail(w);
    if(w->fallback&&io->write(io->context,ENABLE,w->enable&~1u)!=1)return fail(w);
    for(unsigned i=0;i<5;++i)if(io->read(io->context,READS+4*i,&v[i])!=1)return fail(w);
    if(io->read(io->context,READS+8,&v[5])!=1)return fail(w);
    int timely=sample(w,&end);
    w->ops.leave(io->context);
    w->in_flight=0;
    if(!timely){bump(w,A733_DRAM_LATE);return 0;}
    span(w,end-t);
    /* v: read requests, write requests, read data, write data, latency sum,
     * read data again. A held counter cannot change between the two reads,
     * so one difference proves the counters run past the period, and every
     * later collect disables them first. */
    if(v[2]!=v[5]){bump(w,A733_DRAM_FREE_RUNNING);w->fallback=1;}
    else if(end-w->t0>=2*w->period)bump(w,A733_DRAM_LATE);
    else if(!v[0]||v[2]<MIN_DATA)bump(w,A733_DRAM_IDLE_REFUSED);
    else{
        uint64_t x=((uint64_t)v[4]<<8)/v[0];
        unsigned b=phase==A733_DRAM_PHASE_B;
        if(x>=UINT64_C(1)<<24)bump(w,A733_DRAM_OUT_OF_RANGE);
        else{w->sum[b]+=x;w->square[b]+=x*x;w->data[b]+=v[2];bump(w,b?A733_DRAM_ACCEPTED_B:A733_DRAM_ACCEPTED_A);}
    }
    return 0;
}
int a733_dram_window_step(struct a733_dram_window *w,unsigned eligible)
{
    if(!w||!w->initialized)return -1;
    unsigned phase=atomic_load_explicit(&w->phase,memory_order_relaxed);
    if(phase==A733_DRAM_ENDED)return 0;
    if(!eligible){
        if(phase==A733_DRAM_IDLE)return 0;
        a733_dram_window_abandon(w);return A733_DRAM_END;
    }
    uint64_t t;
    if(!sample(w,&t)){
        if(w->in_flight){w->in_flight=0;bump(w,A733_DRAM_LATE);}
        return 0;
    }
    if(w->in_flight)return t-w->t1<w->period?0:collect(w,phase,t);
    if(t>=w->end){
        if(phase==A733_DRAM_PHASE_B)w->b_end=t;
        set_phase(w,A733_DRAM_ENDED);return A733_DRAM_END;
    }
    if(phase==A733_DRAM_IDLE){w->phase_start=t;set_phase(w,A733_DRAM_PHASE_A);}
    else if(phase==A733_DRAM_PHASE_A&&t-w->phase_start>=w->phase_a){
        w->b_start=t;set_phase(w,A733_DRAM_PHASE_B);return A733_DRAM_BEGIN_B;
    }
    /* Every accepted window fits the 64-bit sums: x < 2^24 and at most
     * 65535 windows. */
    if(w->count[A733_DRAM_STARTED]==UINT16_MAX)return 0;
    return start(w,t);
}
#if defined(__aarch64__)
void a733_dram_copy_2mib(void *destination,const void *source); /* a733_dram_copy.S */
#define copy a733_dram_copy_2mib
#else
static void copy(void *destination,const void *source){memcpy(destination,source,A733_DRAM_COPY_BYTES);}
#endif
int a733_dram_copy_pass(struct a733_dram_window *w,const atomic_uint *state,
    struct a733_dram_copy_slot *slot,void *destination,const void *source)
{
    unsigned phase=atomic_load_explicit(&w->phase,memory_order_acquire);
    if(phase==A733_DRAM_ENDED||atomic_load_explicit(state,memory_order_acquire)!=1)return 0;
    if(phase!=A733_DRAM_PHASE_B)return -1;
    copy(destination,source);
    unsigned passes=atomic_load_explicit(&slot->passes,memory_order_relaxed);
    atomic_store_explicit(&slot->passes,passes+1,memory_order_release);
    return 1;
}
void a733_dram_window_block(const struct a733_dram_window *w,const struct a733_dram_copy_slot slots[8],
    struct a733_dram_block *b)
{
    b->result|=(uint16_t)((w->fallback?A733_DRAM_DISABLE_FIRST:0)|(w->early?A733_DRAM_ENDED_EARLY:0));
    for(unsigned i=0;i<A733_DRAM_COUNTS;++i)b->count[i]=w->count[i];
    for(unsigned i=0;i<A733_CPU_DOMAINS;++i)b->mhz[i]=w->mhz[i];
    uint64_t ms=w->b_end>w->b_start?(w->b_end-w->b_start)*1000/w->counter_hz:0;
    b->phase_ticks_max=w->ticks_max;
    b->fabric_hz=w->fabric_hz;b->phase_b_ms=ms>UINT32_MAX?UINT32_MAX:(uint32_t)ms;
    for(unsigned i=0;i<2;++i){b->phase[i][0]=w->sum[i];b->phase[i][1]=w->square[i];b->phase[i][2]=w->data[i];}
    for(unsigned i=0;i<8;++i)b->passes[i]=atomic_load_explicit(&slots[i].passes,memory_order_acquire);
}
