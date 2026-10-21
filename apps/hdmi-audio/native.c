/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Internal I2S3 configuration adapted from snd_sunxi_i2s.c, Linux
 * 2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (c) 2021, Dby <dby@allwinnertech.com>
 * GNU GPL version 2 or (at your option) any later version. */
#include "application.h"
#include "../hdmi-audio/native.h"
#include "a733_i2s_platform_native.h"
#include "a733_storage_digest.h"
#include "a733_cpu_coherence.h"
#include "aarch64_device_memory.h"
#include "boot.h"
#include <string.h>
#define CCU UINT32_C(0x02002000)
#define I2S UINT32_C(0x02535000)
#define MODULE (CCU+0x1230)
#define BUS (CCU+0x123c)
#define MODULE_MASK UINT32_C(0x8700001f)
#define MODULE_VALUE UINT32_C(0x82000018)
#define BUS_MASK UINT32_C(0x10001)
struct hdmi_audio_native audio;
_Alignas(64) struct hdmi_audio_result hdmi_audio_result;
_Alignas(64) atomic_uint hdmi_audio_result_ready;
static int present(const uint8_t *p)
{unsigned v=0;for(unsigned i=0;i<32;++i)v|=p[i];return v!=0;}
int hdmi_audio_session_valid(const struct hdmi_audio_session *s)
{
    return s&&s->authority&&s->frames&&s->frames<=HDMI_AUDIO_MAX_FRAMES&&
        present(s->pcm_sha256)&&present(s->memory)&&present(s->electrical)&&
        s->dma.controller==0x04601000&&s->dma.evidence&&
        s->dma.established==A733_DMA_HANDOFF_SUPERVISED&&
        s->dma.service_channels==1&&s->dma.owned_channels==0xffff&&
        s->dma.evidence==s->dma.allowed.handoff&&(s->dma.allowed.desired_cpu&1)&&
        s->dma.allowed.owner&&s->dma.allowed.handoff&&!s->dma.allowed.deadline_us;
}
static int current(void)
{return audio.initialized&&!atomic_load_explicit(&audio.failed,memory_order_acquire)&&hdmi_live(NULL)&&
    xTaskGetCurrentTaskHandle()==app.handles[1];}
