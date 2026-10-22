#include "radio.h"
#include "a733_ce_native.h"
#include "a733_storage_digest.h"
#include <string.h>
enum { STARTING,CONNECTING,SECURING,ONLINE };
/* CPU time one owner poll may spend repeating session steps that advance. At
 * 25 MHz four-bit SDIO this covers the largest 4,096-byte receive. */
#define BOARD_RADIO_POLL_BUDGET_US 500u
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);
}
static int platform_id(void *context,uint8_t id[16])
{
    const struct board_radio_chip_id *source=context;
    if(!source||!source->secure_read_record||!id)return 0;
    /* Allwinner's platform API returns the four words in native little endian. */
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
        id[4*i+j]=(uint8_t)(source->words[i]>>(8*j));
    return 1;
}
int board_radio_init(struct board_radio *r,const struct board_radio_config *input)
{
    if(!r||r->self||!input||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||
        !separate(r,sizeof(*r),input,sizeof(*input)))return 0;
    const struct board_radio_config *c=input;
    if(!c->shared||!c->shared->used||!c->admit||!c->chip_id||!c->chip_id->secure_read_record||
        !c->recovery_state||!separate(r,sizeof(*r),c->recovery_state,sizeof(*c->recovery_state))||
        !separate(r,sizeof(*r),c->shared,sizeof(*c->shared))||
        !separate(r,sizeof(*r),c->chip_id,sizeof(*c->chip_id))||
        c->session.owner_task!=c->entropy.owner||c->network.bootstrap!=c->session.owner_task||
        c->entropy.clock!=&c->shared->counter||!c->entropy.source||!c->entropy.source->engine||
        c->entropy.source->engine->binding.registry!=&c->shared->clocks||
        c->target.security!=AIC8800_WIFI_WPA2_PSK_CCMP||
        !c->target.beacon_evidence||c->target.beacon_evidence!=c->security.beacon_record||
        c->security.entropy_record!=c->entropy.qualification_record||
        c->network.network.entropy_record!=c->entropy.qualification_record||
        !c->association_timeout_us||!c->security_timeout_us||
        c->startup_deadline_us<c->session.bootstrap_deadline||
        c->startup_deadline_us>c->platform.lifetime_deadline_us)return 0;
    r->self=r;r->config=*c;atomic_init(&r->failed,1);atomic_init(&r->network_started,0);
    atomic_init(&r->completed_transfers,0);
    atomic_init(&r->watchdog_deadline,0);atomic_init(&r->snapshot_busy,0);
    atomic_init(&r->client_status,0);atomic_init(&r->verified_received,0);
    struct board_radio_config *p=&r->config;
    p->session.runtime.read_platform_id=platform_id;
    p->session.runtime.context=(void *)p->chip_id;
    p->security.entropy=a733_radio_entropy_bytes;p->security.entropy_context=&r->entropy;
    p->network.network.random32=a733_radio_entropy_random32;
    p->network.network.sequence=a733_radio_entropy_sequence;
    p->network.network.entropy_context=&r->entropy;
    p->network.network.transport=&r->session.transport;
    p->network.network.dispatcher=&r->session.dispatcher;
    if(!a733_radio_entropy_init(&r->entropy,&p->entropy)||
        !a733_radio_platform_native_init(&r->native,&p->shared->locks,p->session.owner_task,
            p->admit,p->admission_context)||
        !a733_radio_platform_init(&r->platform,&p->platform,&r->native.operations,&r->native,
            &p->shared->clocks,&p->shared->pins,&p->shared->rails,&p->shared->pmic)||
        !a733_radio_session_init(&r->session,&r->platform,&p->session))return 0;
    r->constructed=1;atomic_store_explicit(&r->failed,0,memory_order_release);return 1;
}
static enum board_radio_result retain(struct board_radio *r)
{
    atomic_fetch_or_explicit(r->config.recovery_state,4,memory_order_acq_rel);
    atomic_store_explicit(&r->failed,1,memory_order_release);
    (void)a733_radio_entropy_retain(&r->entropy);
    (void)a733_radio_session_retain(&r->session);
    return BOARD_RADIO_RETAINED;
}
static uint64_t bounded_deadline(struct board_radio *r,uint64_t now,uint64_t duration)
{
    uint64_t end=r->session.deadline;
    if(end>r->config.startup_deadline_us)end=r->config.startup_deadline_us;
    return duration<end-now?now+duration:end;
}
enum board_radio_result board_radio_external_failure(struct board_radio *r)
{
    if(!r||r->self!=r||!r->constructed||
        xTaskGetCurrentTaskHandle()!=r->config.session.owner_task)return BOARD_RADIO_INVALID;
    return retain(r);
}
static enum board_radio_result publish(struct board_radio *r,enum board_radio_result result)
{
    uint64_t watch=r->stage==ONLINE?r->platform.config.lifetime_deadline_us:r->config.startup_deadline_us;
    if(r->stage==ONLINE&&r->startup_profile&&r->startup_profile->client_deadline_us<watch)
        watch=r->startup_profile->client_deadline_us;
    if(r->stage!=STARTING&&r->stage!=ONLINE&&r->operation_deadline<watch)watch=r->operation_deadline;
    if(r->session.deadline<watch)watch=r->session.deadline;
    unsigned unlocked=0;
    if(atomic_compare_exchange_strong_explicit(&r->snapshot_busy,&unlocked,1,memory_order_acquire,memory_order_relaxed)){
        atomic_store_explicit(&r->completed_transfers,r->session.transport.completed_transfers,memory_order_relaxed);
        atomic_store_explicit(&r->watchdog_deadline,watch,memory_order_relaxed);
        atomic_store_explicit(&r->snapshot_busy,0,memory_order_release);
    }
    return result;
}
static uint64_t later(uint64_t now,uint64_t span)
{return span>UINT64_MAX-now?UINT64_MAX:now+span;}
/* Admit recovery after a clean link loss; 0 once admission is overdue. The
 * recovery bound covers reassociation, the 4-way handshake and, with DHCP, the
 * configured address acquisition; the session lease also caps it. */
