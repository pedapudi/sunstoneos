/* SPDX-License-Identifier: Apache-2.0 */
/* High and low performance modes: the per-bin table, the DSU rule, the
 * planner and the switch engine (docs/designs/performance-modes.md). */
#include "a733_perf_mode.h"
#include "a733_clock_reference.h"
#include "a733_cpu_clock.h"
#include <stddef.h>
#define BIT(b) A733_PERF_BIT(A733_PERF_##b)

/* Speed bin vf0400 (SID code 0x04), from linux-orangepi 2ac08e8c:
 * sun60iw2p1-cpu-vf.dtsi V0.9 ("vf", 26 MHz columns for the CPU domains) and
 * sun60iw2p1.dtsi ("soc"); row line, then voltage line.
 *   CPU-L high 1794 MHz at 1000 mV (vf:599, vf:615); low 1196 at 800 (vf:438, vf:454).
 *   CPU-B high 2002 MHz at 1050 mV (vf:1340, vf:1356); low 1404 at 800 (vf:1110, vf:1126).
 *   DSU, by the dsufreq rule: high 1274 MHz at 1000 mV (vf:1989, vf:2005);
 *     low 910 at 800 (vf:1851, vf:1867).
 *   GPU high 1008 MHz at the vfdefault 960 mV that the vendor kernel applies
 *     (soc:4736, soc:4739); low 600 at 800 (soc:4682, soc:4685).
 *   NPU high 852 MHz at 800 mV (vf:2069, vf:2087); low 492 at 800 (vf:2039, vf:2063).
 * checks/perf-mode.py compares every point with the pinned rows. */
static const struct a733_perf_entry a733_perf_modes[]={
    {4,0x0400,{{{1794,1000},{2002,1050},{1274,1000},{1008,960},{852,800}},
               {{1196,800},{1404,800},{910,800},{600,800},{492,800}}}},
};
const struct a733_perf_entry *a733_perf_entries(unsigned *count)
{
    if(count)*count=sizeof(a733_perf_modes)/sizeof(*a733_perf_modes);
    return a733_perf_modes;
}
const struct a733_perf_entry *a733_perf_entry(uint32_t sid_word)
{
    unsigned column;
    if(!a733_cpu_clock_bin(sid_word,&column) || column>=A733_CPU_VF_BINS)return 0;
    for(unsigned i=0;i<sizeof(a733_perf_modes)/sizeof(*a733_perf_modes);++i)if(a733_perf_modes[i].column==column)return &a733_perf_modes[i];
    return 0;
}
static uint32_t row_mv(enum a733_cpu_domain d,uint32_t mhz,unsigned column)
{
    unsigned n;const struct a733_cpu_opp *t=a733_cpu_clock_opps(d,&n);
    for(unsigned i=0;t && i<n;++i)if(t[i].mhz==mhz)return a733_cpu_clock_opp_mv(&t[i],column);
    return 0;
}
/* Three quarters of the faster cluster, rounded down to the 26 MHz step
 * (sunxi-dsufreq.c:284-291). */
static uint32_t dsu_cap(uint32_t l_mhz,uint32_t b_mhz)
{
    uint32_t cap=(l_mhz>b_mhz ? l_mhz : b_mhz)*3u/4u;
    return cap-cap%26u;
}
#define BIG_FAST_MHZ 1488u /* BIG_CORE_THRESHOLD_EXT, sunxi-dsufreq.c:25. */
/* The vendor dsufreq rule (sunxi-dsufreq.c:197-228, 284-295, 314-320,
 * 489-495): the smaller of the cap and the fastest DSU row whose voltage
 * equals that of CPU-L's row, or else the fastest row at the lowest voltage;
 * that row is also the floor while CPU-B runs at 1488 MHz or more. */
