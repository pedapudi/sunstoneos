/* SPDX-License-Identifier: Apache-2.0 */
/* Include with PMIC_MODEL_NO_MAIN for actual-provider consumer composition. */
#include "a733_pmic.h"
#include "a733_pmic_native.h"
#include "a733_pins_native.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct pmic_model_word { uintptr_t address; uint32_t value; };
struct pmic_model {
    struct a733_pmic provider;
    struct a733_clock_leases clocks;
    struct a733_rail_leases rails;
    struct a733_gpu_resource_manager gpu;
    struct a733_pins pins;
    struct a733_pmic_lifetime lifetime;
    struct pmic_model_word words[256];unsigned word_count;
    uint64_t now;
    /* Optional bus timing: now advances by now_step per call, and each wire
     * event sets its flag wire_us after the write. Zero keeps instant flags. */
    uint64_t now_step,wire_us,flag_at;uint32_t flag_value;
    /* Optional finer timing: each time read costs now_ns and each register
     * access access_ns nanoseconds instead of now_step, and START, repeated
     * START and STOP complete edge_us after their write instead of wire_us.
     * With now_ns set, a flag sets at flag_ns: the write's completion time
     * in nanoseconds plus its wire time, so its fraction of a microsecond
     * counts. */
    uint64_t now_ns,access_ns,frac_ns,edge_us,flag_ns;
    /* Optional board costs: a write costs write_ns instead of access_ns when
     * set, and each provider poll costs poll_ns at its task check. */
    uint64_t write_ns,poll_ns;
    /* Optional STOP behaviour: the control register reads stop_control
     * instead of 0x40 once M_STP clears; after each STOP the status register
     * reads 0x58 until its stop_status_reads-th read, which gives 0xf8; after the k-th
     * STOP of a setup (k < 8) line control reads stop_line[k] when nonzero,
     * else 0x30. */
    uint32_t stop_control,stop_status_reads,stop_line[8],status_late;unsigned stops,status_waits,line_reads;
    /* Optional deadline probe: accesses that start at or after late_after,
     * when it is nonzero. */
    uint64_t late_after;unsigned late;
    /* Optional concurrent consumer: each bus access also calls live(), and
     * concurrent(), when set, models a caller on another core; it also runs
     * at every time read, which includes the service task's spins. */
    unsigned probe_live,live_busy;
    void (*concurrent)(struct pmic_model *);
    unsigned worker,register_held,i2c_held,retained,wire,selected,reads,writes;
    unsigned fail_phase,fail_status,stall_phase,stop_stall;
    /* Optional observer of each PMIC register data write. */
    void (*pmic_written)(void *,uint8_t reg,uint8_t value);void *observer;
    struct a733_rail_token *replace_parent;
    struct a733_rail_token replacement;
    uint8_t pmic[256];
    struct pmic_model_word trace[4096];unsigned trace_count;
};
static int pmic_model_separate(const void *a,size_t n,const void *b,size_t k)
{return (uintptr_t)a<=(uintptr_t)b ? (uintptr_t)b-(uintptr_t)a>=n : (uintptr_t)a-(uintptr_t)b>=k;}
static uint32_t *pmic_model_word(struct pmic_model *m,uintptr_t address)
{
    for(unsigned i=0;i<m->word_count;++i)if(m->words[i].address==address)return &m->words[i].value;
    assert(m->word_count<256);m->words[m->word_count].address=address;
    return &m->words[m->word_count++].value;
}
static int pmic_model_now(void *v,uint64_t *out)
{
    struct pmic_model *m=v;
    if(m->replace_parent){*m->replace_parent=m->replacement;m->replace_parent=0;}
    if(m->concurrent)m->concurrent(m);
    *out=m->now;
    if(m->now_ns){m->frac_ns+=m->now_ns;m->now+=m->frac_ns/1000;m->frac_ns%=1000;}
    else m->now+=m->now_step;
    return 1;
}
static void pmic_model_cost(struct pmic_model *m,uint64_t ns)
{m->frac_ns+=ns;m->now+=m->frac_ns/1000;m->frac_ns%=1000;}
static int pmic_model_current(void *v)
{struct pmic_model *m=v;if(m->poll_ns)pmic_model_cost(m,m->poll_ns);return m->worker==1;}
static int pmic_model_outside(void *v,const void *p,size_t n)
{
    struct pmic_model *m=v;
    /* Model callbacks access these fields; embedded provider/managers are
     * independently excluded by the actual core. */
    return p && pmic_model_separate(p,n,m->words,sizeof(m->words)) &&
        pmic_model_separate(p,n,&m->now,sizeof(*m)-(size_t)((char *)&m->now-(char *)m));
}
static int pmic_model_enter(void *v,uint64_t deadline)
{
    struct pmic_model *m=v;assert(m->worker && !m->register_held);
    if(m->retained || m->i2c_held || m->now>=deadline)return 0;
    m->i2c_held=1;return 1;
}
static void pmic_model_leave(void *v)
{struct pmic_model *m=v;assert(m->worker && m->i2c_held && !m->retained);m->i2c_held=0;}
static void pmic_model_retain(void *v)
{struct pmic_model *m=v;assert(m->worker && m->i2c_held);m->retained=1;}
static void pmic_model_probe(struct pmic_model *m,unsigned write)
{
    if(m->late_after && m->now>=m->late_after)++m->late;
    const uint64_t ns=write && m->write_ns ? m->write_ns : m->access_ns;
    if(ns)pmic_model_cost(m,ns);
    if(m->probe_live && a733_pmic_live(&m->provider,&m->lifetime)==A733_PMIC_BUSY)++m->live_busy;
    if(m->concurrent)m->concurrent(m);
}
static int pmic_model_read(void *v,uintptr_t address,uint32_t *out)
{
    struct pmic_model *m=v;assert(m->worker && m->i2c_held && !m->retained);pmic_model_probe(m,0);
    assert(a733_pmic_native_address(address,0));++m->reads;
    if(address==0x0708300c && m->flag_at && (m->now_ns ? m->now*1000+m->frac_ns>=m->flag_ns : m->now>=m->flag_at)){*pmic_model_word(m,address)=m->flag_value;m->flag_at=0;}
    if(address==0x07083010 && m->status_late){++m->status_waits;if(!--m->status_late)*pmic_model_word(m,address)=0xf8;}
    if(address==0x07083020)++m->line_reads;
    *out=*pmic_model_word(m,address);return 1;
}
static int pmic_model_write(void *v,uintptr_t address,uint32_t value)
{
    struct pmic_model *m=v;assert(m->worker && m->i2c_held && !m->retained);pmic_model_probe(m,1);
    assert(a733_pmic_native_address(address,1) && value<=255);++m->writes;
    assert(m->trace_count<4096);m->trace[m->trace_count++]=(struct pmic_model_word){address,value};
    if(address==0x07083008){*pmic_model_word(m,address)=value;return 1;}
    unsigned data=*pmic_model_word(m,0x07083008),status=0;
    if(value==0x68) {
        assert(m->wire==0 || m->wire==3);status=m->wire==0 ? 8 : 0x10;++m->wire;
    } else if(value==0x48) {
        switch(m->wire) {
        case 1:assert(data==0x6c);status=0x18;break;
        case 2:m->selected=data;status=0x28;break;
        case 3: /* Data byte after the register index: a register write. */
            m->pmic[m->selected]=(uint8_t)data;status=0x28;m->wire=5;
            if(m->pmic_written)m->pmic_written(m->observer,(uint8_t)m->selected,(uint8_t)data);
            break;
        case 4:assert(data==0x6d);status=0x40;break;
        case 5:*pmic_model_word(m,0x07083008)=m->pmic[m->selected];status=0x58;break;
        default:assert(0);
        }
        ++m->wire;
    } else {
        assert(value==0x58 && m->wire==6);
        if(m->stop_stall){*pmic_model_word(m,address)=0x58;return 1;}
        m->wire=0;m->status_late=m->stop_status_reads;*pmic_model_word(m,0x07083010)=m->status_late ? 0x58 : 0xf8;
        *pmic_model_word(m,0x07083020)=m->stops<8 && m->stop_line[m->stops] ? m->stop_line[m->stops] : 0x30;++m->stops;
        const uint32_t idle=m->stop_control ? m->stop_control : 0x40;
        if(m->wire_us){*pmic_model_word(m,address)=0x50;m->flag_at=m->now+(m->edge_us ? m->edge_us : m->wire_us);m->flag_ns=m->flag_at*1000+m->frac_ns;m->flag_value=idle;}
        else *pmic_model_word(m,address)=idle;
        return 1;
    }
    uint32_t done=m->wire==m->stall_phase ? 0x40 : 0x48;
    if(m->wire_us){*pmic_model_word(m,address)=0x40;
        m->flag_at=m->now+(value==0x68 && m->edge_us ? m->edge_us : m->wire_us);m->flag_ns=m->flag_at*1000+m->frac_ns;m->flag_value=done;}
    else *pmic_model_word(m,address)=done;
    *pmic_model_word(m,0x07083010)=m->wire==m->fail_phase ? m->fail_status : status;return 1;
}
static int pmic_model_pin_enter(void *v,uint64_t deadline)
{
    struct pmic_model *m=v;assert(!(m->worker && m->i2c_held));
    if(m->register_held || m->now>=deadline)return 0;m->register_held=1;return 1;
}
static void pmic_model_pin_leave(void *v)
{struct pmic_model *m=v;assert(m->register_held);m->register_held=0;}
static int pmic_model_pin_read(void *v,uintptr_t a,uint32_t *out)
{struct pmic_model *m=v;assert(m->register_held && a733_pins_native_address(a,0));*out=*pmic_model_word(m,a);return 1;}
static int pmic_model_pin_write(void *v,uintptr_t a,uint32_t value)
{struct pmic_model *m=v;assert(m->register_held && a733_pins_native_address(a,1));*pmic_model_word(m,a)=value;return 1;}
static int pmic_model_gpu_read(void *v,uintptr_t a,uint32_t *out)
{(void)v;(void)a;(void)out;assert(0);return 0;}
static int pmic_model_gpu_write(void *v,uintptr_t a,uint32_t r,uint32_t s,uint64_t d)
{(void)v;(void)a;(void)r;(void)s;(void)d;assert(0);return 0;}
static int pmic_model_gpu_rail(void *v,struct a733_gpu_resource_rail *out,uint64_t d)
{(void)v;(void)out;(void)d;assert(0);return 0;}
static void pmic_model_setup(struct pmic_model *m)
{
    memset(m,0,sizeof(*m));m->now=100;
    *pmic_model_word(m,0x07090160)=0x10000002;
    *pmic_model_word(m,0x0701019c)=0x10001;
    *pmic_model_word(m,0x07025000)=0x22;
    *pmic_model_word(m,0x07025014)=0x11;
    *pmic_model_word(m,0x07025024)=5;
    *pmic_model_word(m,0x0708300c)=0x40;
    *pmic_model_word(m,0x07083010)=0xf8;
    *pmic_model_word(m,0x07083014)=0x28;
    *pmic_model_word(m,0x07083020)=0x30;
    *pmic_model_word(m,0x07083200)=0x00f81000;
    *pmic_model_word(m,0x07083210)=0x80c0;
    *pmic_model_word(m,0x07083218)=0x00100010;
    m->pmic[0x0e]=3;m->pmic[0x10]=0xff;m->pmic[0x21]=0xff;
    m->pmic[0x12]=23;m->pmic[0x13]=m->pmic[0x15]=30;m->pmic[0x19]=100;
    m->pmic[0x2e]=m->pmic[0x2f]=13;
    assert(a733_clock_leases_init(&m->clocks,pmic_model_now,m));
    const struct a733_gpu_resource_ops gpu={pmic_model_now,pmic_model_gpu_read,pmic_model_gpu_write,pmic_model_gpu_rail};
    assert(a733_gpu_resources_init(&m->gpu,&gpu,m));
    const struct a733_rail_binding binding={&m->clocks,&m->gpu,900,pmic_model_now,m};
    assert(a733_rail_leases_init(&m->rails,&binding));
    const struct a733_pins_operations pin_ops={pmic_model_pin_enter,pmic_model_pin_leave,
        pmic_model_now,pmic_model_pin_read,pmic_model_pin_write};
    struct a733_pin_handoff handoff={0};
    for(unsigned b=1;b<A733_PIN_BANKS;++b)handoff.authorized[b]=a733_pin_bank_mask(b);
    for(unsigned i=0;i<2;++i)handoff.latch[i]=(struct a733_pin_latch_handoff){100+i,900,a733_pin_bank_mask(A733_PIN_L+i),0};
    assert(a733_pins_init_banks(&m->pins,&pin_ops,m,&handoff));
    const struct a733_pmic_config config={777,888,1000000,{{2,1,1},{2,1,1}}};
    const struct a733_pmic_ops ops={pmic_model_now,pmic_model_current,pmic_model_enter,
        pmic_model_leave,pmic_model_retain,pmic_model_read,pmic_model_write,pmic_model_outside};
    assert(a733_pmic_init(&m->provider,&config,&ops,m,&m->clocks,&m->rails,&m->pins));
    assert(a733_pmic_identity(&m->provider,&m->lifetime)==A733_PMIC_READY);
}
static enum a733_pmic_status pmic_model_step(struct pmic_model *m)
{m->worker=1;enum a733_pmic_status r=a733_pmic_poll(&m->provider);m->worker=0;return r;}
#ifndef PMIC_MODEL_NO_MAIN
static void ready(struct pmic_model *m)
{for(unsigned i=0;i<16 && a733_pmic_live(&m->provider,&m->lifetime)!=A733_PMIC_READY;++i)assert(pmic_model_step(m)!=A733_PMIC_RETAINED);assert(a733_pmic_live(&m->provider,&m->lifetime)==A733_PMIC_READY);}
static struct a733_rail_token rail(struct pmic_model *m,unsigned owner,a733_rail_mask_t mask)
{
    struct a733_rail_token t={0};assert(a733_rail_acquire(&m->rails,owner,mask,0,m->now+1000000,&t)==A733_RAIL_PENDING);
    enum a733_rail_result r=A733_RAIL_PENDING;
    for(unsigned i=0;i<16 && r!=A733_RAIL_READY;++i)r=a733_rail_poll(&m->rails,&t);
    assert(r==A733_RAIL_READY);return t;
}
static void finish(struct pmic_model *m,const struct a733_pmic_request *q,struct a733_pmic_snapshot *out)
{
    for(unsigned i=0;i<150 && a733_pmic_result(&m->provider,q,out)==A733_PMIC_PENDING;++i)
        assert(pmic_model_step(m)!=A733_PMIC_RETAINED);
    assert(a733_pmic_result(&m->provider,q,out)==A733_PMIC_COMPLETE);
}
static struct pmic_model m;
static enum a733_pmic_status run(struct pmic_model *m,const struct a733_pmic_request *q,unsigned steps)
{
    struct a733_pmic_snapshot s;enum a733_pmic_status r=A733_PMIC_PENDING;
    for(unsigned i=0;i<steps && (r=a733_pmic_result(&m->provider,q,&s))==A733_PMIC_PENDING;++i)pmic_model_step(m);
    return a733_pmic_result(&m->provider,q,&s);
}
/* The STOP wait and line control of docs/designs/pmic-access.md: the status
 * reaching 0xf8 late, never, a control value other than 0x40, and a held
 * line after an intermediate and after the final STOP. */