static int recover(struct board_radio *r,uint64_t now)
{
    uint64_t association=later(r->config.association_timeout_us,r->config.security_timeout_us);
    if(!r->loss_deadline)r->loss_deadline=later(now,association);
    if(now>=r->loss_deadline)return 0;
    uint64_t span=association;
    if(r->config.network.network.dhcp)
        span=later(span,(uint64_t)r->config.network.network.address_timeout*(1000000u/configTICK_RATE_HZ));
    uint64_t deadline=later(now,span);if(deadline>r->session.deadline)deadline=r->session.deadline;
    if(a733_radio_session_recover(&r->session,&r->config.target,&r->config.security,0,deadline)){
        r->recovering=1;r->loss_deadline=0;if(r->recoveries!=UINT64_MAX)++r->recoveries;
    }
    return 1;
}
struct progress {uint64_t host,transport;unsigned platform,session;};
static struct progress progress(const struct board_radio *r)
{return (struct progress){r->platform.host.progress,r->session.transport.progress,r->platform.stage,r->session.stage};}
static int advanced(const struct board_radio *r,const struct progress *before)
{
    struct progress now=progress(r);
    return now.host!=before->host||now.transport!=before->transport||
        now.platform!=before->platform||now.session!=before->session;
}
enum board_radio_result board_radio_poll(struct board_radio *r)
{
    if(!r||r->self!=r||!r->constructed||
        xTaskGetCurrentTaskHandle()!=r->config.session.owner_task)return BOARD_RADIO_INVALID;
    if(atomic_load_explicit(&r->failed,memory_order_acquire)||
        (atomic_load_explicit(r->config.recovery_state,memory_order_acquire)&4))return retain(r);
    enum a733_radio_entropy_result entropy=a733_radio_entropy_poll(&r->entropy);
    if(entropy==A733_RADIO_ENTROPY_RETAINED||entropy==A733_RADIO_ENTROPY_INVALID||
        atomic_load_explicit(&r->entropy.fault,memory_order_acquire)||
        atomic_load_explicit(&r->config.entropy.source->engine->fault,memory_order_acquire))return retain(r);
    uint64_t now;
    if(!a733_counter_clock_now(&r->config.shared->counter,&now))return retain(r);
    if(r->stage!=ONLINE&&now>=r->config.startup_deadline_us)return retain(r);
    /* Repeat whole session polls, each with its own platform and deadline
     * checks, while a step advances. Yield on the first call that only waits:
     * an in-flight bus operation without FIFO progress, a poll interval, an
     * idle receive status or event backpressure. */
    enum a733_radio_session_result result;
    for(uint64_t start=now;;){
        struct progress before=progress(r);
        result=a733_radio_session_poll(&r->session);
        if((result!=A733_SESSION_PENDING&&result!=A733_SESSION_MANAGEMENT&&result!=A733_SESSION_NETWORK)||
            !advanced(r,&before))break;
        if(!a733_counter_clock_now(&r->config.shared->counter,&now))return retain(r);
        if(now-start>=BOARD_RADIO_POLL_BUDGET_US)break;
    }
    if(result==A733_SESSION_RETAINED||result==A733_SESSION_INVALID)return retain(r);
    if(result==A733_SESSION_BUSY)return publish(r,BOARD_RADIO_PENDING);
    if(r->session.dispatcher.initialized){
        /* Station events of a reconnect or recovery would otherwise fill the
         * event queue and stop receive parsing. */
        if((r->stage!=ONLINE||r->recovering)&&a733_radio_session_take_event(&r->session,&r->startup_event)&&
            r->stage!=ONLINE&&r->startup_events!=UINT64_MAX)++r->startup_events;
        if(a733_radio_session_take_record(&r->session,&r->diagnostic)&&
            r->diagnostic_records!=UINT64_MAX)++r->diagnostic_records;
    }
    if(r->stage==ONLINE){
        if(result==A733_SESSION_LOST&&!recover(r,now))return retain(r);
        if(result==A733_SESSION_NETWORK)r->recovering=0;
        return publish(r,!atomic_load(&r->session.sockets.offline)&&aic8800_network_ready(&r->session.sockets.network)?
            BOARD_RADIO_NETWORK:BOARD_RADIO_PENDING);
    }
    if(result!=A733_SESSION_MANAGEMENT)return publish(r,BOARD_RADIO_PENDING);
    if(now>=r->session.deadline)return retain(r);
    if(r->stage==STARTING){
        if(entropy!=A733_RADIO_ENTROPY_READY)return publish(r,BOARD_RADIO_PENDING);
        uint64_t deadline=bounded_deadline(r,now,r->config.association_timeout_us);
        if(a733_radio_session_connect(&r->session,&r->config.target,deadline)){
            r->operation_deadline=deadline;r->stage=CONNECTING;
        }
    }else if(r->stage==CONNECTING){
        if(now>=r->operation_deadline)return retain(r);
        if(r->session.wifi.link==AIC8800_WIFI_SECURITY_PENDING){
            uint64_t deadline=bounded_deadline(r,now,r->config.security_timeout_us);
            if(a733_radio_session_start_security(&r->session,&r->config.security,deadline)){
                r->operation_deadline=deadline;r->stage=SECURING;
            }
        }else if(r->session.wifi.link!=AIC8800_WIFI_CONNECTING)return retain(r);
    }else if(r->stage==SECURING){
        if(now>=r->operation_deadline||r->session.supplicant.state==AIC8800_SUPPLICANT_FAULT)
            return retain(r);
        if(r->session.wifi.link==AIC8800_WIFI_SECURED&&
            r->session.supplicant.state==AIC8800_SUPPLICANT_AUTHORIZED&&
            a733_radio_session_start_network(&r->session,&r->config.network)){
            r->stage=ONLINE;atomic_store_explicit(&r->network_started,1,memory_order_release);
        }
    }
    return r->session.fault?retain(r):publish(r,BOARD_RADIO_PENDING);
}
int board_radio_reconnect(struct board_radio *r,uint64_t deadline)
{
    int ok=r&&r->self==r&&r->constructed&&r->stage==ONLINE&&r->config.recovery_state&&
        xTaskGetCurrentTaskHandle()==r->config.session.owner_task&&
        !atomic_load_explicit(&r->failed,memory_order_acquire)&&
        !(atomic_load_explicit(r->config.recovery_state,memory_order_acquire)&4)&&
        a733_radio_session_reconnect(&r->session,&r->config.target,&r->config.security,0,3,deadline);
    if(ok)r->recovering=1;
    return ok;
}
int board_radio_submit(struct board_radio *r,const struct sunstone_net_request *request,uint64_t *ticket)
{
    return r&&r->self==r&&r->constructed&&!atomic_load_explicit(&r->failed,memory_order_acquire)&&
        !(atomic_load_explicit(r->config.recovery_state,memory_order_acquire)&4)&&
        atomic_load_explicit(&r->network_started,memory_order_acquire)&&
        separate(r,sizeof(*r),request,sizeof(*request))&&separate(r,sizeof(*r),ticket,sizeof(*ticket))&&
        sunstone_net_submit(&r->session.sockets,request,ticket);
}
int board_radio_receive(struct board_radio *r,struct sunstone_net_reply *reply,TickType_t wait)
{
    return r&&r->self==r&&r->constructed&&atomic_load_explicit(&r->network_started,memory_order_acquire)&&
        separate(r,sizeof(*r),reply,sizeof(*reply))&&
        sunstone_net_receive(&r->session.sockets,reply,wait);
}
uint64_t board_radio_completed_transfers(const struct board_radio *r)
{
    if(!r||r->self!=r||!r->constructed||atomic_load_explicit(&r->failed,memory_order_acquire)||
        (atomic_load_explicit(r->config.recovery_state,memory_order_acquire)&4))return 0;
    uint64_t count=atomic_load_explicit(&r->completed_transfers,memory_order_acquire);
    return atomic_load_explicit(&r->failed,memory_order_acquire)||
        (atomic_load_explicit(r->config.recovery_state,memory_order_acquire)&4)?0:count;
}
int board_radio_watchdog(struct board_radio *r,uint64_t *progress,uint64_t *deadline)
{
    if(!r||r->self!=r||!r->constructed||!progress||!deadline||
        !separate(r,sizeof(*r),progress,sizeof(*progress))||!separate(r,sizeof(*r),deadline,sizeof(*deadline)))return -1;
    if(!separate(progress,sizeof(*progress),deadline,sizeof(*deadline)))return -1;
    if(atomic_load_explicit(&r->failed,memory_order_acquire)||
        (atomic_load_explicit(r->config.recovery_state,memory_order_acquire)&4))return -1;
    unsigned unlocked=0;
    if(!atomic_compare_exchange_strong_explicit(&r->snapshot_busy,&unlocked,1,memory_order_acquire,memory_order_relaxed))return 0;
    uint64_t p=atomic_load_explicit(&r->completed_transfers,memory_order_relaxed);
    uint64_t d=atomic_load_explicit(&r->watchdog_deadline,memory_order_relaxed);
    atomic_store_explicit(&r->snapshot_busy,0,memory_order_release);
    if(atomic_load_explicit(&r->failed,memory_order_acquire)||
        (atomic_load_explicit(r->config.recovery_state,memory_order_acquire)&4))return -1;
    if(!p||!d)return 0;*progress=p;*deadline=d;return 1;
}
static int evidence(const uint8_t bytes[32])
{unsigned value=0;for(unsigned i=0;i<32;++i)value|=bytes[i];return value!=0;}
static int ce_now(void *cookie,uint64_t *now)
{struct board_radio_ce_provider *p=cookie;return a733_counter_clock_now(&p->shared->counter,now);}
static int ce_enter(void *cookie,uint64_t deadline)
{struct board_radio_ce_provider *p=cookie;return a733_platform_lock_enter(&p->shared->locks,A733_PLATFORM_REGISTER_LOCK,deadline);}
static void ce_leave(void *cookie)
{struct board_radio_ce_provider *p=cookie;a733_platform_lock_leave(&p->shared->locks,A733_PLATFORM_REGISTER_LOCK);}
static void ce_retain(void *cookie)
{struct board_radio_ce_provider *p=cookie;(void)a733_platform_lock_retain(&p->shared->locks,A733_PLATFORM_REGISTER_LOCK);}
static int ce_authorize(void *cookie,const struct a733_ce_config *c,const struct a733_clock_token *t,
    const struct a733_ce_memory *memory)
{
    struct board_radio_ce_provider *p=cookie;
    const struct board_radio_profile *f=p->profile;
    if(!p->used||!f->ce_authority||!f->secure_ce_authority||!evidence(f->ce_evidence)||
        !p->source||p->source->engine!=&p->source->owned_session||
        memory!=p->source->engine->controller.memory||t->id!=p->token.id||
        t->owner!=p->token.owner||t->shared!=p->token.shared||t->exclusive!=p->token.exclusive||
        c->owner!=f->ce.owner||c->handoff!=f->ce.handoff||c->profile!=A733_CE_TRNG_ONLY||
        c->health_profile!=f->ce.health_profile||c->configure_health)return 0;
    for(unsigned i=0;i<4;++i)if(c->health[i]!=f->ce.health[i])return 0;
    for(unsigned i=0;i<3;++i)if(c->fabric[i]!=f->ce.fabric[i]||c->fabric_hz[i]!=f->ce.fabric_hz[i])return 0;
    return 1;
}
static int entropy_qualified(void *cookie,const struct a733_ce_session *engine,uint64_t record)
{
    struct board_radio_ce_provider *p=cookie;const struct board_radio_profile *f=p->profile;
    return p->used&&p->source&&engine==p->source->engine&&engine==&p->source->owned_session&&
        record==f->entropy_record&&record&&f->characterization_record&&f->startup_test_record&&
        f->health_enable_epoch&&evidence(f->entropy_evidence)&&engine->binding.token.id==p->token.id&&
        engine->binding.config.health_profile==f->ce.health_profile&&!f->ce.configure_health;
}
static int radio_authorize(void *cookie,const struct a733_radio_platform_config *c,const struct a733_clock_token *token)
{
    struct board_radio_ce_provider *p=cookie;const struct board_radio_profile *f=p->profile;
    return p->used&&f->platform_authority&&evidence(f->platform_evidence)&&
        token->owner==f->platform.owner&&memcmp(c,&f->platform,sizeof(*c))==0;
}
static int startup_profile_valid(const struct board_radio_profile *f,const uint8_t expected[32],
    const struct board_radio_chip_id *id)
{
    if(!f||!expected||!id||
        f->magic!=BOARD_RADIO_PROFILE_MAGIC||(f->version!=BOARD_RADIO_PROFILE_VERSION&&f->version!=BOARD_RADIO_PROFILE_LISTENER_VERSION)||f->bytes!=sizeof(*f)||!board_radio_profile_network_valid(f)||
        !f->platform_authority||!f->ce_authority||!f->secure_ce_authority||
        !evidence(f->platform_evidence)||!evidence(f->ce_evidence)||!evidence(f->entropy_evidence)||
        !f->entropy_record||!f->characterization_record||!f->startup_test_record||!f->health_enable_epoch||
        memcmp(f->chip_id,id->words,sizeof(f->chip_id))||!id->secure_read_record||
        f->ce.configure_health||f->ce.profile!=A733_CE_TRNG_ONLY||
        !f->peer_port||!f->request_bytes||f->request_bytes>sizeof(f->request)||
        !f->response_bytes||f->response_bytes>sizeof(f->response)||!f->client_timeout_ticks||
        f->client_timeout_ticks>(uint32_t)(portMAX_DELAY/2)||
        f->client_deadline_us<=f->startup_deadline_us||f->client_deadline_us>f->platform.lifetime_deadline_us)return 0;
    uint8_t actual[32];storage_digest_bytes(f,sizeof(*f),actual);
    if(memcmp(actual,expected,sizeof(actual)))return 0;
    for(unsigned i=0;i<4;++i){
        if(f->firmware_address[i]<UINT64_C(0x43010000)||f->firmware_address[i]>=UINT64_C(0x43100000)||
            !f->firmware_bytes[i]||f->firmware_bytes[i]>UINT64_C(0x43100000)-f->firmware_address[i])return 0;
        for(unsigned j=0;j<i;++j)
            if(f->firmware_address[i]<f->firmware_address[j]+f->firmware_bytes[j]&&
                f->firmware_address[j]<f->firmware_address[i]+f->firmware_bytes[i])return 0;
    }
    return 1;
}
int board_radio_startup(struct board_radio *r,struct a733_native_usb *shared,
    struct board_radio_ce_provider *p,const void *providers,size_t provider_bytes,
    const struct board_radio_profile *f,const uint8_t expected[32],const struct board_radio_chip_id *id,
    TaskHandle_t owner,TaskHandle_t client,atomic_uint *recovery_state)
{
    if(!r||r->self||!shared||!p||p->used||!providers||!provider_bytes||!owner||!client||
        owner==client||!recovery_state||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||
        !startup_profile_valid(f,expected,id)||!separate(r,sizeof(*r),providers,provider_bytes)||
        !separate(r,sizeof(*r),f,sizeof(*f)))return 0;
    p->used=1;p->shared=shared;p->profile=f;p->source=&r->ce_source;
    p->pool_mutex=xSemaphoreCreateMutexStatic(&p->mutex_storage);
    if(!p->pool_mutex)return 0;
    a733_clock_mask_t stable=A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC|A733_CLOCK_RC16M;
    if(a733_clock_acquire(&shared->clocks,f->ce.owner,stable,A733_CLOCK_CE,f->ce.deadline_us,&p->token)!=A733_CLOCK_OK)return 0;
    struct a733_ce_ops ops={.now_us=ce_now,.authorize=ce_authorize,.enter=ce_enter,.leave=ce_leave,.retain=ce_retain,
        .read=a733_ce_native_read,.write=a733_ce_native_write,.sync=a733_ce_native_sync};
    if(xSemaphoreTake(p->pool_mutex,0)!=pdTRUE)return 0;
    int bound=a733_ce_entropy_native_init(&r->ce_source,&f->ce,&ops,p,&shared->clocks,&p->token);
    BaseType_t released=xSemaphoreGive(p->pool_mutex);
    if(!bound||released!=pdTRUE)return 0;
    struct board_radio_config c={.shared=shared,.platform=f->platform,.chip_id=id,.recovery_state=recovery_state,
        .admit=radio_authorize,.admission_context=p,
        .entropy={.source=&r->ce_source,.owner=owner,.pool_mutex=p->pool_mutex,.clock=&shared->counter,
            .qualification_record=f->entropy_record,.request_timeout_us=f->refill_timeout_us,
            .provider_state=providers,.provider_bytes=provider_bytes,.qualified=entropy_qualified,
            .qualification_context=p,.qualification_bytes=sizeof(*p)},
        .target=f->target,.startup_deadline_us=f->startup_deadline_us,
        .association_timeout_us=f->association_timeout_us,.security_timeout_us=f->security_timeout_us};
    c.session=(struct a733_radio_session_config){.owner_task=owner,.owner=f->session_owner,
        .bootstrap_deadline=f->bootstrap_deadline_us,.runtime_lease_us=f->runtime_lease_us,
        .renew_margin_us=f->renew_margin_us,.transport=f->transport,.bootstrap=f->bootstrap,
        .firmware={.identity=f->radio_identity,.profile=1},.runtime={.binding_timeout=f->binding_timeout_us},.wifi=f->wifi};
    for(unsigned i=0;i<4;++i)c.session.firmware.blobs[i]=(struct aic8800_firmware_blob){
        (const uint8_t *)(uintptr_t)f->firmware_address[i],(size_t)f->firmware_bytes[i]};
    c.security=(struct aic8800_supplicant_config){.entropy_record=f->entropy_record,
        .beacon_record=f->target.beacon_evidence,.ticks_per_second=1000000,.command_timeout=f->command_timeout_us,
        .ap_rsn_bytes=f->ap_rsn_bytes};
    memcpy(c.security.pmk,f->pmk,sizeof(f->pmk));memcpy(c.security.ap_rsn,f->ap_rsn,sizeof(f->ap_rsn));
    c.network=(struct sunstone_net_config){.bootstrap=owner,.clients={client},.network={
        .entropy_record=f->entropy_record,.tx_timeout=f->tx_timeout_us,.queue_timeout=f->queue_timeout_ticks,
        .dhcp=f->network_dhcp,.address_timeout=(TickType_t)f->address_timeout_ticks,
        .lease_cap=(TickType_t)f->lease_cap_ticks}};
    memcpy(c.network.network.address,f->address,4);memcpy(c.network.network.netmask,f->netmask,4);
    memcpy(c.network.network.gateway,f->gateway,4);memcpy(c.network.network.dns,f->network_dns,4);
    if(!board_radio_init(r,&c))return 0;
    r->startup_profile=f;return 1;
}
/* While the link is lost, or a reassociation is under way, socket requests
 * wait for the port and may reach their own timeout. A SEND or RECEIVE of an
 * established exchange reports in that TIMEOUT reply the bytes it still
 * moved, and repeating it moves no byte twice, so the client repeats it
 * within its absolute deadline. send_stage names the SEND stage; RECEIVE
 * follows it. Every other non-OK reply remains terminal. */