int a733_perf_dsu(unsigned column,uint32_t l_mhz,uint32_t b_mhz,struct a733_perf_point *dsu)
{
    const uint32_t l_mv=column<A733_CPU_VF_BINS ? row_mv(A733_CPU_LITTLE,l_mhz,column) : 0;
    if(!l_mv || !dsu)return 0;
    unsigned n;const struct a733_cpu_opp *t=a733_cpu_clock_opps(A733_CPU_DSU,&n);
    uint32_t min_mv=0,by_min=0,by_volt=0;
    for(unsigned i=0;t && i<n;++i) {
        const uint32_t mv=a733_cpu_clock_opp_mv(&t[i],column);
        if(!mv)continue;
        if(!min_mv || mv<min_mv){min_mv=mv;by_min=0;}
        if(mv==min_mv && t[i].mhz>by_min)by_min=t[i].mhz;
        if(mv==l_mv && t[i].mhz>by_volt)by_volt=t[i].mhz;
    }
    if(!by_min)return 0;
    if(!by_volt)by_volt=by_min;
    uint32_t mhz=dsu_cap(l_mhz,b_mhz);
    if(mhz>by_volt)mhz=by_volt;
    if(b_mhz>=BIG_FAST_MHZ && mhz<by_min)mhz=by_min;
    const uint32_t mv=row_mv(A733_CPU_DSU,mhz,column);
    if(!mv)return 0; /* The CPU clock driver admits table rows only. */
    *dsu=(struct a733_perf_point){mhz,mv};return 1;
}
uint32_t a733_perf_pll_mhz(uint32_t oscillator,uint32_t reference,uint32_t pll)
{
    if(!a733_clock_ref24_valid(oscillator,reference) || (pll&UINT32_C(0xf8000000))!=UINT32_C(0xf8000000))return 0;
    const uint32_t n=((pll>>8)&255u)+1u,p=((pll>>20)&7u)+1u,m=((pll>>1)&1u)+1u;
    return 24u*n/(m*p);
}
int a733_perf_gpu_mhz(uint32_t oscillator,uint32_t reference,uint32_t peri0,uint32_t pll,uint32_t clock,
    uint32_t *mhz,unsigned *stopped)
{
    static const uint32_t outputs[6]={0,800,600,400,300,200};
    const unsigned mux=(clock>>24)&7u,field=clock&15u,divider=field==0 ? 1u : field==8 ? 2u : field==12 ? 4u : 0u;
    if(!mhz || !stopped || mux>5 || !divider)return 0;
    if(mux && !(a733_clock_ref24_valid(oscillator,reference) && a733_clock_peri0_valid(peri0)))return 0;
    *mhz=(mux ? outputs[mux] : a733_perf_pll_mhz(oscillator,reference,pll))/divider;
    *stopped=!(clock>>31) || !*mhz;
    return 1;
}
int a733_perf_npu_mhz(uint32_t oscillator,uint32_t reference,uint32_t pll,uint32_t clock,uint32_t *mhz,
    unsigned *stopped)
{
    if(!mhz || !stopped || ((clock>>24)&7u))return 0;
    *mhz=a733_perf_pll_mhz(oscillator,reference,pll)/((clock&31u)+1u);
    *stopped=!(clock>>31) || !*mhz;
    return 1;
}
enum a733_perf_mode a733_perf_mode_of(const struct a733_perf_entry *e,unsigned owned,unsigned read_ok,
    const struct a733_perf_point read[A733_PERF_BLOCKS])
{
    if(!e || !owned || (owned&read_ok)!=owned)return A733_PERF_UNKNOWN;
    unsigned high=1,low=1;
    for(unsigned b=0;b<A733_PERF_BLOCKS;++b) {
        if(!(owned&A733_PERF_BIT(b)))continue;
        const struct a733_perf_point *h=&e->point[0][b],*l=&e->point[1][b];
        high&=read[b].mhz==h->mhz && read[b].mv==h->mv;
        low&=read[b].mhz==l->mhz && read[b].mv==l->mv;
    }
    return high ? A733_PERF_HIGH : low ? A733_PERF_LOW : A733_PERF_MIXED;
}
static int add(struct a733_perf_step *out,int *n,enum a733_perf_action a,enum a733_perf_block b,uint32_t value)
{
    if(*n<0 || *n>=(int)A733_PERF_STEPS || value>UINT16_MAX){*n=-1;return 0;}
    out[(*n)++]=(struct a733_perf_step){(uint8_t)a,(uint8_t)b,(uint16_t)value};
    return 1;
}
int a733_perf_plan(const struct a733_perf_entry *e,unsigned change,const struct a733_perf_point from[A733_PERF_BLOCKS],
    const struct a733_perf_point to[A733_PERF_BLOCKS],struct a733_perf_step out[A733_PERF_STEPS])
{
    int n=0;
    if(!e || !from || !to || !out || (change&~A733_PERF_ALL))return -1;
    /* Every accelerator is held before the first write; each is released
     * after its own change, before the CPU domains change. */
    static const enum a733_perf_block accelerators[]={A733_PERF_GPU,A733_PERF_NPU};
    for(unsigned i=0;i<2;++i)if(change&A733_PERF_BIT(accelerators[i]))add(out,&n,A733_PERF_HOLD,accelerators[i],0);
    for(unsigned i=0;i<2;++i) {
        const enum a733_perf_block b=accelerators[i];
        if(!(change&A733_PERF_BIT(b)))continue;
        const int up=to[b].mhz>from[b].mhz;
        if(up && to[b].mv!=from[b].mv)add(out,&n,A733_PERF_VOLTAGE,b,to[b].mv);
        if(to[b].mhz!=from[b].mhz)add(out,&n,A733_PERF_RATE,b,to[b].mhz);
        if(!up && to[b].mv!=from[b].mv)add(out,&n,A733_PERF_VOLTAGE,b,to[b].mv);
        add(out,&n,A733_PERF_RELEASE,b,0);
    }
    /* CPU domains in the vendor order (cpufreq-dt.c:85-109): the DSU lowers
     * before the clusters change and raises after them. A DSU lowering that
     * its present clusters do not admit, as a THROTTLE lowering without a
     * restore can leave them, waits until the clusters have changed. */
    uint32_t mhz[3]={from[A733_PERF_CPU_L].mhz,from[A733_PERF_CPU_B].mhz,from[A733_PERF_DSU].mhz};
    const uint32_t dsu=to[A733_PERF_DSU].mhz;
    unsigned pending=(change&BIT(DSU)) && dsu!=mhz[2];
    if(pending && dsu<mhz[2] && dsu<=dsu_cap(mhz[0],mhz[1])){add(out,&n,A733_PERF_CHANGE_CPU,A733_PERF_DSU,dsu);pending=0;}
    static const enum a733_perf_block lower[]={A733_PERF_CPU_L,A733_PERF_CPU_B},raise[]={A733_PERF_CPU_B,A733_PERF_CPU_L};
    /* The driver completes a request at the present rate without a write,
     * so a domain whose rate stays is not requested. */
    for(unsigned pass=0;pass<2;++pass)for(unsigned i=0;i<2;++i) {
        const enum a733_perf_block b=pass ? raise[i] : lower[i];
        if(!(change&A733_PERF_BIT(b)) || (pass ? to[b].mhz<=mhz[b] : to[b].mhz>=mhz[b]))continue;
        add(out,&n,A733_PERF_CHANGE_CPU,b,to[b].mhz);mhz[b]=to[b].mhz;
    }
    if(pending) {
        /* The driver refuses a DSU point above the cap of its clusters. A
         * late lowering leaves DCDC5 at the voltage that CPU-L's change kept
         * for the old DSU point, which the readback reports. */
        if(dsu>dsu_cap(mhz[0],mhz[1]))return -1;
        add(out,&n,A733_PERF_CHANGE_CPU,A733_PERF_DSU,dsu);
    }
    return n;
}