static void stop_cases(void)
{
    struct a733_rail_token t;struct a733_pmic_request q;struct a733_pmic_snapshot s;
    pmic_model_setup(&m);ready(&m);t=rail(&m,10,A733_RAIL_BIT(A733_RAIL_DCDC1));m.stop_status_reads=3;
    assert(a733_pmic_submit(&m.provider,10,&t,A733_RAIL_DCDC1,1000000,&q)==A733_PMIC_PENDING);
    finish(&m,&q,&s);
    assert(s.programmed_uv==3300000 && m.status_waits==6*3 && !m.retained && !m.i2c_held);
    assert(a733_pmic_reap(&m.provider,&q)==A733_PMIC_COMPLETE);
    /* Never 0xf8: pending with no further write until the deadline retains. */
    pmic_model_setup(&m);ready(&m);t=rail(&m,10,A733_RAIL_BIT(A733_RAIL_DCDC1));m.stop_status_reads=UINT32_MAX;
    assert(a733_pmic_submit(&m.provider,10,&t,A733_RAIL_DCDC1,1000000,&q)==A733_PMIC_PENDING);
    assert(run(&m,&q,40)==A733_PMIC_PENDING && m.stops==1 && m.writes==10 && m.status_waits>=20);
    m.now=1000000;assert(pmic_model_step(&m)==A733_PMIC_RETAINED && m.retained && m.i2c_held && m.writes==10);
    assert(a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_RETAINED);
    /* A control value other than 0x40 once M_STP clears is an error state. */
    pmic_model_setup(&m);ready(&m);t=rail(&m,10,A733_RAIL_BIT(A733_RAIL_DCDC1));m.stop_control=0x48;
    assert(a733_pmic_submit(&m.provider,10,&t,A733_RAIL_DCDC1,1000000,&q)==A733_PMIC_PENDING);
    assert(run(&m,&q,40)==A733_PMIC_RETAINED && m.retained && m.stops==1 && m.writes==10);
    /* Line control: read at admission and after the final STOP only. */
    pmic_model_setup(&m);ready(&m);t=rail(&m,10,A733_RAIL_BIT(A733_RAIL_DCDC1));m.stop_line[0]=0x10;
    const unsigned before=m.line_reads;
    assert(a733_pmic_submit(&m.provider,10,&t,A733_RAIL_DCDC1,1000000,&q)==A733_PMIC_PENDING);
    finish(&m,&q,&s);assert(m.line_reads==before+2 && m.stops==6);
    assert(a733_pmic_reap(&m.provider,&q)==A733_PMIC_COMPLETE);
    pmic_model_setup(&m);ready(&m);t=rail(&m,10,A733_RAIL_BIT(A733_RAIL_DCDC1));m.stop_line[5]=0x10;
    assert(a733_pmic_submit(&m.provider,10,&t,A733_RAIL_DCDC1,1000000,&q)==A733_PMIC_PENDING);
    assert(run(&m,&q,60)==A733_PMIC_RETAINED && m.retained && m.stops==6);
}
/* The deadline once per bus phase: each register access costs 1 us and a
 * time read nothing, so a deadline k us after submission falls before the
 * k-th access. A deadline that passes during admission retains the request
 * in the admitting poll, before any transfer write, so a per-tick image does
 * not hold the expired request's I2C mutex until its next poll; one that
 * passes during the transfer retains it after at most three accesses that
 * start at or after the deadline, also inside a poll that advances several
 * phases. */
