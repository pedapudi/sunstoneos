/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register sequencing adapted from the published display driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (C) 2023 Allwinner.
 */
#include "a733_display_pll.h"
#include "a733_range.h"
#include <stddef.h>
#include <string.h>

#define FACTORS UINT32_C(0x3f0fff3f)
#define ENABLE UINT32_C(0x00700000)
#define GATES UINT32_C(0x00601000)
#define UPDATE UINT32_C(0x80000000)
#define PHY UINT32_C(0x05509000)
#define HOST UINT32_C(0x05508000)
#define LCD UINT32_C(0x05502000)
#define TOP UINT32_C(0x05500000)
#define CCU UINT32_C(0x02002000)
static const uint16_t offsets[]={0x104,0x108,0x10c,0x124,0x130,0x134,0x138};

static int bounded(uint32_t word)
{
    uint64_t numerator=UINT64_C(24000000)*((word>>8)&255);
    uint32_t p=((word>>16)&15)+1,m0=((word>>4)&3)+1,m1=(word&15)+1;
    uint32_t m2=((word>>28)&3)+1,m3=((word>>24)&15)+1;
    return numerator>=UINT64_C(1260000000)*p && numerator<=UINT64_C(2400000000)*p &&
        numerator<=UINT64_C(1000000000)*p*m0*m1 &&
        numerator<=UINT64_C(1200000000)*p*m2*m3;
}
int a733_display_pll_plan(const struct a733_display_pll_profile *initial,
    uint32_t hs,uint32_t ls,struct a733_display_pll_plan *out)
{
    struct a733_display_pll_plan p={0};
    if (!initial || !out ||
        !a733_extents_disjoint_checked((uintptr_t)initial,sizeof(*initial),(uintptr_t)out,sizeof(*out)) ||
        !initial->id || !initial->generation ||
        !a733_dsi_pll_plan(initial->hs_hz,initial->ls_hz,&p.initial) ||
        !a733_dsi_pll_plan(hs,ls,&p.target)) return 0;
    uint32_t a=initial->words[0],b=initial->words[1];
    if ((a&FACTORS)!=p.initial.factors || (a&0x40) ||
        !(((a&ENABLE)==0 && (b&GATES)==0 && !(a&UPDATE)) ||
          ((a&ENABLE)==ENABLE && (b&GATES)==GATES)) ||
        (initial->words[2]&0xe0000000) || (initial->words[4]&0x80000000) ||
        (initial->words[5]&0x20000) || (initial->words[6]&0x80000000)) return 0;
    static const struct a733_display_pll_step sequence[A733_DISPLAY_PLL_STEPS]={
        {0,0x400000,0x400000}, {1,0x400000,0x400000}, {1,0x200000,0x200000},
        {0,0x100000,0x100000}, {0,0x200000,0x200000}, {1,0x1000,0x1000},
        {0,UPDATE,UPDATE}, {0,0xff00,0}, {0,0x30,0}, {0,0xf,0},
        {0,0x30000000,0}, {0,0x0f000000,0}, {0,0x000f0000,0}, {0,UPDATE,UPDATE}
    };
    memcpy(p.steps,sequence,sizeof(sequence));
    for (unsigned i=7;i<13;++i) p.steps[i].value=p.target.factors&p.steps[i].mask;
    for (unsigned i=0;i<A733_DISPLAY_PLL_STEPS;++i) if (!p.steps[i].index) {
        a=(a&~p.steps[i].mask)|p.steps[i].value;
        if (!bounded(a)) return 0;
    }
    if ((a&FACTORS)!=p.target.factors) return 0;
    *out=p;return 1;
}

int a733_display_pll_init(struct a733_display_pll *p,const struct a733_display_pll_config *c)
{
    struct a733_display_pll_plan plan;
    if (!p || !c || p->initialized || !c->registry ||
        !a733_extents_disjoint_checked((uintptr_t)p,sizeof(*p),(uintptr_t)c,sizeof(*c)) ||
        !a733_extents_disjoint_checked((uintptr_t)p,sizeof(*p),(uintptr_t)c->registry,sizeof(*c->registry)) ||
        !a733_extents_disjoint_checked((uintptr_t)c,sizeof(*c),(uintptr_t)c->registry,sizeof(*c->registry)) ||
        !c->ops.now_us || !c->ops.enter || !c->ops.leave || !c->ops.retain ||
        !c->ops.authorize || !c->ops.read32 || !c->ops.write32 || !c->ops.barrier ||
        !c->deadline || !c->reservation.token.id || !c->reservation.token.owner ||
        !c->reservation.handoff || !c->reservation.inactive_epoch ||
        !c->reservation.reference_generation || !c->reservation.rail_generation ||
        (c->reservation.token.exclusive&(A733_CLOCK_VO0|A733_CLOCK_DISPLL1))!=
            (A733_CLOCK_VO0|A733_CLOCK_DISPLL1) ||
        (c->reservation.token.shared&(A733_CLOCK_OSC|A733_CLOCK_PERI0|A733_CLOCK_FABRIC))!=
            (A733_CLOCK_OSC|A733_CLOCK_PERI0|A733_CLOCK_FABRIC) ||
        !a733_display_pll_plan(&c->initial,c->hs_hz,c->ls_hz,&plan)) return 0;
    memset(p,0,sizeof(*p));p->self=p;p->config=*c;p->plan=plan;
    memcpy(p->expected,c->initial.words,sizeof(p->expected));p->initialized=1;return 1;
}