static unsigned exchange_timeout(const struct board_radio *r,const struct sunstone_net_reply *reply,unsigned send_stage)
{return reply->result==SUNSTONE_NET_TIMEOUT&&(r->client_stage==send_stage||r->client_stage==send_stage+1);}
static int client_failure(struct board_radio *r)
{atomic_fetch_or_explicit(r->config.recovery_state,4,memory_order_acq_rel);
 atomic_store_explicit(&r->client_status,2,memory_order_release);
 atomic_store_explicit(&r->failed,1,memory_order_release);return -1;}
int board_radio_client_result(const struct board_radio *r,unsigned *verified)
{
    if(!r||r->self!=r||!r->constructed||!verified||!separate(r,sizeof(*r),verified,sizeof(*verified)))return -1;
    unsigned status=atomic_load_explicit(&r->client_status,memory_order_acquire);
    *verified=atomic_load_explicit(&r->verified_received,memory_order_acquire);
    if(status==2||atomic_load_explicit(&r->failed,memory_order_acquire)||
        (atomic_load_explicit(r->config.recovery_state,memory_order_acquire)&4))return -1;
    return status==1?1:0;
}
/* One admitted listener and one accepted peer. SEND copies the challenge;
 * RECEIVE verifies the peer's application response before either CLOSE. */