static int now(void *v,uint64_t *out)
{
    uint64_t t;
    if(v!=&audio||!current()||a733_counter_clock_now(&app.counter,&t)!=1||
       t<audio.last_now||t>=app.stop_at)return 0;
    audio.last_now=t;*out=t;return 1;
}
static int enter(void *v,uint64_t deadline)
{
    uint64_t t;
    if(v!=&audio||deadline!=app.stop_at||audio.held||!now(v,&t))return -1;
    int r=a733_platform_lock_enter(&app.locks,A733_PLATFORM_REGISTER_LOCK,deadline);
    if(r==1)audio.held=1;
    return r;
}
static void leave(void *v)
{
    uint64_t t;
    if(v!=&audio||!audio.held||!now(v,&t)){atomic_store_explicit(&audio.failed,1,memory_order_release);return;}
    a733_platform_lock_leave(&app.locks,A733_PLATFORM_REGISTER_LOCK);audio.held=0;
}
static int parent_read(void *v,uintptr_t a,uint32_t *out)
{
    uint64_t t;return v==&audio&&audio.held&&now(v,&t)&&
        a733_audio_native_read(NULL,a,out)&&now(v,&t);
}
static int parent_write(void *v,uintptr_t a,uint32_t value)
{(void)v;(void)a;(void)value;return 0;} /* adopt-only parent */
static int owns(void)
{
    return current()&&a733_audio_parent_ready(&audio.parent)&&
        audio.local.owner==app.session.audio.authority&&
        audio.local.exclusive==A733_CLOCK_I2S3&&
        audio.local.shared==(A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_AUDIO1|A733_CLOCK_AUDIO5)&&
        a733_clock_token_status(&app.clocks,&audio.local)==A733_CLOCK_OK;
}
static int address(uintptr_t a,unsigned write)
{
    if(a==MODULE||a==BUS)return 1;
    if(a<I2S||a>=I2S+0x80||a&3)return 0;
    switch(a-I2S){
    case 0:case 4:case 8:case 12:case 0x14:case 0x1c:case 0x24:
    case 0x28:case 0x30:case 0x34:case 0x44:case 0x48:return 1;
    case 0x18:return !write;
    default:return 0;
    }
}
static int rd(uintptr_t a,uint32_t *out)
{
    uint64_t t;
    if(!audio.held||!owns()||!address(a,0)||!out||!now(&audio,&t))return 0;
    struct aarch64_translation r;
    if(!aarch64_device_translate(a,0,&r)||r.physical!=a||r.attributes)return 0;
    __asm__ volatile("dsb sy":::"memory");*out=*(volatile uint32_t *)a;
    __asm__ volatile("dsb sy":::"memory");return now(&audio,&t);
}
static int wr(uintptr_t a,uint32_t value)
{
    uint64_t t;
    if(!audio.held||!owns()||!address(a,1)||!now(&audio,&t))return 0;
    struct aarch64_translation w;
    if(!aarch64_device_translate(a,1,&w)||w.physical!=a||w.attributes)return 0;
    __asm__ volatile("dsb sy":::"memory");*(volatile uint32_t *)a=value;
    __asm__ volatile("dsb sy":::"memory");return now(&audio,&t);
}
static int update(uintptr_t a,uint32_t mask,uint32_t value)
{
    uint32_t old,got;
    return rd(a,&old)&&wr(a,(old&~mask)|(value&mask))&&rd(a,&got)&&
        (got&mask)==(value&mask)&&(got&~mask)==(old&~mask);
}
static int parent_memory(void)
{
    if(!audio.pool||!audio.pool->initialized||audio.memory.slot>=DEVICE_MEMORY_MAX_ALLOCATIONS||
       audio.pool->backing.cpu!=0x41c00000||audio.pool->backing.physical!=0x41c00000||
       audio.pool->backing.bytes!=2097152)return 0;
    struct device_memory_allocation *r=&audio.pool->allocations[audio.memory.slot];
    return r->owner==audio.record.owner&&r->generation==audio.record.generation&&
        r->first==audio.record.first&&r->pages==audio.record.pages&&
        r->state==DEVICE_MEMORY_DEVICE&&r->owner==app.session.audio.authority&&
        audio.memory.pool==(uintptr_t)audio.pool&&audio.memory.owner==r->owner&&
        audio.memory.generation==r->generation&&
        audio.pcm_span==((app.session.audio.frames*4+4095)&~UINT64_C(4095))&&
        audio.extent.bytes==4096+audio.pcm_span&&audio.extent.cpu==0x41c00000+(uintptr_t)r->first*4096&&
        audio.extent.physical==audio.extent.cpu&&audio.extent.bytes==(size_t)r->pages*4096;
}
static _Noreturn void dma_fatal(void *v)
{(void)v;hdmi_fail(NULL);__builtin_trap();}
static void worker(void)
{
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(el!=4||!parent_memory()||atomic_load_explicit(&audio.failed,memory_order_acquire)||!hdmi_live(NULL)||
       xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING||
       xTaskGetCurrentTaskHandle()!=audio.service.worker||
       !a733_dma_startup_ready(&audio.startup)||
       atomic_load_explicit(&app.counter.failed,memory_order_acquire)||
       atomic_load_explicit(&app.clocks.failed,memory_order_acquire)||
       atomic_load_explicit(&app.locks.retained,memory_order_acquire))dma_fatal(NULL);
}
static uint32_t dma_read(void *v,uintptr_t a)
{if(v!=&audio)dma_fatal(NULL);worker();return a733_dma_native_operations.read(&audio.dma,a);}
static void dma_write(void *v,uintptr_t a,uint32_t x)
{if(v!=&audio)dma_fatal(NULL);worker();a733_dma_native_operations.write(&audio.dma,a,x);}
/* This concrete consumer uses the already verified Normal-NC pool. The
 * shared DMA memory policy intentionally admits WB RAM only and is unchanged.
 * Keep this exception confined to these two exact permanent allocations. */
