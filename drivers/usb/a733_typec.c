/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FUSB302 register operations follow the published controller data sheet and
 * Linux driver. Copyright 2016-2017 Google, Inc. */
#include "a733_typec.h"
#include "a733_range.h"
#include <stddef.h>
#define ADDRESS 0x22u
#define COMP 0x20u
#define VBUSOK 0x80u
#define TRANSACTION_US UINT64_C(5000)
#define DEBOUNCE_US UINT64_C(120000)
#define RISE_US UINT64_C(275000)
#define FALL_US UINT64_C(650000)
static int same(const void *a,const void *b,size_t bytes)
{const unsigned char *x=a,*y=b;for(size_t i=0;i<bytes;i++)if(x[i]!=y[i])return 0;return 1;}
static int intact(struct a733_typec *s)
{return s&&s->self==s&&same(&s->config,&s->sealed,sizeof(s->config));}
void a733_typec_revoke(struct a733_typec *s)
{if(s&&s->self==s)atomic_store_explicit(&s->revoked,1,memory_order_release);}
static int clock_read(struct a733_typec *s)
{uint64_t n;if(s->config.ops.now(s->config.context,&n)!=1||n<s->now)return 0;s->now=n;return 1;}
static void fault(struct a733_typec *s)
{
    a733_typec_revoke(s);
    if(!s->shutdown_attempted){
        s->shutdown_attempted=1;s->state=A733_TYPEC_SHUTDOWN;s->transaction=0;s->step=0;
        if(!a733_deadline_after(s->now,FALL_US,&s->shutdown_deadline))s->shutdown_deadline=UINT64_MAX;
        if(s->config.bus->held||(s->config.bus->state>A733_TWI_READY&&s->config.bus->state<A733_TWI_COMPLETE)){
            (void)a733_twi_retain(s->config.bus);s->bus_failed=1;
        }
        if(s->config.bus->state==A733_TWI_FAULT)s->bus_failed=1;
    }
}
int a733_typec_init(struct a733_typec *s,const struct a733_typec_config *c)
{
    if(!s||s->self||!c||!c->context||!c->bus||!c->owner||!c->generation||
       !c->qualification||!c->recovery||c->source_min_mv<4750||c->source_max_mv>5500||
       c->source_min_mv>c->source_max_mv||c->displayport>1||
       (c->displayport&&c->source_max_mv>5250)||c->source_ma<500||!c->zero_threshold_max_mv||
       c->zero_threshold_max_mv>800||c->device_id<0x80||c->device_id>0x82||
       !c->ops.outside||!c->ops.now||!c->ops.servicing||!c->ops.prepare||!c->ops.live||
       !c->ops.vbus||!c->ops.irq||!a733_ranges_disjoint(s,sizeof(*s),c,sizeof(*c))||
       !a733_ranges_disjoint(s,sizeof(*s),c->bus,sizeof(*c->bus)))return 0;
    s->self=s;s->config=*c;s->sealed=*c;
    atomic_init(&s->entered,0);atomic_init(&s->publishing,0);atomic_init(&s->revoked,0);
    atomic_init(&s->progress_until,0);atomic_init(&s->power_until,0);return 1;
}
int a733_typec_begin(struct a733_typec *s,uint64_t deadline)
{
    if(!intact(s)||s->state!=A733_TYPEC_NEW||s->config.ops.servicing(s->config.context)!=1||
       !deadline||!clock_read(s)||deadline<=s->now)return -1;
    s->startup_deadline=deadline;s->last_service=s->now;s->state=A733_TYPEC_PREPARE;return 0;
}
/* Repeated polls advance this one actual shared-engine transaction. */
static int io(struct a733_typec *s,unsigned reg,unsigned reading,unsigned value)
{
    struct a733_twi *b=s->config.bus;int r;
    if(s->bus_failed)return -1;
    if(!s->transaction){
        uint64_t d;if(!a733_deadline_after(s->now,TRANSACTION_US,&d))return -1;
        if(!s->shutdown_attempted&&s->state<A733_TYPEC_MONITOR&&d>s->startup_deadline)d=s->startup_deadline;
        if(s->shutdown_attempted&&d>s->shutdown_deadline)d=s->shutdown_deadline;
        if(a733_twi_request(b,ADDRESS,(uint16_t)reg,reading,(uint8_t)value,d)!=0)return -1;
        s->transaction=1;s->transaction_deadline=d;return 0;
    }
    r=a733_twi_poll(b);
    if(r<0){s->bus_failed=1;return -1;}
    if(!r)return 0;
    r=a733_twi_take(b,reading?&s->value:NULL);
    if(r!=1){s->bus_failed=1;return -1;}
    s->transaction=0;return 1;
}
static int setting(struct a733_typec *s,unsigned reg,unsigned value,unsigned mask)
{
    int r=io(s,reg,s->substep!=0,value);if(r!=1)return r;
    if(!s->substep){s->substep=1;return 0;}
    s->substep=0;return (s->value&mask)==(value&mask)?1:-1;
}
/* Both CC pull-ups stay present during unattached classification. VBUS measure
 * explicitly disables both CC measurement selectors as the data sheet requires. */