struct baseline_word {uint32_t address,mask,value;};
static const struct baseline_word baseline[]={
    {CCU+0x1588,0x8700001f,0x82000000}, {CCU+0x158c,0x10001,0x10001},
    {CCU+0x150c,0x10001,0x10001},
    {CCU+0x1500,0x80000000,0}, {CCU+0x1508,0x80000000,0},
    {CCU+0x1510,0x80000000,0}, {CCU+0x1580,0x80000000,0},
    {CCU+0x15c0,0x80000000,0}, {CCU+0x15c4,0x80000000,0},
    {TOP+0xc,0x20,0x20}, {TOP+0x20,0x30000,0},
    {LCD,0x80000000,0}, {LCD+0x40,0x80000000,0}, {LCD+0x44,0xf0000000,0},
    {HOST,1,0}, {HOST+0x10,0x1431,0}, {HOST+4,0xffff,0}, {HOST+0xc,0x100,0},
    {PHY,1,0}, {PHY+4,0x331f1f1f,0}, {PHY+0xe4,3,0},
    {PHY+0x54,0x0fff6f77,0}, {PHY+0x58,0xff077fff,0},
    {PHY+0x5c,0x80000000,0}, {PHY+0x110,0x3f,0},
    {PHY+0x60,UINT32_MAX,0}, {PHY+0x64,UINT32_MAX,0}, {PHY+0x68,UINT32_MAX,0}
};
enum {ENTER,AUTHORIZE,BASELINE,PROFILE,PROGRAM,FILTER,SETTLE,FINAL_BASELINE,
    FINAL_PROFILE,FINAL_AUTHORIZE,FINAL_BARRIER,LEAVE};
