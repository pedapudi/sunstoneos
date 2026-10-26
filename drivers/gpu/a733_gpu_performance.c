/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_gpu_platform.h"
#include "a733_clock_reference.h"
#include "a733_gpu_opp.h"
#include "a733_range.h"

enum { BEGIN=1, SUBMIT, RESULT, SETTLE, SAMPLE, RATE_WRITE, RATE_WAIT, DONE };
static int fault(struct a733_gpu_platform *p)
{p->phase=A733_GPU_PLATFORM_FAILED;return -1;}
static int timely(struct a733_gpu_platform *p,uint64_t deadline)
{
    uint64_t t;
    if(!p->ops.now(p->context,&t) || t<p->last_now || t<p->perf.last_now || t>=deadline)return fault(p);
    p->last_now=p->perf.last_now=t;return 1;
}
static int read(struct a733_gpu_platform *p,uintptr_t address,uint32_t *v)
{return p->manager->ops.read32(p->manager->context,address,v);}
static int parent(struct a733_gpu_platform *p)
{
    uint32_t osc,ref,pll,sdm0,sdm1,gate,status;
    const uint32_t branches=(1u<<11)|(1u<<9),both=branches|(branches<<16);
    return read(p,0x07090160,&osc) && read(p,0x02002000,&ref) && read(p,0x020020a0,&pll) &&
        read(p,0x020020a8,&sdm0) && read(p,0x020020ac,&sdm1) && read(p,0x02003908,&gate) &&
        read(p,0x02003988,&status) && a733_clock_ref24_valid(osc,ref) && a733_clock_peri0_valid(pll) &&
        !(sdm0&0x80000000) && !(sdm1&0x08000000) && (gate&both)==both &&
        (status&(branches<<16))==(branches<<16);
}
int a733_gpu_platform_performance_enable(struct a733_gpu_platform *p,uint32_t hz)
{
    if(!p || !p->initialized || p->phase!=A733_GPU_PLATFORM_NEW || !p->pmic_native || p->perf.enabled ||
        (hz!=A733_GPU_CORE_HZ && hz!=A733_GPU_LOW_HZ) ||
        !a733_pmic_consumer_gpu_performance(&p->pmic))return 0;
    p->perf.enabled=1;p->perf.selected_hz=hz;return 1;
}
int a733_gpu_platform_performance_parent(struct a733_gpu_platform *p,uint64_t deadline)
{
    if(timely(p,deadline)<0)return -1;
    struct a733_clock_leases *clocks=p->pmic.config.provider->clocks;
    enum a733_clock_result r=p->perf.parent.id ? a733_clock_token_status(clocks,&p->perf.parent) :
        a733_clock_acquire(clocks,p->pmic.config.owner,A733_CLOCK_PERI0,0,deadline,&p->perf.parent);
    if(r==A733_CLOCK_BUSY)return 0;
    if(r!=A733_CLOCK_OK || !parent(p) || timely(p,deadline)<0)return fault(p);
    return 1;
}
static int clock_rate(struct a733_gpu_platform *p,uint32_t *hz)
{
    uint32_t pll,clock;
    if(!p->ops.read(p->context,A733_GPU_CCU,0xe0,&pll) ||
        !p->ops.read(p->context,A733_GPU_CCU,0xb20,&clock) ||
        (pll&0xf870ff02u)!=0xf8105300u || (clock&0x8800000fu)!=0x80000000u)return 0;
    const unsigned source=(clock>>24)&7;
    if(source!=0 && source!=2)return 0;
    *hz=source==2 ? A733_GPU_LOW_HZ : A733_GPU_CORE_HZ;return 1;
}
int a733_gpu_platform_performance_begin(struct a733_gpu_platform *p,enum a733_perf_action action,
    uint32_t value,uint64_t deadline)
{
    if(!p || !p->perf.enabled || p->phase!=A733_GPU_PLATFORM_ON || !p->token || !p->delegated ||
        (p->perf.stage && p->perf.stage!=DONE) ||
        (action!=A733_PERF_READ && action!=A733_PERF_RATE && action!=A733_PERF_VOLTAGE) ||
        (action==A733_PERF_RATE && value!=600 && value!=1008) ||
        (action==A733_PERF_VOLTAGE && value!=800 && value!=960) || timely(p,deadline)<0)return 0;
    p->perf.action=action;p->perf.value=value;p->perf.deadline=deadline;p->perf.stage=BEGIN;
    return 1;
}
int a733_gpu_platform_performance_poll(struct a733_gpu_platform *p)
{
    if(!p || !p->perf.enabled || p->phase!=A733_GPU_PLATFORM_ON || !p->perf.stage)return -1;
    if(p->perf.stage==DONE)return 1;
    if(timely(p,p->perf.deadline)<0)return -1;
    struct a733_pmic *provider=p->pmic.config.provider;
    const uint64_t deadline=p->perf.deadline;
    switch(p->perf.stage) {
    case BEGIN: {
        int r=a733_gpu_platform_performance_parent(p,deadline);if(r!=1)return r;
        const struct a733_gpu_resource_lease lease={p->token,A733_GPU_RESOURCE_GPU,A733_GPU_RESOURCE_STABLE};
        enum a733_gpu_resource_result held=a733_gpu_resources_status(p->manager,p->client,&lease);
        if(held==A733_GPU_RESOURCE_BUSY)return 0;
        uint32_t hz;
        if(held!=A733_GPU_RESOURCE_OK || !clock_rate(p,&hz) || hz!=p->perf.current_hz)return fault(p);
        if(p->perf.action==A733_PERF_RATE) {
            if(p->perf.value==1008 && p->perf.current_uv!=A733_GPU_SUPPLY_UV)return fault(p);
            p->perf.stage=RATE_WRITE;return 0;
        }
        if(p->perf.action==A733_PERF_VOLTAGE && p->perf.value==800 && hz!=A733_GPU_LOW_HZ)return fault(p);
        const uint32_t uv=p->perf.action==A733_PERF_READ ? p->perf.current_uv : p->perf.value*1000;
        r=a733_pmic_consumer_gpu_begin(&p->pmic,deadline,uv);if(r<0)return fault(p);if(!r)return 0;
        p->perf.stage=p->perf.action==A733_PERF_READ || uv==p->perf.current_uv ? SAMPLE : SUBMIT;
        return 0;
    }
    case SUBMIT: {
        uint32_t hz;
        /* A yielded sample/reap cannot authorize lowering from a stale mux. */
        if(!clock_rate(p,&hz) || hz!=p->perf.current_hz ||
            (p->perf.value==800 && hz!=A733_GPU_LOW_HZ))return fault(p);
        enum a733_pmic_status r=a733_pmic_submit_write(provider,p->pmic.config.owner,&p->pmic.rail,
            A733_RAIL_DCDC4,(uint8_t)((p->perf.current_uv-500000)/10000),
            (uint8_t)((p->perf.value*1000-500000)/10000),deadline,&p->supply_request);
        if(r==A733_PMIC_BUSY || (r==A733_PMIC_PENDING && !p->supply_request.id))return 0;
        if(r!=A733_PMIC_PENDING)return fault(p);
        p->perf.stage=RESULT;return 0;
    }
    case RESULT: {
        struct a733_pmic_snapshot shot;
        enum a733_pmic_status r=a733_pmic_result(provider,&p->supply_request,&shot);
        if(r==A733_PMIC_BUSY || r==A733_PMIC_PENDING)return 0;
        if(r!=A733_PMIC_COMPLETE || shot.rail!=A733_RAIL_DCDC4 || !shot.enabled || !shot.decoded ||
            shot.programmed_uv!=p->perf.value*1000)return fault(p);
        r=a733_pmic_reap(provider,&p->supply_request);
        if(r==A733_PMIC_BUSY)return 0;
        if(r!=A733_PMIC_COMPLETE)return fault(p);
        p->supply_request=(struct a733_pmic_request){0};
        if(!a733_deadline_after(p->perf.last_now,A733_GPU_SUPPLY_SETTLE_US,&p->perf.settled) ||
            p->perf.settled>=deadline)return fault(p);
        p->perf.stage=SETTLE;return 0;
    }
    case SETTLE:
        if(p->perf.last_now<=p->perf.settled)return 0;
        p->perf.stage=SAMPLE;return 0;
    case SAMPLE: {
        int r=a733_pmic_consumer_sample(&p->pmic,deadline);if(r<0)return fault(p);if(!r)return 0;
        if(timely(p,deadline)<0)return -1;
        p->perf.current_uv=p->pmic.snapshot.programmed_uv;p->perf.stage=DONE;return 1;
    }
    case RATE_WRITE:
        if(!p->ops.update(p->context,A733_GPU_CCU,0xb20,0x0700000f,
            p->perf.value==600 ? 0x0a000000 : 0x08000000,deadline))return fault(p);
        p->perf.stage=RATE_WAIT;return 0;
    case RATE_WAIT: {
        uint32_t word,hz;
        if(!p->ops.read(p->context,A733_GPU_CCU,0xb20,&word))return fault(p);
        if(word&0x08000000)return 0;
        if(!clock_rate(p,&hz) || hz!=p->perf.value*1000000 || timely(p,deadline)<0)return fault(p);
        p->perf.current_hz=hz;p->perf.stage=DONE;return 1;
    }
    default:return fault(p);
    }
}
int a733_gpu_platform_performance_off(struct a733_gpu_platform *p)
{
    if(timely(p,p->deadline)<0)return -1;
    uint32_t word;
    if(!p->ops.read(p->context,A733_GPU_CCU,0xb20,&word) || (word&0x80000000))return fault(p);
    if(!p->perf.off_normalized) {
        if(word&0x08000000)return 0;
        if(!p->ops.update(p->context,A733_GPU_CCU,0xb20,0x0700000f,0x08000000,p->deadline))return fault(p);
        p->perf.off_normalized=1;return 0;
    }
    if(word&0x08000000)return 0;
    if(word&0x8700000f)return fault(p);
    p->perf.current_hz=0;return 1;
}