enum { IDLE, APPLY, RELEASE, READ };
static int now(struct a733_perf *p,uint64_t *t){return p->ops.now(p->cookie,t)==1;}
int a733_perf_init(struct a733_perf *p,uint32_t sid_word,unsigned owned,const struct a733_perf_ops *ops,void *cookie)
{
    if(!p || !ops || !ops->now || !ops->admit || !ops->act || (owned&~A733_PERF_SELECTABLE))return 0;
    *p=(struct a733_perf){.entry=a733_perf_entry(sid_word),.ops=*ops,.cookie=cookie};
    /* The DSU shares DCDC5 with CPU-L and changes under CPU-L's grant. */
    p->owned=owned|((owned&BIT(CPU_L)) ? BIT(DSU) : 0u);
    p->record.owned=p->owned;
    if(p->entry)for(unsigned b=0;b<A733_PERF_BLOCKS;++b)p->record.target[b]=p->entry->point[0][b];
    return p->entry!=0;
}
static enum a733_perf_reason refuse(struct a733_perf *p,enum a733_perf_reason reason)
{
    p->record.result=A733_PERF_RESULT_REFUSED;p->record.reason=reason;
    uint64_t t;if(now(p,&t))p->record.finished_us=t;
    return reason;
}
enum a733_perf_reason a733_perf_request(struct a733_perf *p,enum a733_perf_command command,
    enum a733_perf_mode mode,unsigned blocks)
{
    if(!p)return A733_PERF_REQUEST;
    if(p->phase!=IDLE || p->retained)return A733_PERF_ACTIVE;
    uint64_t t;
    struct a733_perf_record *r=&p->record;
    r->command=command;r->mode_requested=mode;r->blocks=blocks;r->reason=A733_PERF_OK;
    r->failed_block=A733_PERF_BLOCKS;r->result=A733_PERF_RESULT_RUNNING;r->started_us=r->finished_us=0;
    if(!now(p,&t))return refuse(p,A733_PERF_NOT_READY);
    r->started_us=t;
    p->index=0;p->start=1;p->held=0;p->count=0;p->block=0;
    if(command==A733_PERF_COMMAND_READ && !mode && !blocks){p->phase=READ;return A733_PERF_OK;}
    if(command!=A733_PERF_COMMAND_SWITCH || (mode!=A733_PERF_HIGH && mode!=A733_PERF_LOW) || !blocks ||
       (blocks&~A733_PERF_SELECTABLE))return refuse(p,A733_PERF_REQUEST);
    if(!p->entry)return refuse(p,A733_PERF_NO_BIN);
    if(blocks&~p->owned)return refuse(p,A733_PERF_NOT_OWNED);
    /* Plan from the points that the last command read back. */
    if((r->read_ok&p->owned)!=p->owned)return refuse(p,A733_PERF_NOT_READY);
    struct a733_perf_point to[A733_PERF_BLOCKS];
    for(unsigned b=0;b<A733_PERF_BLOCKS;++b)
        to[b]=(blocks&A733_PERF_BIT(b)) ? p->entry->point[mode-A733_PERF_HIGH][b] : r->read[b];
    unsigned change=blocks;
    if(blocks&A733_PERF_CPU) {
        if(!a733_perf_dsu(p->entry->column,to[A733_PERF_CPU_L].mhz,to[A733_PERF_CPU_B].mhz,&to[A733_PERF_DSU]))
            return refuse(p,A733_PERF_RULE);
        if(!(p->owned&BIT(DSU)))return refuse(p,A733_PERF_NOT_OWNED);
        change|=BIT(DSU);
    }
    /* A CPU domain changes by its rate; the driver sets the voltage. */
    for(unsigned b=0;b<A733_PERF_BLOCKS;++b)
        if(to[b].mhz==r->read[b].mhz && (b<=A733_PERF_DSU || to[b].mv==r->read[b].mv))change&=~A733_PERF_BIT(b);
    const int n=a733_perf_plan(p->entry,change,r->read,to,p->plan);
    if(n<0)return refuse(p,A733_PERF_RULE);
    unsigned cpu=0;
    for(int i=0;i<n;++i)cpu+=p->plan[i].action==A733_PERF_CHANGE_CPU;
    const enum a733_perf_reason admitted=p->ops.admit(p->cookie,change,cpu);
    if(admitted!=A733_PERF_OK)return refuse(p,admitted);
    for(unsigned b=0;b<A733_PERF_BLOCKS;++b)r->target[b]=to[b];
    p->count=(unsigned)n;p->phase=APPLY;
    return A733_PERF_OK;
}
/* After a stopped step: the switch was refused if only holds had run. */
static void stop(struct a733_perf *p,const struct a733_perf_step *s,enum a733_perf_reason reason)
{
    unsigned wrote=0;
    for(unsigned i=0;i<p->index;++i)wrote|=p->plan[i].action!=A733_PERF_HOLD;
    p->record.reason=reason;p->record.failed_block=s->block;
    p->record.result=wrote ? A733_PERF_RESULT_STOPPED : A733_PERF_RESULT_REFUSED;
    p->phase=RELEASE;p->start=1;p->block=0;
}
/* A device-writing hold may fail before DONE. Never run cleanup or another
 * command against uncertainty; the owner retains the actual device resources. */