static void fail(struct a733_display_pll *p,enum a733_display_pll_error e)
{if (!p->fault) p->error=e;p->fault=1;}
static int timely(struct a733_display_pll *p)
{
    uint64_t n=0;
    if (p->config.ops.now_us(p->config.context,&n)!=1 ||
        !a733_deadline_admit(n,&p->last_now,p->config.deadline)) {fail(p,A733_DISPLAY_PLL_TIME);return 0;}
    return !p->fault;
}
static void delay(struct a733_display_pll *p,unsigned us)
{
    if (!a733_deadline_after(p->last_now,us,&p->wait_until)) fail(p,A733_DISPLAY_PLL_TIME);
}
static void retain(struct a733_display_pll *p)
{
    if (p->held) {p->config.ops.retain(p->config.context);p->held=0;}
    enum a733_clock_result r=a733_clock_retain(p->config.registry,&p->config.reservation.token);
    if (r!=A733_CLOCK_BUSY) p->state=A733_DISPLAY_PLL_FAULT_RETAINED;
}
static int current(struct a733_display_pll *p)
{
    enum a733_clock_result r=a733_clock_token_status(p->config.registry,&p->config.reservation.token);
    if (r==A733_CLOCK_BUSY) return 0;
    if (r!=A733_CLOCK_OK) {fail(p,A733_DISPLAY_PLL_AUTHORITY);return 0;}
    return 1;
}
enum a733_display_pll_state a733_display_pll_poll(struct a733_display_pll *p)
{
    if (!p || !p->initialized || p->self!=p) return A733_DISPLAY_PLL_FAULT_RETAINED;
    if (p->state!=A733_DISPLAY_PLL_PENDING) return p->state;
    if (p->entered) {fail(p,A733_DISPLAY_PLL_REENTRY);return p->state;}
    p->entered=1;
    if (p->fault || !timely(p) || !current(p)) goto out;
    if (p->wait_until && p->last_now<p->wait_until) goto out;
    p->wait_until=0;
    void *ctx=p->config.context;
    uint32_t value=0;
    int ok=1,finishing=0;
    switch (p->stage) {
    case ENTER:
        ok=p->config.ops.enter(ctx,p->config.deadline);
        if (ok==1) {p->held=1;p->stage=AUTHORIZE;}
        else if (ok!=0) {p->held=1;fail(p,A733_DISPLAY_PLL_IO);}
        break;
    case AUTHORIZE: case FINAL_AUTHORIZE:
        ok=p->config.ops.authorize(ctx,&p->config);
        if (ok!=1) fail(p,A733_DISPLAY_PLL_AUTHORITY);
        else {++p->stage;p->index=0;}
        break;
    case BASELINE: case FINAL_BASELINE: {
        const struct baseline_word *b=&baseline[p->index];
        ok=p->config.ops.read32(ctx,b->address,&value);
        if (ok!=1) fail(p,A733_DISPLAY_PLL_IO);
        else if ((value&b->mask)!=b->value) fail(p,A733_DISPLAY_PLL_INITIAL);
        else if (++p->index==sizeof(baseline)/sizeof(*baseline)) {++p->stage;p->index=0;}
        break;
    }
    case PROFILE: case FINAL_PROFILE:
        ok=p->config.ops.read32(ctx,PHY+offsets[p->index],&value);
        if (ok!=1) fail(p,A733_DISPLAY_PLL_IO);
        else if (value!=p->expected[p->index]) fail(p,A733_DISPLAY_PLL_VERIFY);
        else if (++p->index==A733_DISPLAY_PLL_WORDS) {++p->stage;p->index=0;}
        break;
    case PROGRAM: {
        const struct a733_display_pll_step *s=&p->plan.steps[p->index];
        uintptr_t a=PHY+offsets[s->index];
        if (p->substep==0 || p->substep==4) {
            ok=p->config.ops.read32(ctx,a,&value);
            if (ok!=1) fail(p,A733_DISPLAY_PLL_IO);
            else if (value!=p->expected[s->index]) fail(p,A733_DISPLAY_PLL_VERIFY);
            else if (p->substep==0) ++p->substep;
            else {
                ++p->index;p->substep=0;
                if (p->index==7 || p->index==A733_DISPLAY_PLL_STEPS) {p->stage=FILTER;p->locks=0;}
                else if (p->index==8) p->substep=5; /* Post-callback20us wait below. */
            }
        } else if (p->substep==1) {
            ok=p->config.ops.authorize(ctx,&p->config);
            if (ok!=1) fail(p,A733_DISPLAY_PLL_AUTHORITY);else ++p->substep;
        } else if (p->substep==2) {
            p->expected[s->index]=(p->expected[s->index]&~s->mask)|s->value;
            ++p->writes_attempted;
            ok=p->config.ops.write32(ctx,a,p->expected[s->index]);
            if (ok!=1) fail(p,A733_DISPLAY_PLL_IO);else ++p->substep;
        } else {
            ok=p->config.ops.barrier(ctx);
            if (ok!=1) fail(p,A733_DISPLAY_PLL_IO);else ++p->substep;
        }
        break;
    }
    case FILTER:
        ok=p->config.ops.read32(ctx,PHY+0xe0,&value);
        if (ok!=1) fail(p,A733_DISPLAY_PLL_IO);
        else {
            p->locks=(value&0x04000000)?p->locks+1:0;
            if (p->locks==3) p->stage=SETTLE;
        }
        break;
    case SETTLE:
        p->stage=p->index==7?PROGRAM:FINAL_BASELINE;
        if (p->stage==FINAL_BASELINE) p->index=0;
        break;
    case FINAL_BARRIER:
        ok=p->config.ops.barrier(ctx);
        if (ok!=1) fail(p,A733_DISPLAY_PLL_IO);else p->stage=LEAVE;
        break;
    case LEAVE:
        p->config.ops.leave(ctx);p->held=0;
        if (timely(p)) finishing=1;
        break;
    default:fail(p,A733_DISPLAY_PLL_IO);break;
    }
    if (timely(p)) {
        if (p->stage==FILTER || p->stage==SETTLE) delay(p,p->stage==SETTLE?25:5);
        else if (p->stage==PROGRAM && p->substep==5) {p->substep=0;delay(p,20);}
        if (finishing) p->state=A733_DISPLAY_PLL_READY_RETAINED;
    }
out:
    if (p->fault) retain(p);
    p->entered=0;return p->state;
}
int a733_display_pll_ready(const struct a733_display_pll *p)
{
    return p && p->self==p && p->initialized && !p->entered && !p->held && !p->fault &&
        p->state==A733_DISPLAY_PLL_READY_RETAINED;
}