static void deadline_cases(void)
{
    struct a733_rail_token t;struct a733_pmic_request q;
    pmic_model_setup(&m);ready(&m);t=rail(&m,10,A733_RAIL_BIT(A733_RAIL_DCDC1));
    assert(a733_pmic_submit(&m.provider,10,&t,A733_RAIL_DCDC1,1000000,&q)==A733_PMIC_PENDING);
    const unsigned reads=m.reads;assert(pmic_model_step(&m)==A733_PMIC_PENDING && !m.writes);
    const unsigned admission=m.reads-reads;
    assert(admission>=7);
    unsigned admitted=0,transferred=0,completed=0,worst=0;
    for(unsigned k=1;k<=260;++k) {
        pmic_model_setup(&m);ready(&m);t=rail(&m,10,A733_RAIL_BIT(A733_RAIL_DCDC1));
        m.access_ns=1000;const uint64_t deadline=m.now+k;m.late_after=deadline;
        assert(a733_pmic_submit(&m.provider,10,&t,A733_RAIL_DCDC1,deadline,&q)==A733_PMIC_PENDING);
        if(k<=admission) {
            assert(pmic_model_step(&m)==A733_PMIC_RETAINED && !m.writes && m.late<=admission && m.retained);
            assert(a733_pmic_result(&m.provider,&q,&(struct a733_pmic_snapshot){0})==A733_PMIC_RETAINED);
            ++admitted;continue;
        }
        const enum a733_pmic_status r=run(&m,&q,200);
        if(r==A733_PMIC_COMPLETE){assert(m.now<=deadline+1 && !m.late);++completed;continue;}
        assert(r==A733_PMIC_RETAINED && m.provider.failed);
        assert(m.writes && m.late<=3);++transferred;
        if(m.writes && m.late>worst)worst=m.late;
    }
    assert(admitted==admission && transferred>=150 && completed);
    printf("deadline: %u admission reads; %u deadlines inside admission, %u inside the transfer, at most %u late accesses, %u complete\n",
        admission,admitted,transferred,worst,completed);
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--decode")) {
        const enum a733_rail ids[]={A733_RAIL_DCDC1,A733_RAIL_DCDC2,A733_RAIL_DCDC3,A733_RAIL_DCDC4,A733_RAIL_DCDC5,A733_RAIL_DCDC8,A733_RAIL_BLDO5,A733_RAIL_CLDO1};
        const char *names[]={"dcdc1","dcdc2","dcdc3","dcdc4","dcdc5","dcdc8","bldo5","cldo1"};
        for(unsigned i=0;i<8;++i)for(unsigned code=0;code<256;++code){struct a733_pmic_snapshot s;
            assert(a733_pmic_decode(ids[i],255,code,&s));if(s.decoded)printf("%s %u %u\n",names[i],code,s.programmed_uv/1000);}
        return 0;
    }
    pmic_model_setup(&m);ready(&m);
    const enum a733_rail ids[]={A733_RAIL_DCDC1,A733_RAIL_DCDC2,A733_RAIL_DCDC4,A733_RAIL_DCDC8,A733_RAIL_BLDO5,A733_RAIL_CLDO1};
    const unsigned uv[]={3300000,800000,800000,1800000,1800000,1800000};
    for(unsigned i=0;i<6;++i) {
        struct a733_rail_token t=rail(&m,10+i,A733_RAIL_BIT(ids[i]));struct a733_pmic_request q;
        assert(a733_pmic_submit(&m.provider,t.owner,&t,ids[i],1000000,&q)==A733_PMIC_PENDING);
        assert(a733_rail_release(&m.rails,&t,1000000)!=A733_RAIL_READY);
        struct a733_pmic_snapshot s;finish(&m,&q,&s);
        assert(s.rail==ids[i] && s.enabled && s.decoded && s.programmed_uv==uv[i]);
        assert(!m.i2c_held && s.chip==3 && !s.page_before && !s.page_after && !s.revision);
        assert(a733_pmic_reap(&m.provider,&q)==A733_PMIC_COMPLETE);
        assert(a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_INVALID);
    }
    /* Independent DMA/autonomous engines refuse before pin transition. */
    const uintptr_t dirty[]={0x07083200,0x07083314,0x07083218,0x0340011c};
    const uint32_t bits[]={1,1,0x100,1u<<11};
    for(unsigned i=0;i<4;++i){pmic_model_setup(&m);*pmic_model_word(&m,dirty[i])|=bits[i];
        assert(pmic_model_step(&m)==A733_PMIC_PENDING);assert(pmic_model_step(&m)==A733_PMIC_RETAINED);
        assert(m.retained && !m.provider.pin.id && !m.writes);}
    /* Arbitration loss never causes STOP, reset, retry or result publication. */
    pmic_model_setup(&m);ready(&m);struct a733_rail_token t=rail(&m,10,A733_RAIL_BIT(A733_RAIL_DCDC1));
    struct a733_pmic_request q;struct a733_pmic_snapshot s={.programmed_uv=99};
    m.fail_phase=2;m.fail_status=0x38;
    assert(a733_pmic_submit(&m.provider,10,&t,A733_RAIL_DCDC1,1000000,&q)==A733_PMIC_PENDING);
    for(unsigned i=0;i<20 && !m.provider.failed;++i)pmic_model_step(&m);
    assert(m.provider.failed && m.retained && m.i2c_held);
    for(unsigned i=0;i<m.trace_count;++i)assert(m.trace[i].address!=0x0708300c || m.trace[i].value!=0x58);
    assert(a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_RETAINED && s.programmed_uv==99);
    assert(a733_pmic_reap(&m.provider,&q)==A733_PMIC_RETAINED);
    /* Queued expiry has no bus effects and can be reaped; active expiry cannot. */
    pmic_model_setup(&m);ready(&m);t=rail(&m,10,A733_RAIL_BIT(A733_RAIL_DCDC1));
    assert(a733_pmic_submit(&m.provider,10,&t,A733_RAIL_DCDC1,200,&q)==A733_PMIC_PENDING);
    m.now=200;assert(pmic_model_step(&m)==A733_PMIC_EXPIRED && !m.writes);
    assert(a733_pmic_reap(&m.provider,&q)==A733_PMIC_COMPLETE);
    /* The bus is still busy when the active transfer expires. */
    m.wire_us=1000;
    assert(a733_pmic_submit(&m.provider,10,&t,A733_RAIL_DCDC1,300,&q)==A733_PMIC_PENDING);
    assert(pmic_model_step(&m)==A733_PMIC_PENDING);assert(pmic_model_step(&m)==A733_PMIC_PENDING);
    m.now=300;assert(pmic_model_step(&m)==A733_PMIC_RETAINED && m.retained);
    /* Aliased outputs never corrupt the actual provider/registry/rail state. */
    pmic_model_setup(&m);ready(&m);t=rail(&m,10,A733_RAIL_BIT(A733_RAIL_DCDC1));
    assert(a733_pmic_submit(&m.provider,10,&t,A733_RAIL_DCDC1,1000000,
        (struct a733_pmic_request *)&m.pins)==A733_PMIC_INVALID);
    /* Admission snapshots caller token before any callback can replace it. */
    struct a733_rail_token original=t;
    m.replacement=rail(&m,11,A733_RAIL_BIT(A733_RAIL_DCDC1));m.replace_parent=&t;
    assert(a733_pmic_submit(&m.provider,10,&t,A733_RAIL_DCDC1,1000000,&q)==A733_PMIC_PENDING);
    assert(t.owner==11 && m.provider.slots[0].borrow.parent.owner==10 &&
        m.provider.slots[0].borrow.parent.id==original.id);
    finish(&m,&q,&s);assert(a733_pmic_reap(&m.provider,&q)==A733_PMIC_COMPLETE);
    /* Two owners queue copied requests; wrong owner cannot consume a result. */
    struct a733_pmic_request first,second,wrong;
    assert(a733_pmic_submit(&m.provider,10,&original,A733_RAIL_DCDC1,1000000,&first)==A733_PMIC_PENDING);
    assert(a733_pmic_submit(&m.provider,11,&t,A733_RAIL_DCDC1,1000000,&second)==A733_PMIC_PENDING);
    wrong=first;wrong.owner=11;
    assert(a733_pmic_result(&m.provider,&wrong,&s)==A733_PMIC_INVALID);
    finish(&m,&first,&s);assert(a733_pmic_reap(&m.provider,&first)==A733_PMIC_COMPLETE);
    finish(&m,&second,&s);assert(a733_pmic_reap(&m.provider,&second)==A733_PMIC_COMPLETE);
    /* STOP timeout retains the bus even after all six wire phases succeeded. */
    pmic_model_setup(&m);ready(&m);t=rail(&m,10,A733_RAIL_BIT(A733_RAIL_DCDC1));m.stop_stall=1;
    assert(a733_pmic_submit(&m.provider,10,&t,A733_RAIL_DCDC1,1000000,&q)==A733_PMIC_PENDING);
    for(unsigned i=0;i<24;++i)assert(pmic_model_step(&m)==A733_PMIC_PENDING);
    m.now=1000000;assert(pmic_model_step(&m)==A733_PMIC_RETAINED && m.retained);
    assert(!a733_pmic_native_address(0x07083018,1) && !a733_pmic_native_address(0x07083314,1) &&
        !a733_pmic_native_address(0x07083014,1));
    stop_cases();deadline_cases();
    puts("PASS actual PMIC state machine, shared rail borrows, six snapshots, engine exclusion, arbitration and deadline retention, "
         "late and missing STOP status, STOP control error, line control at the final STOP, deadline per bus phase");
    return 0;
}
#endif