static void retained(struct a733_perf *p,unsigned block,uint64_t now)
{
    p->retained|=A733_PERF_BIT(block);
    p->record.reason=A733_PERF_STEP_FAILED;p->record.failed_block=block;
    p->record.result=A733_PERF_RESULT_STOPPED;p->record.mode=A733_PERF_UNKNOWN;
    p->record.read_ok=0;p->record.finished_us=now;p->phase=IDLE;
}
enum a733_perf_result a733_perf_step(struct a733_perf *p)
{
    if(!p)return A733_PERF_RESULT_NONE;
    struct a733_perf_record *r=&p->record;
    for(unsigned budget=0;budget<A733_PERF_STEPS && p->phase!=IDLE;++budget) {
        uint64_t t;
        if(!now(p,&t)) {
            /* Loss of the clock cannot turn an incomplete operation into a
             * clean refusal, or allow the next request to reuse its owner. */
            retained(p,p->phase==APPLY && p->index<p->count ? p->plan[p->index].block :
                p->block<A733_PERF_BLOCKS ? p->block : A733_PERF_GPU,p->record.started_us);
            break;
        }
        if(p->phase==APPLY) {
            if(p->index==p->count){p->phase=READ;p->block=0;p->start=1;continue;}
            const struct a733_perf_step *s=&p->plan[p->index];
            if(p->start) {
                /* One deadline covers every hold; the other actions get their own. */
                if(s->action!=A733_PERF_HOLD || !p->index) {
                    const uint64_t span=s->action==A733_PERF_HOLD ? A733_PERF_HOLD_US :
                        s->action==A733_PERF_CHANGE_CPU ? A733_PERF_CPU_US : A733_PERF_ACCEL_US;
                    if(t>UINT64_MAX-span){retained(p,s->block,t);break;}
                    p->deadline=t+span;
                }
                if(s->action==A733_PERF_HOLD)p->held|=A733_PERF_BIT(s->block);
            }
            enum a733_perf_status st=p->ops.act(p->cookie,(enum a733_perf_action)s->action,
                (enum a733_perf_block)s->block,s->value,p->deadline,p->start,0);
            p->start=0;
            if(st==A733_PERF_PENDING) {
                /* The owner must resolve the captured bound. A still-pending
                 * firmware command cannot be classified as a clean refusal. */
                if(t>=p->deadline){retained(p,s->block,t);break;}
                return r->result;
            }
            if(st==A733_PERF_DONE) {
                if(s->action==A733_PERF_HOLD)p->held|=A733_PERF_BIT(s->block);
                if(s->action==A733_PERF_RELEASE)p->held&=~A733_PERF_BIT(s->block);
                ++p->index;p->start=1;continue;
            }
            if(st==A733_PERF_FAILED){retained(p,s->block,t);break;}
            /* REFUSED proves this HOLD acquired no device ownership. A
             * concurrent healthy CLOSE must not be followed by RELEASE of
             * a hold that never existed. */
            if(s->action==A733_PERF_HOLD && st==A733_PERF_REFUSED)
                p->held&=~A733_PERF_BIT(s->block);
            stop(p,s,s->action==A733_PERF_HOLD ? (st==A733_PERF_EXPIRED ? A733_PERF_BUSY : A733_PERF_NOT_READY) : st==A733_PERF_REFUSED ? A733_PERF_STEP_REFUSED :
                st==A733_PERF_EXPIRED ? A733_PERF_STEP_EXPIRED : A733_PERF_STEP_FAILED);
            continue;
        }
        if(p->phase==RELEASE) {
            if(p->block==A733_PERF_BLOCKS){p->phase=READ;p->block=0;p->start=1;continue;}
            if(!(p->held&A733_PERF_BIT(p->block))){++p->block;continue;}
            if(p->start) {
                if(t>UINT64_MAX-A733_PERF_ACCEL_US){retained(p,p->block,t);break;}
                p->deadline=t+A733_PERF_ACCEL_US;
            }
            enum a733_perf_status st=p->ops.act(p->cookie,A733_PERF_RELEASE,
                (enum a733_perf_block)p->block,0,p->deadline,p->start,0);
            p->start=0;
            if(st==A733_PERF_PENDING && t<p->deadline)return r->result;
            if(st!=A733_PERF_DONE){retained(p,p->block,t);break;}
            p->held&=~A733_PERF_BIT(p->block);++p->block;p->start=1;continue;
        }
        /* READ: every block in order, one action at a time. */
        if(p->block==A733_PERF_BLOCKS) {
            r->mode=a733_perf_mode_of(p->entry,p->owned,r->read_ok,r->read);
            if((r->read_ok&p->owned)!=p->owned && r->reason==A733_PERF_OK)r->reason=A733_PERF_READ_FAILED;
            if(r->result==A733_PERF_RESULT_RUNNING)r->result=A733_PERF_RESULT_DONE;
            r->finished_us=t;p->phase=IDLE;break;
        }
        if(p->start) {
            if(t>UINT64_MAX-A733_PERF_READ_US){retained(p,p->block,t);break;}
            p->deadline=t+A733_PERF_READ_US;
        }
        struct a733_perf_point point={0,0};
        enum a733_perf_status st=p->ops.act(p->cookie,A733_PERF_READ,(enum a733_perf_block)p->block,0,p->deadline,p->start,&point);
        p->start=0;
        if(st==A733_PERF_FAILED || (st==A733_PERF_PENDING && t>=p->deadline)) {
            retained(p,p->block,t);break;
        }
        if(st==A733_PERF_PENDING)return r->result;
        if(st==A733_PERF_DONE){r->read[p->block]=point;r->read_ok|=A733_PERF_BIT(p->block);}
        else r->read_ok&=~A733_PERF_BIT(p->block);
        ++p->block;p->start=1;
    }
    return r->result;
}