static void measure(struct a733_typec *s,unsigned sw,unsigned word,enum a733_typec_state next)
{s->measure_switch=(uint8_t)sw;s->measure_word=(uint8_t)word;s->measuring=0;s->substep=0;s->after_measure=next;s->state=A733_TYPEC_MEASURE;}
static int measured(struct a733_typec *s)
{
    int r;
    switch(s->measuring){
    case 0:r=setting(s,2,s->measure_switch,255);if(r==1)++s->measuring;return r<0?-1:0;
    case 1:r=setting(s,4,s->measure_word,127);if(r==1){if(!a733_deadline_after(s->now,100,&s->until))return -1;++s->measuring;}return r<0?-1:0;
    case 2:if(s->now>=s->until)++s->measuring;return 0;
    case 3:r=io(s,0x40,1,0);if(r==1){s->status=s->value;s->measure_result=!!(s->value&COMP);s->state=s->after_measure;}return r<0?-1:0;
    default:return -1;
    }
}
static int publish(struct a733_typec *s)
{
    uint64_t d;if(!a733_deadline_after(s->cycle_start,A733_TYPEC_FRESH_US,&d)||s->now>=d)return -1;
    unsigned zero=0;if(!atomic_compare_exchange_strong_explicit(&s->publishing,&zero,1,memory_order_acquire,memory_order_relaxed))return 0;
    int ok=!atomic_load_explicit(&s->revoked,memory_order_acquire);
    if(ok){
        s->published=(struct a733_typec_token){(uintptr_t)s,s->config.owner,s->config.generation,s->cycle_start,s->polarity};
        if(s->pd.ready)s->dp_published=(struct a733_typec_dp_token){(uintptr_t)s,
            s->config.owner,s->config.generation,s->cycle_start,s->pd.hpd_irq,
            s->polarity,s->pd.pin_assignment,s->pd.hpd};
    }
    atomic_store_explicit(&s->publishing,0,memory_order_release);
    if(ok)atomic_store_explicit(&s->power_until,d,memory_order_release);
    return ok?1:-1;
}
static int output(struct a733_typec *s,unsigned on)
{
    uint64_t d;if(!a733_deadline_after(s->now,TRANSACTION_US,&d))return -1;
    int r=s->config.ops.vbus(s->config.context,on,d);
    if(r==1)s->enabled=on;
    return r;
}
static int active(struct a733_typec *s)
{
    int r;
    switch(s->step){
    case 0:{
        uint64_t d;if(!a733_deadline_after(s->now,1000,&d))return -1;
        r=s->config.ops.irq(s->config.context,&s->irq_level,d);
        if(r!=1)return r<0?-1:0;
        if(!s->irq_level)return -1;
        /* RISE cleared the interrupt registers and unmasked only VBUSOK,
         * comparator and OCP/temperature changes. Status reads do not clear
         * them, so INT_N stays low after any such change. While it is high,
         * the last status read still holds: publish from this sample, and
         * read both status registers again once per REFRESH_US. */
        if(s->status_at && s->now-s->status_at<A733_TYPEC_REFRESH_US){
            s->cycle_start=s->now;s->step=3;return 2;
        }
        ++s->step;}
        /* fall through: each completed stage starts the next in this poll. */
    case 1:
        if(!s->transaction)s->cycle_start=s->now;
        r=io(s,0x40,1,0);
        if(r!=1)return r<0?-1:0;
        if((s->value&(VBUSOK|COMP))!=VBUSOK)return -1;
        s->status=s->value;++s->step;
        /* fall through */
    case 2:
        r=io(s,0x41,1,0);
        if(r!=1)return r<0?-1:0;
        if(s->value&3)return -1;
        s->status_at=s->cycle_start;++s->step;
        /* fall through */
    case 3:
        return 2; /* Publish only after final authority and time checks in poll. */
    default:return -1;
    }
}
static int step(struct a733_typec *s)
{
    static const uint8_t setup[][3]={{0x0b,0x0f,0x0f},{2,0,255},{3,0xb0,0xf7},
        {6,0x24,0x2f},{7,0,0x73},{8,6,0xef},{9,0,0x5f},{0xa,255,255},{0xe,255,255},{0xf,1,1}};
    int r;
    switch(s->state){
    case A733_TYPEC_PREPARE:
        r=s->config.ops.prepare(s->config.context,s->startup_deadline);
        if(r==1){s->prepared=1;s->state=A733_TYPEC_OFF;}return r<0?-1:0;
    case A733_TYPEC_OFF:
        r=output(s,0);if(r==1)s->state=A733_TYPEC_ID;return r<0?-1:0;
    case A733_TYPEC_ID:
        r=io(s,1,1,0);if(r==1){if(s->value!=s->config.device_id)return -1;s->state=A733_TYPEC_RESET;}return r<0?-1:0;
    case A733_TYPEC_RESET:
        r=io(s,0xc,s->step!=0,1);
        if(r==1){if(!s->step)++s->step;else if(!(s->value&1)){s->state=A733_TYPEC_SETUP;s->step=0;}}
        return r<0?-1:0;
    case A733_TYPEC_SETUP:
        if(s->step<sizeof(setup)/sizeof(*setup)){
            r=setting(s,setup[s->step][0],setup[s->step][1],setup[s->step][2]);
            if(r==1)++s->step;
            return r<0?-1:0;
        }
        s->step=0;s->state=A733_TYPEC_LOOK;return 0;
    case A733_TYPEC_LOOK:
        if(s->step==0){r=setting(s,8,0x27,0xef);if(r==1)++s->step;return r<0?-1:0;}
        if(s->step==1){
            r=io(s,0x3d,1,0);
            if(r==1){unsigned result=(s->value>>3)&7;if(result==1||result==2)++s->step;else if(result)return -1;}
            return r<0?-1:0;
        }
        r=setting(s,8,6,0xef);
        if(r==1){s->step=0;s->classification_step=0;measure(s,0xc4,38,A733_TYPEC_CLASSIFY);}
        return r<0?-1:0;
    case A733_TYPEC_MEASURE:return measured(s);
    case A733_TYPEC_CLASSIFY:{
        unsigned pin=s->classification_step/2;
        if(!(s->classification_step&1)){
            s->cc[pin]=s->measure_result?0:1;
            ++s->classification_step;measure(s,pin?0xc8:0xc4,4,A733_TYPEC_CLASSIFY);return 0;
        }
        if(s->cc[pin]&&!s->measure_result)s->cc[pin]=2; /* Ra. */
        if(pin==0){++s->classification_step;measure(s,0xc8,38,A733_TYPEC_CLASSIFY);return 0;}
        unsigned candidate=s->cc[0]==1&&s->cc[1]==0?1:s->cc[1]==1&&s->cc[0]==0?2:0;
        /* Ra/cables requiring VCONN, audio/debug accessories remain unsupported. */
        s->candidate=candidate;s->step=0;
        if(!candidate)s->state=A733_TYPEC_LOOK;
        else measure(s,candidate==1?0xc4:0xc8,38,A733_TYPEC_DEBOUNCE);
        return 0;}
    case A733_TYPEC_DEBOUNCE:
        if(s->step==0){
            if(s->measure_result)return -1;
            r=io(s,0x42,1,0); /* Discard only the preceding mux/threshold changes. */
            if(r==1){s->candidate_since=s->now;s->step=1;}return r<0?-1:0;
        }
        if(s->step==1){
            r=io(s,0x40,1,0);
            if(r==1){if(s->value&COMP){s->state=A733_TYPEC_LOOK;s->step=0;}else s->step=2;}
            return r<0?-1:0;
        }
        if(s->step==2){
            r=io(s,0x42,1,0);
            if(r==1){
                /* I_COMP_CHNG is latched even while the interrupt pin is masked. */
                if(s->value&COMP){s->state=A733_TYPEC_LOOK;s->step=0;}
                else if(s->now-s->candidate_since>=DEBOUNCE_US){
                    s->step=3;measure(s,s->candidate==1?0xc8:0xc4,38,A733_TYPEC_DEBOUNCE);
                }else s->step=1;
            }return r<0?-1:0;
        }
        if(s->step!=3)return -1;
        s->step=0;
        if(!s->measure_result){s->state=A733_TYPEC_LOOK;return 0;}
        s->polarity=s->candidate;measure(s,0xc0,0x40,A733_TYPEC_SAFE0);return 0;
    case A733_TYPEC_SAFE0:
        if(s->measure_result||(s->status&VBUSOK))return -1;
        /* Restore attached CC monitoring before enabling VBUS. */
        if(!s->step){s->step=1;measure(s,s->polarity==1?0x44:0x88,38,A733_TYPEC_SAFE0);return 0;}
        if(s->measure_result)return -1;
        s->step=0;s->state=A733_TYPEC_ENABLE;return 0;
    case A733_TYPEC_ENABLE:
        /* Publish the watchdog limit before the separate task can raise PL8. */
        if(!s->step){
            uint64_t progress;
            if(!a733_deadline_after(s->now,RISE_US,&s->until)||!a733_deadline_after(s->now,A733_TYPEC_FRESH_US,&progress))return -1;
            atomic_store_explicit(&s->power_until,s->until,memory_order_release);
            atomic_store_explicit(&s->progress_until,progress,memory_order_release);s->step=1;
        }
        r=output(s,1);
        if(r==1){s->state=A733_TYPEC_RISE;s->step=0;}
        return r<0?-1:0;
    case A733_TYPEC_RISE:
        if(s->now>=s->until)return -1;
        if(s->step==0){
            r=io(s,0x40,1,0);if(r==1){if(s->value&COMP)return -1;if(s->value&VBUSOK)++s->step;}return r<0?-1:0;
        }
        /* Clear observations caused by our own threshold/mux changes, then
         * enable only VBUS/CC and overtemperature/OCP interrupt sources. */
        if(s->step<=3){r=io(s,s->step==1?0x42:s->step==2?0x3e:0x3f,1,0);if(r==1)++s->step;return r<0?-1:0;}
        if(s->step==4){r=setting(s,0xa,0x5f,255);if(r==1)++s->step;return r<0?-1:0;}
        if(s->step==5){r=setting(s,0xe,0x7f,255);if(r==1)++s->step;return r<0?-1:0;}
        if(s->step==6){r=setting(s,6,4,0x2f);if(r==1)++s->step;return r<0?-1:0;}
        s->state=A733_TYPEC_MONITOR;s->step=0;return 0;
    case A733_TYPEC_MONITOR:
        if(s->config.displayport&&s->pd_turn){
            r=a733_typec_pd_service(s);if(r==1)s->pd_turn=0;
            return r<0?-1:0;
        }
        return active(s);
    case A733_TYPEC_SHUTDOWN:
        if(!s->prepared){s->state=A733_TYPEC_UNCERTAIN;return -1;}
        r=output(s,0);
        if(r<0){s->state=A733_TYPEC_UNCERTAIN;return -1;}
        if(r==1){
            if(s->bus_failed||s->config.bus->state!=A733_TWI_READY){s->state=A733_TYPEC_UNCERTAIN;return -1;}
            s->step=0;measure(s,0,0x40,A733_TYPEC_DISCHARGE);
        }return 0;
    case A733_TYPEC_DISCHARGE:
        if(!s->measure_result&&!(s->status&VBUSOK)){s->state=A733_TYPEC_OFF_RETAINED;return 0;}
        if(s->now>=s->shutdown_deadline){s->state=A733_TYPEC_UNCERTAIN;return -1;}
        measure(s,0,0x40,A733_TYPEC_DISCHARGE);return 0;
    case A733_TYPEC_OFF_RETAINED:case A733_TYPEC_UNCERTAIN:return 0;
    default:return -1;
    }
}
int a733_typec_poll(struct a733_typec *s)
{
    if(!intact(s)){a733_typec_revoke(s);return -1;}
    if(s->config.ops.servicing(s->config.context)!=1)return -1;
    unsigned zero=0;if(!atomic_compare_exchange_strong_explicit(&s->entered,&zero,1,memory_order_acquire,memory_order_relaxed)){a733_typec_revoke(s);return -1;}
    int r=0;unsigned oldstep=s->step,oldstage=s->state;enum a733_twi_state oldbus=s->config.bus->state;
    if(!clock_read(s)){fault(s);s->state=A733_TYPEC_UNCERTAIN;goto done;}
    if(!s->shutdown_attempted&&
       (atomic_load_explicit(&s->revoked,memory_order_acquire)||
        (s->state<A733_TYPEC_MONITOR&&s->now>=s->startup_deadline)||
        (s->prepared&&s->now-s->last_service>A733_TYPEC_SERVICE_GAP_US)))fault(s);
    s->last_service=s->now;
    if(s->prepared&&!s->shutdown_attempted){r=s->config.ops.live(s->config.context);if(r<0)fault(s);else if(!r)goto done;}
    r=step(s);
    if(!clock_read(s)||r<0){if(!s->shutdown_attempted)fault(s);else if(s->state!=A733_TYPEC_UNCERTAIN)s->state=A733_TYPEC_UNCERTAIN;}
    if(oldstep!=s->step||oldstage!=(unsigned)s->state||oldbus!=s->config.bus->state){
        uint64_t d;if(a733_deadline_after(s->now,A733_TYPEC_FRESH_US,&d))atomic_store_explicit(&s->progress_until,d,memory_order_release);else fault(s);
    }
    if(r==2&&!s->shutdown_attempted){
        int health=s->config.ops.live(s->config.context);
        if(health<0||!clock_read(s))fault(s);
        else if(health==1){r=publish(s);if(r==1){s->step=0;s->pd_turn=s->config.displayport;}else if(r<0)fault(s);}
        else r=0;
    }
done:
    atomic_store_explicit(&s->entered,0,memory_order_release);
    return atomic_load_explicit(&s->revoked,memory_order_acquire)?-1:r==1?1:0;
}
static int observation(struct a733_typec *s,struct a733_typec_token *out,uint64_t deadline)
{
    if(!intact(s)||atomic_load_explicit(&s->revoked,memory_order_acquire))return -1;
    unsigned zero=0;if(!atomic_compare_exchange_strong_explicit(&s->publishing,&zero,1,memory_order_acquire,memory_order_relaxed))return 0;
    struct a733_typec_token t=s->published;
    atomic_store_explicit(&s->publishing,0,memory_order_release);
    uint64_t now;if(s->config.ops.now(s->config.context,&now)!=1||now>=deadline){a733_typec_revoke(s);return -1;}
    if(!t.provider)return 0;
    if(now<t.observed_us||now-t.observed_us>=A733_TYPEC_FRESH_US||atomic_load_explicit(&s->revoked,memory_order_acquire)){
        a733_typec_revoke(s);return -1;
    }
    *out=t;return 1;
}
int a733_typec_snapshot(struct a733_typec *s,struct a733_typec_token *out,uint64_t deadline)
{
    if(!intact(s)||!out||!a733_ranges_disjoint(s,sizeof(*s),out,sizeof(*out))||
       !s->config.ops.outside(s->config.context,out,sizeof(*out)))return -1;
    struct a733_typec_token t;int r=observation(s,&t,deadline);if(r==1)*out=t;return r;
}
int a733_typec_validate(struct a733_typec *s,const struct a733_typec_token *token,uint64_t deadline)
{
    if(!intact(s)||!token||!a733_ranges_disjoint(s,sizeof(*s),token,sizeof(*token))||
       !s->config.ops.outside(s->config.context,token,sizeof(*token)))return -1;
    struct a733_typec_token t;int r=observation(s,&t,deadline);if(r!=1)return r;
    uint64_t now;if(s->config.ops.now(s->config.context,&now)!=1||now>=deadline||
       token->provider!=t.provider||token->owner!=t.owner||token->generation!=t.generation||
       token->polarity!=t.polarity||token->observed_us>t.observed_us||now<token->observed_us||
       now-token->observed_us>=A733_TYPEC_FRESH_US){a733_typec_revoke(s);return -1;}
    return atomic_load_explicit(&s->revoked,memory_order_acquire)?-1:1;
}

