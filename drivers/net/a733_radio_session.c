#include "a733_radio_session.h"
#ifndef SUNSTONE_AIC8800_RSN
#error "Native radio session requires the actual RSN integration"
#endif
enum { PLATFORM,ADOPT,SETUP,IDENTITY,FIRMWARE,START_RUNTIME,DISPATCH,WIFI,DATA,MANAGEMENT,NETWORK };
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);
}
static int platform_outside(struct a733_radio_platform *b,const void *p,size_t n)
{
    return separate(b,sizeof(*b),p,n)&&separate(b->clocks,sizeof(*b->clocks),p,n)&&
        separate(b->pins,sizeof(*b->pins),p,n)&&separate(b->rails,sizeof(*b->rails),p,n)&&
        separate(b->pmic,sizeof(*b->pmic),p,n)&&
        separate(b->rails->binding.gpu,sizeof(*b->rails->binding.gpu),p,n)&&
        b->pmic->ops.outside(b->pmic->cookie,p,n)==1;
}
static int outside(struct a733_radio_session *s,const void *p,size_t n)
{
    if(!separate(s,sizeof(*s),p,n)||!platform_outside(s->platform,p,n))return 0;
    for(unsigned i=0;i<AIC8800_FIRMWARE_BLOBS;++i)
        if(!separate(s->config.firmware.blobs[i].data,s->config.firmware.blobs[i].bytes,p,n))return 0;
    return 1;
}
static int owner(struct a733_radio_session *s)
{return s && s->self==s && xTaskGetCurrentTaskHandle()==s->config.owner_task && s->platform->ops.current(s->platform->cookie)==1;}
static int enter(struct a733_radio_session *s)
{
    if(!owner(s))return 0;unsigned expected=0;
    return atomic_compare_exchange_strong_explicit(&s->busy,&expected,1,memory_order_acquire,memory_order_relaxed);
}
static enum a733_radio_session_result leave(struct a733_radio_session *s,enum a733_radio_session_result r)
{atomic_store_explicit(&s->busy,0,memory_order_release);return r;}
static enum a733_radio_session_result fail(struct a733_radio_session *s)
{
    s->fault=1;
    sunxi_sdio_stop(&s->platform->host,SUNXI_SDIO_CARD_ERROR);
    if(s->dispatcher.initialized)aic8800_transport_fault_receive(&s->transport,&s->dispatcher);
    else if(s->transport.initialized)aic8800_transport_poll(&s->transport);
    if(s->borrow.generation)(void)a733_radio_platform_live(s->platform,&s->borrow,s->deadline);
    else if(s->platform->fault||s->platform->card.state==SDIO_CARD_READY)
        (void)a733_radio_platform_poll(s->platform);
    if(s->sockets.initialized){
        atomic_store(&s->sockets.failed,1);aic8800_network_fault(&s->sockets.network);
        /* Failed network polling emits link-down and services copied replies;
         * it cannot advance the retained transport or discard packet storage. */
        (void)sunstone_net_poll_owned(&s->sockets);
    }
    return A733_SESSION_RETAINED;
}
static int now(struct a733_radio_session *s,uint64_t *out)
{
    if(s->platform->ops.now_us(s->platform->cookie,out)!=1||*out<s->last_now||
        *out>=s->platform->config.lifetime_deadline_us)return 0;
    s->last_now=*out;return 1;
}
enum a733_radio_session_result a733_radio_session_retain(struct a733_radio_session *s)
{
    if(!enter(s))return A733_SESSION_INVALID;
    return leave(s,fail(s));
}
static enum a733_radio_session_result live(struct a733_radio_session *s)
{
    enum a733_radio_platform_result r=a733_radio_platform_live(s->platform,&s->borrow,s->deadline);
    if(r==A733_RADIO_BUSY)return A733_SESSION_BUSY;
    return r==A733_RADIO_READY?A733_SESSION_PENDING:fail(s);
}
static int next_lease(struct a733_radio_session *s,uint64_t current,uint64_t *deadline)
{
    uint64_t end=s->platform->config.lifetime_deadline_us;
    *deadline=s->config.runtime_lease_us>end-current?end:current+s->config.runtime_lease_us;
    return *deadline>current;
}
static enum a733_radio_session_result renew(struct a733_radio_session *s)
{
    uint64_t current,next;
    if(!now(s,&current))return fail(s);
    if(current>=s->deadline)return fail(s);
    /* poll has already authenticated this exact newly adopted bound. */
    if(s->renew_pending){s->renew_pending=0;return A733_SESSION_PENDING;}
    if(s->deadline-current>s->config.renew_margin_us)return A733_SESSION_PENDING;
    if(!next_lease(s,current,&next))return fail(s);
    if(next<=s->deadline)return A733_SESSION_PENDING;
    /* The transport validates its old deadline and actual drained boundary.
     * After its extension succeeds, no protocol poll occurs until the real
     * platform accepts the same bound. A BUSY guard preserves this transition. */
    if(aic8800_transport_renew_runtime(&s->transport,next)){
        s->deadline=next;s->renew_pending=1;
        enum a733_radio_session_result r=live(s);
        if(r==A733_SESSION_PENDING)s->renew_pending=0;
        return r;
    }
    return s->transport.state==AIC8800_TRANSPORT_FAULT?fail(s):A733_SESSION_PENDING;
}
int a733_radio_session_init(struct a733_radio_session *s,struct a733_radio_platform *p,
    const struct a733_radio_session_config *c)
{
    if(!s||s->self||!p||!p->initialized||p->fault||p->clock.id||p->borrower.generation||!c||
        !c->owner_task||!c->owner||c->bootstrap_deadline<p->config.deadline_us||
        c->bootstrap_deadline>p->config.lifetime_deadline_us||!c->runtime_lease_us||
        !c->renew_margin_us||c->renew_margin_us>=c->runtime_lease_us||c->transport.ticks_per_ms!=1000||
        !platform_outside(p,s,sizeof(*s))||!platform_outside(p,c,sizeof(*c))||
        !separate(s,sizeof(*s),c,sizeof(*c)))return 0;
    for(unsigned i=0;i<AIC8800_FIRMWARE_BLOBS;++i){
        const struct aic8800_firmware_blob *b=&c->firmware.blobs[i];
        if(!separate(s,sizeof(*s),b->data,b->bytes)||!separate(c,sizeof(*c),b->data,b->bytes)||
            !platform_outside(p,b->data,b->bytes))return 0;
    }
    /* Constructors consume this private lifetime even if a later profile fails.
     * Failed construction cannot be retried over partially initialized workers. */
    s->platform=p;s->config=*c;s->self=s;s->fault=1;atomic_init(&s->busy,0);
    if(!aic8800_firmware_init(&s->firmware,&c->firmware)||
        !aic8800_runtime_init(&s->runtime,&c->runtime)||!aic8800_wifi_init(&s->wifi,&c->wifi)||
        !aic8800_data_init(&s->data)||!aic8800_security_init(&s->security))return 0;
    s->deadline=c->bootstrap_deadline;s->fault=0;return 1;
}
enum a733_radio_session_result a733_radio_session_poll(struct a733_radio_session *s)
{
    if(!enter(s))return A733_SESSION_INVALID;
    if(s->fault)return leave(s,fail(s));
    if(s->stage==PLATFORM){
        enum a733_radio_platform_result r=a733_radio_platform_poll(s->platform);
        if(r==A733_RADIO_RETAINED||r==A733_RADIO_INVALID)return leave(s,fail(s));
        if(r==A733_RADIO_READY){
            struct sdio_card *card;
            if(!a733_radio_platform_borrow(s->platform,s->config.owner,&s->borrow,&card)||
                card!=&s->platform->card)return leave(s,fail(s));
            s->stage=ADOPT;
        }
        return leave(s,r==A733_RADIO_BUSY?A733_SESSION_BUSY:A733_SESSION_PENDING);
    }
    enum a733_radio_session_result health=live(s);
    if(health!=A733_SESSION_PENDING)return leave(s,health);
    if(s->stage>=DISPATCH){health=renew(s);if(health!=A733_SESSION_PENDING)return leave(s,health);}
    struct aic8800_sdio_transport *t=&s->transport;
    switch(s->stage){
    case ADOPT:
        if(!aic8800_transport_init(t,&s->platform->card,&s->config.transport)||
            !aic8800_transport_setup(t,s->deadline))return leave(s,fail(s));
        s->stage=SETUP;break;
    case SETUP:
        aic8800_transport_poll(t);
        if(t->state==AIC8800_TRANSPORT_READY){
            if(!aic8800_transport_read_identity(t,&s->config.bootstrap,s->deadline))return leave(s,fail(s));
            s->stage=IDENTITY;
        }
        break;
    case IDENTITY:
        aic8800_transport_poll(t);
        if(t->state==AIC8800_TRANSPORT_COMPLETE){
            if(!aic8800_transport_load_firmware(t,&s->firmware,s->deadline))return leave(s,fail(s));
            s->stage=FIRMWARE;
        }
        break;
    case FIRMWARE:
        aic8800_transport_poll(t);
        if(t->state==AIC8800_TRANSPORT_START_CONFIRMED){
            uint64_t current;if(!now(s,&current)||!next_lease(s,current,&s->deadline))return leave(s,fail(s));
            s->stage=START_RUNTIME;
        }
        break;
    case START_RUNTIME:
        if(!aic8800_transport_start_runtime(t,&s->runtime,s->deadline))return leave(s,fail(s));
        s->stage=DISPATCH;break;
    case DISPATCH:
        if(!aic8800_dispatch_init(&s->dispatcher,t))return leave(s,fail(s));
        s->stage=WIFI;break;
    case WIFI:
        if(!t->wifi && aic8800_transport_start_wifi(t,&s->wifi,s->deadline))s->stage=DATA;
        else aic8800_dispatch_poll(&s->dispatcher);
        break;
    case DATA:
        if(aic8800_transport_start_data(t,&s->data))s->stage=MANAGEMENT;
        else aic8800_dispatch_poll(&s->dispatcher);
        break;
    case MANAGEMENT:aic8800_dispatch_poll(&s->dispatcher);break;
    case NETWORK:
        if(!sunstone_net_poll_owned(&s->sockets))return leave(s,fail(s));
        break;
    default:return leave(s,fail(s));
    }
    if(t->state==AIC8800_TRANSPORT_FAULT||s->dispatcher.faulted||
        (s->sockets.initialized&&atomic_load(&s->sockets.network.failed)))return leave(s,fail(s));
    return leave(s,s->stage==NETWORK?A733_SESSION_NETWORK:s->stage==MANAGEMENT?A733_SESSION_MANAGEMENT:A733_SESSION_PENDING);
}
static int admission(struct a733_radio_session *s,uint64_t deadline)
{
    return !s->fault&&!s->renew_pending&&s->stage>=MANAGEMENT&&deadline&&deadline<=s->deadline&&
        live(s)==A733_SESSION_PENDING;
}
static int done(struct a733_radio_session *s,int ok)
{
    if(s->transport.state==AIC8800_TRANSPORT_FAULT)(void)fail(s);
    (void)leave(s,A733_SESSION_PENDING);return ok&&!s->fault;
}
int a733_radio_session_scan(struct a733_radio_session *s,uint64_t deadline)
{if(!enter(s))return 0;return done(s,admission(s,deadline)&&aic8800_transport_scan(&s->transport,deadline));}
int a733_radio_session_connect(struct a733_radio_session *s,const struct aic8800_wifi_target *target,uint64_t deadline)
{if(!enter(s))return 0;return done(s,outside(s,target,sizeof(*target))&&admission(s,deadline)&&aic8800_transport_connect(&s->transport,target,deadline));}
int a733_radio_session_disconnect(struct a733_radio_session *s,uint16_t reason,uint64_t deadline)
{if(!enter(s))return 0;return done(s,admission(s,deadline)&&aic8800_transport_disconnect(&s->transport,reason,deadline));}
int a733_radio_session_authorize_open(struct a733_radio_session *s,uint64_t record)
{if(!enter(s))return 0;return done(s,record&&admission(s,s->deadline)&&aic8800_transport_authorize_open_data(&s->transport,record));}
int a733_radio_session_start_security(struct a733_radio_session *s,const struct aic8800_supplicant_config *config,uint64_t deadline)
{
    if(!enter(s))return 0;
    int ok=outside(s,config,sizeof(*config))&&admission(s,deadline)&&
        aic8800_transport_attachment_ready(&s->transport)&&
        aic8800_supplicant_init(&s->supplicant,config)&&
        aic8800_transport_start_security(&s->transport,&s->security,&s->supplicant,deadline);
    /* Once initialized, failed attachment cannot be retried with changed keys. */
    if(!ok&&s->supplicant.initialized)(void)fail(s);
    return done(s,ok);
}
int a733_radio_session_start_network(struct a733_radio_session *s,const struct sunstone_net_config *config)
{
    if(!enter(s))return 0;
    int ok=outside(s,config,sizeof(*config))&&s->stage==MANAGEMENT&&admission(s,s->deadline)&&
        aic8800_transport_attachment_ready(&s->transport)&&
        config->bootstrap==s->config.owner_task&&config->network.transport==&s->transport&&
        config->network.dispatcher==&s->dispatcher&&sunstone_net_init_owned(&s->sockets,config)&&
        sunstone_net_activate(&s->sockets);
    if(ok)s->stage=NETWORK;else if(s->sockets.initialized)(void)fail(s);
    return done(s,ok);
}
int a733_radio_session_take_event(struct a733_radio_session *s,struct aic8800_dispatch_event *out)
{if(!enter(s))return 0;int ok=outside(s,out,sizeof(*out))&&aic8800_dispatch_take_event(&s->dispatcher,out);(void)leave(s,A733_SESSION_PENDING);return ok;}
int a733_radio_session_take_record(struct a733_radio_session *s,struct aic8800_dispatch_record *out)
{if(!enter(s))return 0;int ok=outside(s,out,sizeof(*out))&&aic8800_dispatch_take_record(&s->dispatcher,out);(void)leave(s,A733_SESSION_PENDING);return ok;}
int a733_radio_session_take_scan(struct a733_radio_session *s,struct aic8800_wifi_scan_summary *out)
{if(!enter(s))return 0;return done(s,outside(s,out,sizeof(*out))&&admission(s,s->deadline)&&aic8800_transport_take_scan(&s->transport,out));}