static int nc_mapping(const struct a733_dma_mapping *m)
{
    if(!parent_memory()||!m||!m->cpu||!m->bytes||
       (((uintptr_t)m->cpu|m->bus|m->bytes)&63)||
       m->bytes>UINTPTR_MAX-(uintptr_t)m->cpu||m->bus>=(UINT64_C(1)<<35)||
       m->bytes>(UINT64_C(1)<<35)-m->bus)return 0;
    int descriptor=(uintptr_t)m->cpu==audio.extent.cpu&&m->bus==audio.extent.physical&&
        m->bytes==4096&&m->access==A733_DMA_DESCRIPTORS&&
        m->bus<UINT64_C(0xfffff800)&&m->bytes<=UINT64_C(0xfffff800)-m->bus;
    int pcm=(uintptr_t)m->cpu==audio.extent.cpu+4096&&m->bus==audio.extent.physical+4096&&
        m->bytes==audio.pcm_span&&m->access==A733_DMA_READ;
    if(!descriptor&&!pcm)return 0;
    /* PAR_EL1 reports Normal-NC as outer shareable (2), whatever the
     * descriptor says; accept inner (3) as well, like the uncached pool. */
    for(size_t off=0;off<m->bytes;off+=4096){
        struct aarch64_translation t;
        if(!aarch64_device_translate((uintptr_t)m->cpu+off,descriptor,&t)||
           t.physical!=m->bus+off||t.attributes!=0x44||(t.shareability!=2&&t.shareability!=3))return 0;
    }
    return 1;
}
static void dma_cache(void *v,enum a733_dma_cache op,void *p,size_t bytes)
{
    if(v!=&audio)dma_fatal(NULL);worker();
    struct a733_dma_mapping m={p,(uintptr_t)p,bytes,
        (uintptr_t)p==audio.extent.cpu?A733_DMA_DESCRIPTORS:A733_DMA_READ};
    if(op!=A733_DMA_CLEAN||!nc_mapping(&m))dma_fatal(NULL);
    /* There are no cacheable aliases and no writes after immutable handoff.
     * DSB orders Normal-NC PCM/descriptor stores before DMA enable. */
    __asm__ volatile("dsb sy":::"memory");
}
static void dma_barrier(void *v)
{if(v!=&audio)dma_fatal(NULL);worker();a733_dma_native_operations.barrier(&audio.dma);}
static uint64_t dma_ticks(void *v)
{if(v!=&audio)dma_fatal(NULL);worker();return a733_dma_native_operations.ticks(&audio.dma);}
static int dma_mapping(void *v,const struct a733_dma_mapping *m)
{
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(v!=&audio||el!=4)return 0;
    if(xTaskGetSchedulerState()==taskSCHEDULER_RUNNING)worker();
    return nc_mapping(m);
}
static int dma_fifo(void *v,const struct a733_dma_endpoint *p,enum a733_dma_access role)
{
    if(v!=&audio||!parent_memory())return 0;
    if(xTaskGetSchedulerState()==taskSCHEDULER_RUNNING)worker();
    return a733_dma_native_operations.validate_fifo(&audio.dma,p,role);
}
static const struct a733_dma_operations dma_ops={dma_read,dma_write,dma_cache,dma_barrier,dma_ticks,dma_mapping,dma_fifo};
static int prepared(void *v,uintptr_t controller,uint16_t channels)
{return v==&audio&&parent_memory()?a733_dma_startup_prepared(&audio.startup,controller,channels):-1;}
int hdmi_audio_boot(void)
{
    const struct hdmi_audio_session *s=&app.session.audio;
    if(audio.initialized||!hdmi_audio_session_valid(s)||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    uint8_t digest[32];size_t bytes=(size_t)s->frames*4;
    for(uintptr_t a=HDMI_AUDIO_PCM_ADDRESS;a<HDMI_AUDIO_PCM_ADDRESS+bytes;a+=4096){
        struct aarch64_translation t;
        if(!aarch64_device_translate(a,0,&t)||t.physical!=a||t.attributes!=0xff||t.shareability!=3)return 0;
    }
    storage_digest_bytes((const void *)HDMI_AUDIO_PCM_ADDRESS,bytes,digest);
    if(memcmp(digest,s->pcm_sha256,32))return 0;
    audio.pool=a733_memory_uncached_init();audio.pcm_span=(bytes+4095)&~(size_t)4095;
    if(!audio.pool||!device_memory_acquire(audio.pool,s->authority,4096+audio.pcm_span,&audio.memory,&audio.extent))return 0;
    /* Sole boot writer; publish permanent ownership before the worker exists. */
    memset((void *)audio.extent.cpu,0,audio.extent.bytes);
    memcpy((void *)(audio.extent.cpu+4096),(const void *)HDMI_AUDIO_PCM_ADDRESS,bytes);
    struct device_memory_allocation *record=device_memory_lookup(audio.pool,audio.memory);
    if(!record)return 0;record->state=DEVICE_MEMORY_DEVICE;audio.record=*record;
    struct dma_memory_window windows[]={
        {audio.extent.cpu,audio.extent.physical,4096,DMA_MEMORY_ROLE(A733_DMA_DESCRIPTORS)},
        {audio.extent.cpu+4096,audio.extent.physical+4096,audio.pcm_span,DMA_MEMORY_ROLE(A733_DMA_READ)}};
    const struct dma_fifo_port port={I2S+0x20,6,1u<<2,1u<<8,A733_DMA_WRITE};
    if(!a733_dma_native_init(&audio.dma,s->dma.controller,s->dma.controller,windows,2,&port,1,dma_fatal,NULL))return 0;
    struct a733_dma_handoff handoff=s->dma;handoff.allowed.deadline_us=app.stop_at;
    if(!a733_dma_startup_supervised_init(&audio.startup,&handoff,&app.counter,&app.clocks,&app.locks)||
       !a733_dma_startup_bind_task(&audio.startup,app.handles[1]))return 0;
    audio.segment=(struct a733_dma_segment){{audio.extent.physical+4096,0,2,8},{I2S+0x20,6,2,8},(uint32_t)bytes};
    audio.mapping=(struct a733_dma_mapping){(void *)(audio.extent.cpu+4096),audio.extent.physical+4096,audio.pcm_span,A733_DMA_READ};
    audio.job=(struct dma_service_job){.kind=DMA_SERVICE_FIFO,.channel=0,
        .transfer={{(void *)audio.extent.cpu,audio.extent.physical,4096,A733_DMA_DESCRIPTORS},&audio.segment,1,&audio.mapping,1}};
    const struct dma_service_protected protected[]={
        {0x41000000,0x41000000,0xc00000},{0x41e00000,0x41e00000,0x400000}};
    const struct dma_service_activation activation={app.handles[1],prepared,&audio};
    if(!dma_service_init_deferred(&audio.service,&dma_ops,&audio,s->dma.controller,1,240000,
        &audio.job,1,protected,2,4,1,&activation)||!dma_service_bind(&audio.service,0,app.handles[1],1))return 0;
    const struct a733_audio_operations backend={.enter=enter,.leave=leave,.now_us=now,.read=parent_read,.write=parent_write};
    const struct a733_i2s_format format={24576000,48000,0,16,32,2,0};
    if(!a733_audio_leases_init(&audio.leases,&app.clocks,s->authority,&backend,&audio)||
       !a733_audio_parent_init(&audio.parent,&audio.leases.operations,&audio.leases,0,app.stop_at)||
       !a733_i2s_plan(&format,&audio.plan)||!a733_hdmi_audio_init(&audio.sampler,&app.hdmi.controller))return 0;
    hdmi_audio_result=(struct hdmi_audio_result){.magic=HDMI_AUDIO_RESULT_MAGIC,.version=1,.bytes=160,
        .stage=HDMI_AUDIO_PENDING,.frames=s->frames,.source_bytes=bytes,.rate_hz=48000,
        .tmds_hz=app.session.request.phy_reference_hz==26000000?73937500:74250000};
    memcpy(hdmi_audio_result.pcm_sha256,s->pcm_sha256,32);
    storage_digest_bytes(&app.session,sizeof(app.session),hdmi_audio_result.session_sha256);
    audio.initialized=1;return 1;
}
static int publish(unsigned stage)
{
    if(audio.published)return stage==HDMI_AUDIO_INPUT_ENDED_RETAINED?1:-1;
    hdmi_audio_result.stage=stage;
    if(!a733_cpu_coherence_publish((uintptr_t)&hdmi_audio_result,sizeof(hdmi_audio_result),app.deadline*24))return -1;
    atomic_store_explicit(&hdmi_audio_result_ready,1,memory_order_release);
    if(!a733_cpu_coherence_publish((uintptr_t)&hdmi_audio_result_ready,64,app.deadline*24))return -1;
    audio.published=1;return stage==HDMI_AUDIO_INPUT_ENDED_RETAINED?1:-1;
}
void hdmi_audio_retain(void)
{
    /* Uncertain DMA remains owned. No reset, forced completion or reclamation.
     * Do not make further device writes after authority/time failure. */
    atomic_store_explicit(&audio.failed,1,memory_order_release);if(!audio.published)(void)publish(HDMI_AUDIO_FAULT_RETAINED);
}
static int fail(void)
{hdmi_audio_retain();return -1;}
static int request(enum dma_service_operation op,uint64_t timeout,struct dma_service_reply *reply)
{
    if(!audio.ticket){const struct dma_service_request r={.operation=op,.job=0,.timeout=timeout};
        if(!dma_service_send(&audio.service,&r,0,&audio.ticket))return 0;}
    if(!dma_service_collect(&audio.service,audio.ticket,reply,0))return 0;
    audio.ticket=0;return reply->result==DMA_SERVICE_OK?1:-1;
}
static int i2s_prepare(void)
{
    int lock=enter(&audio,app.stop_at);if(lock!=1)return lock;
    int ok=1;uint32_t x,y;
    if(!audio.index){
        ok=rd(MODULE,&x)&&rd(BUS,&y)&&!(x&(1u<<31))&&!(y&BUS_MASK)&&
            update(MODULE,MODULE_MASK,MODULE_VALUE&~(1u<<31))&&update(BUS,1,1);
        if(ok){audio.wait_until=audio.last_now+2;audio.index=1;}
    }else if(audio.index==1){
        if(audio.last_now>=audio.wait_until){
            ok=update(BUS,BUS_MASK,BUS_MASK)&&update(MODULE,MODULE_MASK,MODULE_VALUE);
            if(ok)audio.index=2;
        }
    }else{
        static const struct {unsigned off,mask,value;} fields[]={
            {0,0x00070137,0x00060010},{4,0xffff,0x1f73},{8,0,0},
            {0x24,0x1ff,0x50},{0x30,0xf,1},{0x34,0x00ff000f,0x00110003},
            {0x44,0xffffffff,0},{0x48,0xffffffff,0x10},
            {0x14,0x800ff07f,0x80040075},{0x1c,0xff,0}};
        unsigned i=audio.index-2;
        if(i<sizeof(fields)/sizeof(fields[0])){
            unsigned value=fields[i].value;
            if(i==0)value=audio.plan.ctl;
            if(i==1)value=audio.plan.fmt0;
            if(i==3)value=audio.plan.clkdiv;
            ok=update(I2S+fields[i].off,fields[i].mask,value);if(ok)++audio.index;
        }else if(i==sizeof(fields)/sizeof(fields[0])){
            ok=wr(I2S+0x0c,0x66)&&wr(I2S+0x28,0)&&rd(I2S+0x14,&x)&&wr(I2S+0x14,x|(1u<<25));
            if(ok)++audio.index;
        }else{
            ok=rd(I2S+0x14,&x)&&rd(I2S+0x18,&y);
            if(ok&&!(x&(1u<<25))&&((y>>16)&255)==128){leave(&audio);return atomic_load_explicit(&audio.failed,memory_order_acquire)?-1:1;}
        }
    }
    leave(&audio);return ok&&!atomic_load_explicit(&audio.failed,memory_order_acquire)?0:-1;
}
/* Start enables the controller and FIFO requests with TX still off, so DMA
 * primes the FIFO before transmit() starts the serializer, as the I2S0 stream
 * driver does. */
static int producer(unsigned start)
{
    int lock=enter(&audio,app.stop_at);if(lock!=1)return lock;
    int ok=start?(update(I2S,0x107,0x101)&&update(I2S+0x1c,0x80,0x80)):
        (update(I2S+0x1c,0x80,0)&&update(I2S,0x105,0));
    leave(&audio);return ok&&!atomic_load_explicit(&audio.failed,memory_order_acquire)?1:-1;
}
static unsigned primed; /* FIFO requests enabled; TXEN waits for queued samples. */
static int transmit(void)
{
    int lock=enter(&audio,app.stop_at);if(lock!=1)return lock;
    uint32_t f;int ok=rd(I2S+0x18,&f),started=0;
    if(ok&&((f>>16)&255)<128){ok=update(I2S,4,4);started=ok;}
    leave(&audio);
    if(!ok||atomic_load_explicit(&audio.failed,memory_order_acquire))return -1;
    return started;
}
/* ISTA TXO never occurs with request flow control. TXU also latches once the
 * FIFO drains after the final DMA transfer while TXEN stays set, so it marks
 * a playback underrun only when a later FSTA read finds samples again: after
 * the final transfer nothing refills an empty FIFO. */
static int playback_fault(uint32_t ista,uint32_t fsta)
{return (ista&0x20)||((ista&0x40)&&((fsta>>16)&255)<128);}
static int playback_status(uint32_t *fsta)
{
    int lock=enter(&audio,app.stop_at);if(lock!=1)return lock;
    uint32_t ista;int ok=rd(I2S+0x0c,&ista)&&rd(I2S+0x18,fsta);leave(&audio);
    if(!ok||atomic_load_explicit(&audio.failed,memory_order_acquire)||
       playback_fault(ista,*fsta)||((*fsta>>16)&255)>128)return -1;
    return 1;
}
int hdmi_audio_step(void)
{
    if(audio.published)return atomic_load_explicit(&audio.failed,memory_order_acquire)?-1:1;
    uint64_t t;if(!now(&audio,&t)||!parent_memory())return fail();
    int r;struct dma_service_reply reply;
    switch(audio.stage){
    case 0:
        r=a733_dma_startup_poll(&audio.startup);
        if(r==A733_DMA_PLATFORM_RETAINED)return fail();
        if(r!=A733_DMA_PLATFORM_READY)return 0;
        r=dma_service_activate(&audio.service);if(r==DMA_SERVICE_BUSY)return 0;
        if(r!=DMA_SERVICE_OK)return fail();audio.stage=1;return 0;
    case 1:
        r=a733_audio_parent_poll(&audio.parent);
        if(r==A733_I2S_RESOURCE_RETAINED)return fail();
        if(r!=A733_I2S_RESOURCE_READY)return 0;audio.stage=2;return 0;
    case 2:
        r=a733_clock_acquire(&app.clocks,app.session.audio.authority,A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_AUDIO5,
            A733_CLOCK_I2S3,app.stop_at,&audio.local);
        if(r==A733_CLOCK_BUSY)return 0;if(r!=A733_CLOCK_OK)return fail();audio.stage=3;return 0;
    case 3:
        r=i2s_prepare();if(r<0)return fail();if(!r)return 0;audio.stage=4;return 0;
    case 4:
        r=a733_hdmi_audio_prepare(&audio.sampler);if(r<0)return fail();if(!r)return 0;audio.stage=5;return 0;
    case 5:
        r=request(DMA_SERVICE_OPEN,0,&reply);if(r<0)return fail();if(!r)return 0;
        if(reply.phase!=DMA_SERVICE_READY||!reply.owns_session)return fail();audio.stage=6;return 0;
    case 6:
        r=request(DMA_SERVICE_SUBMIT,(app.session.audio.frames*24000000/48000)+24000000,&reply);
        if(r<0)return fail();if(!r)return 0;
        if(reply.phase!=DMA_SERVICE_RUNNING||!reply.holds_buffers)return fail();audio.stage=7;return 0;
    case 7:
        if(!primed){r=producer(1);if(r<0)return fail();if(!r)return 0;primed=1;}
        r=transmit();if(r<0)return fail();if(!r)return 0;
        if(a733_hdmi_audio_mute(&audio.sampler,0)!=1)return fail();
        hdmi_audio_result.stage=HDMI_AUDIO_RUNNING;audio.stage=8;return 0;
    case 8:{
        uint32_t f;r=playback_status(&f);if(r<0)return fail();
        r=request(DMA_SERVICE_STATUS,0,&reply);if(r<0)return fail();if(!r)return 0;
        if(reply.phase==DMA_SERVICE_RUNNING||reply.phase==DMA_SERVICE_DRAINING)return 0;
        hdmi_audio_result.dma_result=reply.outcome;
        if(reply.phase!=DMA_SERVICE_RESULT||reply.outcome!=DMA_SERVICE_COMPLETE||!reply.holds_buffers)return fail();
        hdmi_audio_result.source_consumed_bytes=hdmi_audio_result.source_bytes;audio.stage=9;return 0;}
    case 9:{
        uint32_t f;r=playback_status(&f);if(r<0)return fail();if(!r)return 0;
        if(((f>>16)&255)!=128)return 0;
        hdmi_audio_result.fifo_empty=1;audio.stage=10;return 0;}
    case 10:
        if(a733_hdmi_audio_mute(&audio.sampler,1)!=1)return fail();
        r=producer(0);if(r<0)return fail();if(!r)return 0;
        /* The FIFO observation is not a serializer fence; stopping may truncate
         * an in-flight sample. Keep every buffer and hardware lease reserved. */
        return publish(HDMI_AUDIO_INPUT_ENDED_RETAINED);
    default:return fail();
    }
}