int a733_typec_dp_snapshot(struct a733_typec *s,struct a733_typec_dp_token *out,uint64_t deadline)
{
    if(!intact(s)||!out||!a733_ranges_disjoint(s,sizeof(*s),out,sizeof(*out))||
       !s->config.ops.outside(s->config.context,out,sizeof(*out)))return -1;
    struct a733_typec_token cc;int r=observation(s,&cc,deadline);if(r!=1)return r;
    unsigned zero=0;if(!atomic_compare_exchange_strong_explicit(&s->publishing,&zero,1,memory_order_acquire,memory_order_relaxed))return 0;
    struct a733_typec_dp_token t=s->dp_published;
    atomic_store_explicit(&s->publishing,0,memory_order_release);
    if(!t.provider)return 0;
    uint64_t now;
    if(s->config.ops.now(s->config.context,&now)!=1||now>=deadline||now<t.observed_us||
       now-t.observed_us>=A733_TYPEC_FRESH_US||atomic_load_explicit(&s->revoked,memory_order_acquire)){
        a733_typec_revoke(s);return -1;
    }
    *out=t;return 1;
}
int a733_typec_dp_validate(struct a733_typec *s,const struct a733_typec_dp_token *t,uint64_t deadline)
{
    if(!intact(s)||!t||!a733_ranges_disjoint(s,sizeof(*s),t,sizeof(*t))||
       !s->config.ops.outside(s->config.context,t,sizeof(*t)))return -1;
    struct a733_typec_dp_token current;int r=a733_typec_dp_snapshot(s,&current,deadline);if(r!=1)return r;
    uint64_t now;
    if(s->config.ops.now(s->config.context,&now)!=1||now>=deadline||
       t->provider!=current.provider||t->owner!=current.owner||t->generation!=current.generation||
       t->polarity!=current.polarity||t->pin_assignment!=current.pin_assignment||
       t->observed_us>current.observed_us||
       now<t->observed_us||now<current.observed_us||
       now-current.observed_us>=A733_TYPEC_FRESH_US){a733_typec_revoke(s);return -1;}
    /* An ordinary HPD event invalidates the consumer snapshot, not CC power. */
    if(t->hpd!=current.hpd||t->hpd_irq!=current.hpd_irq)return -1;
    return atomic_load_explicit(&s->revoked,memory_order_acquire)?-1:1;
}