static int listener_poll(struct board_radio *r)
{
    const struct board_radio_profile *f=r->startup_profile;
    if(r->client_stage==7)return 1;
    uint64_t now;if(!a733_counter_clock_now(&r->config.shared->counter,&now)||now>=f->client_deadline_us)return client_failure(r);
    if(!atomic_load_explicit(&r->network_started,memory_order_acquire))return 0;
    if(r->client_pending){
        struct sunstone_net_reply reply;if(!board_radio_receive(r,&reply,0))return 0;
        r->client_reply=reply;r->client_pending=0;
        if(reply.ticket!=r->client_ticket)return client_failure(r);
        if(r->client_stage==0&&reply.handle)r->listener_handle=reply.handle;
        if(r->client_stage==2&&reply.handle&&reply.handle!=r->listener_handle)r->client_handle=reply.handle;
        uint64_t expected=r->client_stage==1||r->client_stage==6?r->listener_handle:r->client_handle;
        unsigned waited=exchange_timeout(r,&reply,3);
        int ok=(reply.result==SUNSTONE_NET_OK||waited)&&(r->client_stage==0?r->listener_handle!=0:reply.handle==expected);
        if(r->client_stage==2)ok=ok&&r->client_handle&&reply.port&& !memcmp(reply.address,f->peer_address,4);
        if(r->client_stage==3)ok=ok&&(reply.stack_result>0||(waited&&!reply.stack_result))&&
            (unsigned)reply.stack_result<=f->request_bytes-r->client_sent;
        if(r->client_stage==4)ok=ok&&(reply.bytes||waited)&&reply.bytes<=f->response_bytes-r->client_received&&
            !memcmp(reply.payload,f->response+r->client_received,reply.bytes);
        if(!ok){
            r->client_error=1;
            if(r->client_stage==6)return client_failure(r);
            r->client_stage=r->client_stage==5?6:r->client_handle?5:r->listener_handle?6:7;
            if(r->client_stage==7)return client_failure(r);
        }else if(r->client_stage==3){
            r->client_sent+=(unsigned)reply.stack_result;if(r->client_sent==f->request_bytes)++r->client_stage;
        }else if(r->client_stage==4){
            r->client_received+=reply.bytes;
            atomic_store_explicit(&r->verified_received,(unsigned)r->client_received,memory_order_release);
            if(r->client_received==f->response_bytes)++r->client_stage;
        }else{
            ++r->client_stage;
            if(r->client_stage==7){
                if(r->client_error)return client_failure(r);
                atomic_store_explicit(&r->client_status,1,memory_order_release);return 1;
            }
        }
    }
    struct sunstone_net_request request={.timeout=f->client_timeout_ticks};
    if(r->client_stage==0){request.operation=SUNSTONE_NET_OPEN_TCP;request.port=f->peer_port;}
    else if(r->client_stage==1){request.operation=SUNSTONE_NET_LISTEN;request.handle=r->listener_handle;}
    else if(r->client_stage==2){request.operation=SUNSTONE_NET_ACCEPT;request.handle=r->listener_handle;}
    else if(r->client_stage==3){request.operation=SUNSTONE_NET_SEND;request.handle=r->client_handle;
        request.bytes=(uint16_t)(f->request_bytes-r->client_sent);memcpy(request.payload,f->request+r->client_sent,request.bytes);}
    else if(r->client_stage==4){request.operation=SUNSTONE_NET_RECEIVE;request.handle=r->client_handle;
        request.bytes=(uint16_t)(f->response_bytes-r->client_received);}
    else{request.operation=SUNSTONE_NET_CLOSE;request.handle=r->client_stage==5?r->client_handle:r->listener_handle;}
    uint64_t ticket;if(board_radio_submit(r,&request,&ticket)){r->client_ticket=ticket;r->client_pending=1;}
    return 0;
}
int board_radio_client_poll(struct board_radio *r)
{
    if(!r||r->self!=r||!r->constructed||!r->startup_profile||
        xTaskGetCurrentTaskHandle()!=r->config.network.clients[0])return -1;
    if(atomic_load_explicit(&r->failed,memory_order_acquire)||
        (atomic_load_explicit(r->config.recovery_state,memory_order_acquire)&4))return -1;
    const struct board_radio_profile *f=r->startup_profile;
    if(f->tcp_mode)return listener_poll(r);
    if(r->client_stage==5)return 1;
    uint64_t now;if(!a733_counter_clock_now(&r->config.shared->counter,&now)||now>=f->client_deadline_us)
        return client_failure(r);
    if(!atomic_load_explicit(&r->network_started,memory_order_acquire))return 0;
    struct sunstone_net_request *request=&r->client_request;
    if(r->client_pending){
        struct sunstone_net_reply copied;
        if(!board_radio_receive(r,&copied,0))return 0;
        r->client_reply=copied;
        struct sunstone_net_reply *reply=&r->client_reply;r->client_pending=0;
        unsigned waited=exchange_timeout(r,reply,2);
        if(reply->ticket!=r->client_ticket||(reply->result!=SUNSTONE_NET_OK&&!waited))return client_failure(r);
        if(r->client_stage==0){r->client_handle=reply->handle;++r->client_stage;}
        else if(r->client_stage==1)++r->client_stage;
        else if(r->client_stage==2){
            if((reply->stack_result<=0&&!(waited&&!reply->stack_result))||
               (unsigned)reply->stack_result>f->request_bytes-r->client_sent)return client_failure(r);
            r->client_sent+=(unsigned)reply->stack_result;if(r->client_sent==f->request_bytes)++r->client_stage;
        }else if(r->client_stage==3){
            if((!reply->bytes&&!waited)||reply->bytes>f->response_bytes-r->client_received||
                memcmp(reply->payload,f->response+r->client_received,reply->bytes))return client_failure(r);
            r->client_received+=reply->bytes;
            atomic_store_explicit(&r->verified_received,(unsigned)r->client_received,memory_order_release);
            if(r->client_received==f->response_bytes)++r->client_stage;
        }else if(r->client_stage==4){++r->client_stage;
            atomic_store_explicit(&r->client_status,1,memory_order_release);return 1;}
    }
    memset(request,0,sizeof(*request));request->handle=r->client_handle;request->timeout=f->client_timeout_ticks;
    request->port=f->peer_port;memcpy(request->address,f->peer_address,4);
    if(r->client_stage==0){request->operation=SUNSTONE_NET_OPEN_TCP;request->port=0;}
    else if(r->client_stage==1)request->operation=SUNSTONE_NET_CONNECT;
    else if(r->client_stage==2){request->operation=SUNSTONE_NET_SEND;
        request->bytes=(uint16_t)(f->request_bytes-r->client_sent);memcpy(request->payload,f->request+r->client_sent,request->bytes);}
    else if(r->client_stage==3){request->operation=SUNSTONE_NET_RECEIVE;request->bytes=(uint16_t)(f->response_bytes-r->client_received);}
    else request->operation=SUNSTONE_NET_CLOSE;
    /* Queue copies must lie outside the entire radio graph; the local copy
     * expires immediately after submit, while service owns its copied bytes. */
    struct sunstone_net_request copy=*request;uint64_t ticket;
    if(board_radio_submit(r,&copy,&ticket)){r->client_ticket=ticket;r->client_pending=1;}
    return 0;
}
