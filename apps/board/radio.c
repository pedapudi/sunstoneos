#include "radio.h"
#include "a733_ce_native.h"
#include "a733_range.h"
#include "a733_storage_digest.h"
#include "sunstone_wifi_credential.h"
#include <string.h>
/* Present in the OS image only (apps/os); the association stage fails cleanly without them. */
extern void os_ce_entropy_begin(void) __attribute__((weak));
extern void os_ce_entropy_step(void) __attribute__((weak));
extern int os_ce_entropy_ready(void) __attribute__((weak));
extern int os_ce_entropy_failed(void) __attribute__((weak));
extern int os_ce_entropy_bytes(void *,uint8_t *,size_t) __attribute__((weak));
extern void os_ce_entropy_wipe(void) __attribute__((weak));
extern int os_radio_quiesce_fault(uint32_t,const void *) __attribute__((weak));
extern int ss_wifi_credential(const uint8_t **,size_t *,const uint8_t **,const uint8_t **,size_t *) __attribute__((weak));
enum { STARTING,CONNECTING,SECURING,ONLINE,SCANNING,SCANNED,SHUTTING_DOWN,DOWN };
#ifdef SUNSTONE_A733_RADIO_IRQ
#include "sunstone_deadline.h"
static int event_bind(void *,TaskHandle_t);
static uint64_t event_deadline(void *);
static int event_work(void *);
#endif
/* Elapsed time before an owner stops repeating advancing session steps. At
 * 25 MHz four-bit SDIO this covers the largest 4,096-byte receive. */
#define BOARD_RADIO_POLL_BUDGET_US 500u
static int platform_id(void *context,uint8_t id[16])
{
    const struct board_radio_chip_id *source=context;
    if(!source||!source->secure_read_record||!id)return 0;
    /* Allwinner's platform API returns the four words in native little endian. */
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)
        id[4*i+j]=(uint8_t)(source->words[i]>>(8*j));
    return 1;
}
/* The report's scan list comes from the FreeRTOS heap that the network stack already uses. */
static void *report_allocate(void *context,size_t bytes){(void)context;return pvPortMalloc(bytes);}
static void report_release(void *context,void *memory){(void)context;vPortFree(memory);}
int board_radio_init(struct board_radio *r,const struct board_radio_config *input)
{
    if(!r||r->self||!input||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||
        !a733_ranges_disjoint(r,sizeof(*r),input,sizeof(*input)))return 0;
    const struct board_radio_config *c=input;
    unsigned staged=c->report_memory.allocate!=NULL;
    if(c->scan_only&&(!staged||!c->scan_timeout_us))return 0;
    if(c->associate&&(!c->scan_only||c->enumeration_only||!c->connect_timeout_us||!c->security_timeout_us||
        !c->security.command_timeout))return 0;
    if(staged&&(!c->report_memory.release||!c->scan_timeout_us))return 0;
    if(!c->shared||!c->shared->used||!c->admit||!c->chip_id||!c->chip_id->secure_read_record||
        !c->recovery_state||!a733_ranges_disjoint(r,sizeof(*r),c->recovery_state,sizeof(*c->recovery_state))||
        !a733_ranges_disjoint(r,sizeof(*r),c->shared,sizeof(*c->shared))||
        !a733_ranges_disjoint(r,sizeof(*r),c->chip_id,sizeof(*c->chip_id))||
        (!c->scan_only&&(
        c->session.owner_task!=c->entropy.owner||c->network.bootstrap!=c->session.owner_task||
        c->entropy.clock!=&c->shared->counter||!c->entropy.source||!c->entropy.source->engine||
        c->entropy.source->engine->binding.registry!=&c->shared->clocks||
        c->target.security!=AIC8800_WIFI_WPA2_PSK_CCMP||
        !c->target.beacon_evidence||c->target.beacon_evidence!=c->security.beacon_record||
        c->security.entropy_record!=c->entropy.qualification_record||
        c->network.network.entropy_record!=c->entropy.qualification_record||
        !c->association_timeout_us||!c->security_timeout_us))||
        c->startup_deadline_us<c->session.bootstrap_deadline||
        c->startup_deadline_us>c->platform.lifetime_deadline_us)return 0;
    r->self=r;r->config=*c;atomic_init(&r->failed,1);atomic_init(&r->network_started,0);
    atomic_init(&r->completed_transfers,0);
    atomic_init(&r->watchdog_deadline,0);atomic_init(&r->snapshot_busy,0);
    atomic_init(&r->client_status,0);atomic_init(&r->verified_received,0);
    struct board_radio_config *p=&r->config;
    p->session.runtime.read_platform_id=platform_id;
    p->session.runtime.context=(void *)p->chip_id;
    /* The association stage brings its own (unqualified) entropy callback in the configuration. */
    if(!p->associate){p->security.entropy=a733_radio_entropy_bytes;p->security.entropy_context=&r->entropy;}
    p->network.network.random32=a733_radio_entropy_random32;
    p->network.network.sequence=a733_radio_entropy_sequence;
    p->network.network.entropy_context=&r->entropy;
    p->network.network.transport=&r->session.transport;
    p->network.network.dispatcher=&r->session.dispatcher;
    if(staged&&!a733_radio_report_init(&r->report,&p->report_memory))return 0;
    if((!p->scan_only&&!a733_radio_entropy_init(&r->entropy,&p->entropy))||
        !a733_radio_platform_native_init(&r->native,&p->shared->locks,p->session.owner_task,
            p->admit,p->admission_context)||
        !a733_radio_platform_init(&r->platform,&p->platform,&r->native.operations,&r->native,
            &p->shared->clocks,&p->shared->pins,&p->shared->rails,&p->shared->pmic)||
        ((!p->enumeration_only||p->four_bit)&&!a733_radio_platform_four_bit(&r->platform))||
        (p->first_run_guard&&!a733_radio_platform_first_run_guard(&r->platform))||
        !a733_radio_session_init(&r->session,&r->platform,&p->session))return 0;
#ifdef SUNSTONE_A733_RADIO_IRQ
    if(!a733_radio_events_init(&r->events,&r->platform,p->session.owner_task))return 0;
    p->network.events=(struct sunstone_net_event_source){event_bind,event_deadline,event_work,r};
#endif
    r->constructed=1;atomic_store_explicit(&r->failed,0,memory_order_release);return 1;
}
#ifdef SUNSTONE_A733_RADIO_IRQ
/* The OS supplies the supervision certificate; the non-OS board composition
 * has its own watchdog policy and may omit this hook. */
extern int os_radio_owner_blocked_until(uint64_t) __attribute__((weak));
extern int os_radio_client_blocked_until(uint64_t) __attribute__((weak));
static uint64_t event_min(uint64_t a,uint64_t b){return b&&(!a||b<a)?b:a;}
static int event_bind(void *cookie,TaskHandle_t owner)
{
    struct board_radio *r=cookie;
    return owner==r->events.owner&&a733_radio_events_begin(&r->events);
}
static uint64_t event_deadline(void *cookie)
{return ((struct board_radio *)cookie)->event_until;}
static int event_work(void *cookie)
{
    struct board_radio *r=cookie;
    if(!r||!r->constructed||!a733_radio_events_begin(&r->events))return -1;
    uint64_t now=sunstone_deadline_now_us(),until=r->platform.config.lifetime_deadline_us;
    struct a733_radio_platform *p=&r->platform;
    if(now>=until)return -1;
    unsigned stopping=p->stop_requested||p->aborting||r->stage==SHUTTING_DOWN||
        atomic_load_explicit(&r->failed,memory_order_acquire);
    unsigned software=r->event_ready;r->event_ready=0;
    if(software&&!stopping)return 1;
    if(r->event_caller_limit&&r->event_caller_limit<=now){
        if(r->event_caller_due==r->event_caller_limit)return -1;
        r->event_caller_due=r->event_caller_limit;return 1;
    }
    r->event_caller_due=0;until=event_min(until,r->event_caller_limit);
    if(p->stop_requested)until=event_min(until,p->stop_deadline);
    else if(!stopping&&r->stage!=ONLINE&&r->stage!=SCANNED&&r->stage!=DOWN)until=event_min(until,r->config.startup_deadline_us);
    if(stopping&&r->stage!=DOWN&&!a733_radio_platform_clean(p))until=event_min(until,p->deadline);
    /* The publisher always knows this immutable owner handle. Registering
     * before this acquire check preserves readiness published before sleep. */
    if(!atomic_load_explicit(&r->config.shared->pmic_ready,memory_order_acquire)||
       (atomic_load_explicit(&r->failed,memory_order_acquire)&&a733_radio_platform_clean(p))||r->stage==DOWN){
        r->event_until=until;
        if(os_radio_owner_blocked_until&&!os_radio_owner_blocked_until(until))return -1;
        return 0;
    }
    if(!stopping){
        until=event_min(until,r->session.deadline);
        if(r->session.deadline>r->session.config.renew_margin_us&&
           r->session.deadline-r->session.config.renew_margin_us>now)
            until=event_min(until,r->session.deadline-r->session.config.renew_margin_us);
        if(r->session.transport.initialized){
            if(aic8800_transport_event_work(&r->session.transport,now))return 1;
            until=event_min(until,aic8800_transport_event_deadline(&r->session.transport,now));
        }
        if(p->card.state==SDIO_CARD_ENUMERATING&&p->card.retry_at>now)
            until=event_min(until,p->card.retry_at);
    }
    if(until<=now)return stopping?-1:1; /* A live timer may mature after the last pass. */
    if(p->host.state==SUNXI_SDIO_UNPREPARED||p->host.state==SUNXI_SDIO_QUIESCED||p->aborting||
       (stopping&&p->host.state==SUNXI_SDIO_READY&&p->stage!=RSTAGE_READY)){
        uint64_t cadence=50; /* Existing non-IRQ metadata/PMIC result observation. */
        if(p->stage==RSTAGE_SUPPLY_WAIT)cadence=p->wait_since+10001>now?p->wait_since+10001-now:1;
        else if(p->stage==RSTAGE_MODULE_SETTLE)cadence=p->wait_since+50001>now?p->wait_since+50001-now:1;
        else if(p->stage==RSTAGE_RESET_WAIT)cadence=p->wait_since+10>now?p->wait_since+10-now:1;
        if(now>UINT64_MAX-cadence)return -1;
        until=event_min(until,now+cadence);
        r->event_until=until;
        if(os_radio_owner_blocked_until&&!os_radio_owner_blocked_until(until))return -1;
        return 0;
    }
    if(p->host.state==SUNXI_SDIO_READY&&p->stage!=RSTAGE_READY&&
       !(p->card.state==SDIO_CARD_ENUMERATING&&p->card.retry_at>now))return 1;
    int ready=a733_radio_events_prepare(&r->events,&until);
    if(ready)return ready;
    r->event_until=until;
    if(os_radio_owner_blocked_until&&!os_radio_owner_blocked_until(until))return -1;
    return 0;
}
#endif
int board_radio_owner_wait(struct board_radio *r,uint64_t caller_limit)
{
#ifdef SUNSTONE_A733_RADIO_IRQ
    if(!r||!r->constructed)return -1;
    r->event_caller_limit=caller_limit;
    if(r->stage==ONLINE&&!atomic_load_explicit(&r->failed,memory_order_acquire)&&!r->platform.aborting&&
       r->session.sockets.initialized&&r->session.sockets.event_bound&&!r->platform.stop_requested)
        return sunstone_net_wait_owned(&r->session.sockets)?1:-1;
    int work=event_work(r);
    if(work)return work;
    int result=sunstone_deadline_wait_until(r->event_until);
    if(result==SUNSTONE_DEADLINE_ERROR)return -1;
    ++r->events.wakes;if(result==SUNSTONE_DEADLINE_EXPIRED)++r->events.deadline_wakes;
    return 1;
#else
    (void)r;(void)caller_limit;return 0;
#endif
}
int board_radio_client_wait(struct board_radio *r)
{
#ifdef SUNSTONE_A733_RADIO_IRQ
    if(!r||!r->constructed||xTaskGetCurrentTaskHandle()!=r->config.network.clients[0])return -1;
    if(!r->client_registered){if(!sunstone_deadline_register())return -1;r->client_registered=1;}
    uint64_t until=r->platform.config.lifetime_deadline_us;
    if(!r->config.scan_only){
        if(!r->startup_profile)return -1;
        unsigned status=atomic_load_explicit(&r->client_status,memory_order_acquire);
        if(status!=1)until=r->startup_profile->client_deadline_us;
        if(atomic_load_explicit(&r->failed,memory_order_acquire))return -1;
        if(r->client_pending&&r->session.sockets.initialized&&
           uxQueueMessagesWaiting(r->session.sockets.clients[0].reply_queue))return 1;
        if(!r->client_pending&&!r->client_retry&&!status&&
           atomic_load_explicit(&r->network_started,memory_order_acquire))return 1;
    }
    if(sunstone_deadline_now_us()>=until)return -1;
    if(os_radio_client_blocked_until&&!os_radio_client_blocked_until(until))return -1;
    if(sunstone_deadline_wait_until(until)==SUNSTONE_DEADLINE_ERROR)return -1;
    r->client_retry=0;return 1;
#else
    (void)r;return 0;
#endif
}
void board_radio_irq(struct board_radio *r)
{
#ifdef SUNSTONE_A733_RADIO_IRQ
    if(r)a733_radio_events_irq(&r->events);
#else
    (void)r;
#endif
}
static void assoc_update(struct board_radio *r);
static void wipe_pmk(struct board_radio *r);
static void wipe_secrets(struct board_radio *r);
/* The board's failure bound on the failure shutdown, past the platform's own 2 s deadline. */
#define FAILURE_SHUTDOWN_BOUND_US 3000000u
static enum board_radio_result retain(struct board_radio *r)
{
    uint64_t now=0;
    int have=a733_counter_clock_now(&r->config.shared->counter,&now);
    if(!r->failing){r->failing=1;r->failing_since=now;}
    unsigned was_failed=atomic_exchange_explicit(&r->failed,1,memory_order_acq_rel);
#ifdef SUNSTONE_A733_RADIO_IRQ
    if(!was_failed&&r->config.network.clients[0])xTaskNotifyGive(r->config.network.clients[0]);
#else
    (void)was_failed;
#endif
    /* The first retain starts the failure shutdown when the module is powered; a later one never cuts it short. */
    if(!r->wiped)(void)a733_radio_session_retain(&r->session);
    unsigned waiting=r->platform.aborting&&!r->platform.abort_done&&!r->platform.fault&&have&&
        now-r->failing_since<FAILURE_SHUTDOWN_BOUND_US;
    if(waiting){
        (void)a733_radio_platform_poll(&r->platform);
        waiting=r->platform.aborting&&!r->platform.abort_done&&!r->platform.fault;
    }
    /* The board's failure bit waits for the failure shutdown. A failure that left nothing powered (a
     * refusal before any write, or a failure shutdown that finished with every readback matched) leaves it alone, so
     * the session ends normally and the record can be read; a watchdog reset would lose DRAM. */
    if(!waiting&&!a733_radio_platform_clean(&r->platform)&&
       (!os_radio_quiesce_fault||!os_radio_quiesce_fault(r->platform.fail_line<<16|(r->platform.fail_stage&0xffffu),
           __builtin_return_address(0))))atomic_fetch_or_explicit(r->config.recovery_state,4,memory_order_acq_rel);
    if(!r->config.scan_only)(void)a733_radio_entropy_retain(&r->entropy);
    if(waiting)return BOARD_RADIO_PENDING;
    if(r->config.report_memory.allocate){
        a733_radio_report_platform(&r->report,&r->platform);
        a733_radio_report_firmware_progress(&r->report,&r->session.firmware,r->session.transport.config.ticks_per_ms);
        a733_radio_report_session_failure(&r->report,r->session.fail_line,r->session.fail_stage);
        assoc_update(r);
    }
    /* The failure is final: nothing polls the session again, so every credential-derived byte leaves RAM now, at EL1. */
    wipe_secrets(r);
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
#ifdef SUNSTONE_A733_RADIO_IRQ
    if(!a733_radio_events_begin(&r->events))return retain(r);
#endif
    return retain(r);
}
static enum board_radio_result publish(struct board_radio *r,enum board_radio_result result)
{
#ifdef SUNSTONE_A733_RADIO_IRQ
    if(r->stage!=r->event_before_stage)r->event_ready=1;
#endif
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
int board_radio_shutdown(struct board_radio *r,uint64_t deadline)
{
    if(!r||r->self!=r||!r->constructed||r->stage!=SCANNED||xTaskGetCurrentTaskHandle()!=r->config.session.owner_task||
       atomic_load_explicit(&r->failed,memory_order_acquire)||
       (atomic_load_explicit(r->config.recovery_state,memory_order_acquire)&4))return 0;
    const struct aic8800_sdio_transport *t=&r->session.transport;
    /* The transport must be quiet: nothing in flight on the bus and no record held. */
    if(t->pending||t->held||r->session.fault||r->platform.host.state!=SUNXI_SDIO_READY)return 0;
    if(!a733_radio_platform_shutdown(&r->platform,&r->session.borrow,deadline))return 0;
    r->stage=SHUTTING_DOWN;return 1;
}
const struct a733_radio_report *board_radio_report(const struct board_radio *r)
{return r&&r->self==r&&r->constructed&&r->config.report_memory.allocate?&r->report:NULL;}
/* The association stage's record tail: counters and codes only, never an SSID, BSSID, key or credential byte. */
static void assoc_update(struct board_radio *r)
{
    if(!r->config.associate||r->wiped||!r->config.report_memory.allocate)return;
    struct a733_radio_assoc *a=&r->assoc;
    a->flags|=1u; /* the entropy is unqualified: bring-up only */
    a->link=(uint32_t)r->session.wifi.link;a->status=r->session.wifi.station_event.status;
    a->supplicant=(uint32_t)r->session.supplicant.state;a->rsn=(uint32_t)r->session.supplicant.rsn_state;
    a->eapol_rx=r->session.supplicant.eapol_rx;a->eapol_tx=r->session.supplicant.eapol_tx;
    if(r->cand_found)a->frequency_rssi=(uint32_t)r->cand_frequency|(uint32_t)(uint16_t)(int16_t)r->cand_rssi<<16;
    a733_radio_report_association(&r->report,a);
}
/* Zero with stores the compiler cannot remove, then clean the lines to DRAM: the runner reads DRAM, not the cache. */
static void scrub(void *memory,size_t bytes)
{
    volatile uint8_t *b=memory;
    for(size_t i=0;i<bytes;++i)b[i]=0;
#if defined(__aarch64__)
    for(uintptr_t a=(uintptr_t)memory&~(uintptr_t)63;a<(uintptr_t)memory+bytes;a+=64)
        __asm__ volatile("dc civac, %0"::"r"(a):"memory");
    __asm__ volatile("dsb sy":::"memory");
#endif
}
#define SCRUB(x) scrub(&(x),sizeof(x))
static void wipe_owner_secrets(struct board_radio *r)
{
    if(!r->config.associate)return;
    SCRUB(r->config.target);SCRUB(r->config.security);
    SCRUB(r->cand_bssid);SCRUB(r->cand_rsn);SCRUB(r->startup_event);SCRUB(r->diagnostic);
}
/* Every place a credential-derived byte lives, once the module is down or the failure shutdown is done and the session is
 * never polled again: the SSID in the target and in the management layer's request, held records and scan results; the
 * PMK and the PTK/GTK in the supplicant (whose slots are its only heap), the key store and the transport's buffers; the
 * dispatcher's queued events and the owner's copies of them; the entropy reservoir. Kept: what the record and the later
 * failure notes read (the report, the platform, the session's failure site and firmware progress). EL1 only. */
static void wipe_secrets(struct board_radio *r)
{
    if(!r->config.associate||r->wiped)return;
    r->wiped=1;
    wipe_pmk(r);
    wipe_owner_secrets(r);
    struct a733_radio_session *s=&r->session;
    SCRUB(s->transport.tx);SCRUB(s->transport.rx);SCRUB(s->transport.confirmations);SCRUB(s->transport.unrelated);
    SCRUB(s->transport.setup_confirmation);SCRUB(s->transport.last_result);
    SCRUB(s->wifi);SCRUB(s->data);SCRUB(s->security);SCRUB(s->supplicant);SCRUB(s->dispatcher);
    SCRUB(s->reconnect_target);SCRUB(s->reconnect_security);
    if(os_ce_entropy_wipe)os_ce_entropy_wipe();
}
static void wipe_pmk(struct board_radio *r)
{
    volatile uint8_t *a=r->config.security.pmk,*b=r->session.supplicant.config.pmk;
    for(unsigned i=0;i<32;++i){a[i]=0;b[i]=0;}
}
/* A scan result that carries the credential's SSID and an RSN element offering PSK and CCMP. The element is found in the
 * beacon or probe response (information elements from offset 36), copied, and kept for the strongest such result. */
static void assoc_candidate(struct board_radio *r,const struct aic8800_wifi_scan_result *sc)
{
    const uint8_t *ssid,*pmk,*password;size_t ssid_bytes,password_bytes;
    if(!sc->has_ssid||!ss_wifi_credential||ss_wifi_credential(&ssid,&ssid_bytes,&pmk,&password,&password_bytes)!=0)return;
    r->assoc.flags|=32u;
    if(ssid_bytes!=sc->ssid_bytes||memcmp(ssid,sc->ssid,ssid_bytes))return;
    ++r->assoc.candidates;r->assoc.flags|=2u;
    if(!sc->frame||sc->frame_bytes<=36)return;
    const uint8_t *ie=sc->frame+36,*end=sc->frame+sc->frame_bytes;
    while(end-ie>=2&&(size_t)(end-ie)>=2u+ie[1]){
        if(ie[0]==48){
            size_t n=2u+ie[1];
            struct aic8800_supplicant_config *t=&r->config.security;
            if(n<=sizeof(t->ap_rsn)){
                memset(t->ap_rsn,0,sizeof(t->ap_rsn));memcpy(t->ap_rsn,ie,n);t->ap_rsn_bytes=(uint16_t)n;
                if(aic8800_supplicant_config_valid(t)){
                    r->assoc.flags|=4u;
                    if(!r->cand_found||sc->rssi_dbm>r->cand_rssi){
                        memcpy(r->cand_bssid,sc->bssid,6);memcpy(r->cand_rsn,t->ap_rsn,n);r->cand_rsn_bytes=(uint16_t)n;
                        r->cand_frequency=sc->frequency;r->cand_rssi=sc->rssi_dbm;r->cand_found=1;
                        r->assoc.stage=r->assoc.stage<1?1:r->assoc.stage;
                    }
                }
            }
            break;
        }
        ie+=2u+ie[1];
    }
    memset(r->config.security.ap_rsn,0,sizeof(r->config.security.ap_rsn));r->config.security.ap_rsn_bytes=0;
}
/* Build the target and the supplicant configuration from the candidate and the credential, at EL1. 0 if any piece is
 * missing; the PMK is copied only into the supplicant configuration, which is zeroed once the supplicant has taken it. */
static int assoc_build(struct board_radio *r)
{
    const uint8_t *ssid,*pmk,*password;size_t ssid_bytes,password_bytes;
    if(!ss_wifi_credential||ss_wifi_credential(&ssid,&ssid_bytes,&pmk,&password,&password_bytes)!=0||!pmk||
        !ssid_bytes||ssid_bytes>sizeof(r->config.target.ssid))return 0;
    r->assoc.flags|=32u;
    unsigned index=~0u;
    for(unsigned i=0;i<r->config.session.wifi.channel_count;++i)
        if(r->config.session.wifi.channels[i].frequency==r->cand_frequency){index=i;break;}
    if(index==~0u)return 0;
    struct aic8800_wifi_target *t=&r->config.target;
    memset(t,0,sizeof(*t));
    t->connection_authority=r->config.security.beacon_record;t->beacon_evidence=r->config.security.beacon_record;
    t->channel_index=index;t->security=AIC8800_WIFI_WPA2_PSK_CCMP;
    memcpy(t->bssid,r->cand_bssid,6);memcpy(t->ssid,ssid,ssid_bytes);t->ssid_bytes=(uint8_t)ssid_bytes;
    struct aic8800_supplicant_config *c=&r->config.security;
    memcpy(c->ap_rsn,r->cand_rsn,r->cand_rsn_bytes);c->ap_rsn_bytes=r->cand_rsn_bytes;memcpy(c->pmk,pmk,32);
    if(!aic8800_supplicant_config_valid(c)){wipe_pmk(r);return 0;}
    r->assoc.flags|=8u;
    return 1;
}
/* Keep the report current: the platform's stages and card facts every pass,
 * the firmware and identity once the management layer exists. */
static void report_progress(struct board_radio *r)
{
    if(!r->config.report_memory.allocate)return;
    a733_radio_report_platform(&r->report,&r->platform);
    if(r->session.transport.have[0]&&r->session.transport.have[1])
        a733_radio_report_identity(&r->report,r->session.transport.identity.chip_word,
            r->session.transport.identity.subrevision_word);
    a733_radio_report_firmware_progress(&r->report,&r->session.firmware,r->session.transport.config.ticks_per_ms);
    if(r->session.firmware.state==AIC8800_FW_START_CONFIRMED)a733_radio_report_firmware(&r->report);
    /* Identity is read before the firmware loads; keep the later stage. */
    assoc_update(r);
}
enum board_radio_result board_radio_stop_result(struct board_radio *r,enum a733_radio_session_result result)
{
    if(!r||r->self!=r||!r->constructed||xTaskGetCurrentTaskHandle()!=r->config.session.owner_task||
       (result!=A733_SESSION_DOWN&&result!=A733_SESSION_RETAINED&&result!=A733_SESSION_INVALID))return BOARD_RADIO_INVALID;
    if(result==A733_SESSION_DOWN&&(!r->session.stop_requested||r->platform.stage!=RSTAGE_DOWN||
       r->platform.fault||r->platform.pmic_request.id))return BOARD_RADIO_INVALID;
    if(!r->wiped){
        report_progress(r);
        if(r->config.report_memory.allocate)
            a733_radio_report_session_failure(&r->report,r->session.fail_line,r->session.fail_stage);
        /* A refused stop can still own an accepted SDIO transfer. Preserve
         * its protocol storage and ownership until physical drain is proved. */
        if(result==A733_SESSION_DOWN)wipe_secrets(r);
        else wipe_owner_secrets(r);
    }
    if(result==A733_SESSION_DOWN){r->stage=DOWN;return BOARD_RADIO_DOWN;}
    atomic_store_explicit(&r->failed,1,memory_order_release);return BOARD_RADIO_RETAINED;
}
enum board_radio_result board_radio_poll(struct board_radio *r)
{
    if(!r||r->self!=r||!r->constructed||
        xTaskGetCurrentTaskHandle()!=r->config.session.owner_task)return BOARD_RADIO_INVALID;
#ifdef SUNSTONE_A733_RADIO_IRQ
    if(!a733_radio_events_begin(&r->events))return retain(r);
    r->event_ready=0;r->event_before_stage=r->stage;
#endif
    if(atomic_load_explicit(&r->failed,memory_order_acquire)||
        (atomic_load_explicit(r->config.recovery_state,memory_order_acquire)&4))return retain(r);
    enum a733_radio_entropy_result entropy=A733_RADIO_ENTROPY_PENDING;
    if(!r->config.scan_only){
        entropy=a733_radio_entropy_poll(&r->entropy);
        if(entropy==A733_RADIO_ENTROPY_RETAINED||entropy==A733_RADIO_ENTROPY_INVALID||
            atomic_load_explicit(&r->entropy.fault,memory_order_acquire)||
            atomic_load_explicit(&r->config.entropy.source->engine->fault,memory_order_acquire))return retain(r);
    }
    /* The entropy stage runs outside the radio platform and session (apps/os/ce_probe.c): nothing is polled, so the
     * module stays unpowered. */
    if(r->config.entropy_only){r->stage=SCANNED;return BOARD_RADIO_SCANNED;}
    if(r->config.associate&&r->stage!=SHUTTING_DOWN&&r->stage!=DOWN){
        /* The unqualified CE entropy of the association stage runs in this task beside the radio. A failure ends the stage
         * as a clean radio failure with the record. */
        if(!os_ce_entropy_begin||!os_ce_entropy_step||!os_ce_entropy_ready||!os_ce_entropy_failed||!os_ce_entropy_bytes){
            r->assoc.flags|=64u;return retain(r);}
        if(!r->entropy_begun){os_ce_entropy_begin();r->entropy_begun=1;}
        os_ce_entropy_step();
        if(os_ce_entropy_failed()){r->assoc.flags|=64u;return retain(r);}
    }
    uint64_t now;
    if(!a733_counter_clock_now(&r->config.shared->counter,&now))return retain(r);
    if(r->stage==SHUTTING_DOWN||r->stage==DOWN){
        /* Only the platform's shutdown runs; the session and transport are not polled again. */
        enum a733_radio_platform_result down=a733_radio_platform_poll(&r->platform);
        if(down==A733_RADIO_RETAINED||down==A733_RADIO_INVALID)return retain(r);
        if(down==A733_RADIO_DOWN){
            if(!r->wiped)report_progress(r);
            r->stage=DOWN;wipe_secrets(r); /* nothing polls the session again */
            return BOARD_RADIO_DOWN;
        }
        return publish(r,BOARD_RADIO_SCANNED);
    }
    if(r->stage!=ONLINE&&r->stage!=SCANNED&&now>=r->config.startup_deadline_us)return retain(r);
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
        if(now-start>=BOARD_RADIO_POLL_BUDGET_US){
#ifdef SUNSTONE_A733_RADIO_IRQ
            r->event_ready=1;
#endif
            break;
        }
    }
    if(result==A733_SESSION_RETAINED||result==A733_SESSION_INVALID)return retain(r);
    if(result==A733_SESSION_BUSY)return publish(r,BOARD_RADIO_PENDING);
    report_progress(r);
    if(r->session.dispatcher.initialized){
        /* Station events of a reconnect or recovery would otherwise fill the
         * event queue and stop receive parsing. A staged owner records every
         * scan result as it arrives: the queue holds one result at a time and
         * applies backpressure, so none is lost. */
        while((r->stage!=ONLINE||r->recovering)&&
            a733_radio_session_take_event(&r->session,&r->startup_event)){
            if(r->stage!=ONLINE&&r->startup_events!=UINT64_MAX)++r->startup_events;
            if(r->startup_event.kind==AIC8800_DISPATCH_SCAN&&r->config.report_memory.allocate){
                /* The association stage's record names no SSID, so the list carries none. */
                unsigned named=r->startup_event.metadata.scan.has_ssid&&!r->config.associate;
                a733_radio_report_ap(&r->report,r->startup_event.metadata.scan.bssid,
                    named?r->startup_event.metadata.scan.ssid:NULL,
                    named?r->startup_event.metadata.scan.ssid_bytes:0,
                    r->startup_event.metadata.scan.frequency,r->startup_event.metadata.scan.rssi_dbm);
                if(r->config.associate&&r->stage==SCANNING)assoc_candidate(r,&r->startup_event.metadata.scan);
            }
            if(!r->config.report_memory.allocate)break; /* One event per pass, as before. */
        }
        if(a733_radio_session_take_record(&r->session,&r->diagnostic)&&
            r->diagnostic_records!=UINT64_MAX)++r->diagnostic_records;
    }
    if(r->stage==ONLINE){
        if(result==A733_SESSION_LOST&&!recover(r,now))return retain(r);
        if(result==A733_SESSION_NETWORK)r->recovering=0;
        return publish(r,!atomic_load(&r->session.sockets.offline)&&aic8800_network_ready(&r->session.sockets.network)?
            BOARD_RADIO_NETWORK:BOARD_RADIO_PENDING);
    }
    if(result==A733_SESSION_ENUMERATED){
        /* Enumeration-only session: the card is enumerated and borrowed; this is its end state. */
        r->stage=SCANNED;report_progress(r);return publish(r,BOARD_RADIO_SCANNED);
    }
    if(result!=A733_SESSION_MANAGEMENT)return publish(r,BOARD_RADIO_PENDING);
    if(now>=r->session.deadline)return retain(r);
    if(r->stage==SCANNED)return publish(r,BOARD_RADIO_SCANNED);
    if(r->stage==STARTING&&r->config.report_memory.allocate&&!r->scan_started){
        uint64_t deadline=bounded_deadline(r,now,r->config.scan_timeout_us);
        if(a733_radio_session_scan(&r->session,deadline)){
            r->scan_started=1;r->operation_deadline=deadline;r->stage=SCANNING;
        }
    }else if(r->stage==SCANNING){
        if(now>=r->operation_deadline)return retain(r);
        struct aic8800_wifi_scan_summary summary;
        if(a733_radio_session_take_scan(&r->session,&summary)){
            a733_radio_report_scan_done(&r->report,summary.reported_results);
            r->scan_done=1;r->stage=r->config.scan_only&&!r->config.associate?SCANNED:STARTING;
            if(r->stage==SCANNED)return publish(r,BOARD_RADIO_SCANNED);
        }
    }else if(r->stage==STARTING&&r->config.associate){
        if(!r->cand_found)return retain(r); /* no scan result carried the credential's SSID with a usable RSN element */
        if(!os_ce_entropy_ready())return publish(r,BOARD_RADIO_PENDING);
        r->assoc.stage=2;r->assoc.flags|=16u;
        if(!assoc_build(r))return retain(r);
        uint64_t deadline=bounded_deadline(r,now,r->config.connect_timeout_us);
        if(a733_radio_session_connect(&r->session,&r->config.target,deadline)){
            r->operation_deadline=deadline;r->stage=CONNECTING;r->assoc.stage=3;r->assoc.ms_connect=(uint32_t)(now/1000u)&0xffffu;
        }
    }else if(r->stage==STARTING){
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
                r->operation_deadline=deadline;r->stage=SECURING;r->assoc.stage=5;
                if(r->config.associate)wipe_pmk(r);
            }
        }else if(r->session.wifi.link!=AIC8800_WIFI_CONNECTING)return retain(r);
        if(r->session.wifi.link==AIC8800_WIFI_SECURITY_PENDING&&r->assoc.stage<4)r->assoc.stage=4;
    }else if(r->stage==SECURING){
        if(now>=r->operation_deadline||r->session.supplicant.state==AIC8800_SUPPLICANT_FAULT)
            return retain(r);
        if(r->config.associate){
            if(r->session.wifi.link==AIC8800_WIFI_SECURED&&r->session.supplicant.state==AIC8800_SUPPLICANT_AUTHORIZED){
                /* The stage's end: authorized, no address requested. */
                r->assoc.stage=6;r->assoc.ms_authorized=(uint32_t)(now/1000u)&0xffffu;
                wipe_pmk(r);r->stage=SCANNED;report_progress(r);return publish(r,BOARD_RADIO_SCANNED);
            }
        }else if(r->session.wifi.link==AIC8800_WIFI_SECURED&&
            r->session.supplicant.state==AIC8800_SUPPLICANT_AUTHORIZED&&
            a733_radio_session_start_network(&r->session,&r->config.network)){
            r->stage=ONLINE;atomic_store_explicit(&r->network_started,1,memory_order_release);
#ifdef SUNSTONE_A733_RADIO_IRQ
            if(r->config.network.clients[0])xTaskNotifyGive(r->config.network.clients[0]);
#endif
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
        a733_ranges_disjoint(r,sizeof(*r),request,sizeof(*request))&&a733_ranges_disjoint(r,sizeof(*r),ticket,sizeof(*ticket))&&
        sunstone_net_submit(&r->session.sockets,request,ticket);
}
int board_radio_receive(struct board_radio *r,struct sunstone_net_reply *reply,TickType_t wait)
{
    return r&&r->self==r&&r->constructed&&atomic_load_explicit(&r->network_started,memory_order_acquire)&&
        a733_ranges_disjoint(r,sizeof(*r),reply,sizeof(*reply))&&
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
        !a733_ranges_disjoint(r,sizeof(*r),progress,sizeof(*progress))||!a733_ranges_disjoint(r,sizeof(*r),deadline,sizeof(*deadline)))return -1;
    if(!a733_ranges_disjoint(progress,sizeof(*progress),deadline,sizeof(*deadline)))return -1;
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
/* A scan-only profile (version 7) needs no entropy qualification, crypto engine,
 * credential or TCP exchange, and association_timeout_us bounds its scan. Every
 * other field keeps its meaning. */
static int startup_profile_valid(const struct board_radio_profile *f,const uint8_t expected[32],
    const struct board_radio_chip_id *id)
{
    unsigned scan=board_radio_profile_scan_only(f);
    if(!f||!expected||!id||
        f->magic!=BOARD_RADIO_PROFILE_MAGIC||(f->version!=BOARD_RADIO_PROFILE_VERSION&&f->version!=BOARD_RADIO_PROFILE_LISTENER_VERSION&&f->version!=BOARD_RADIO_PROFILE_SCAN_VERSION)||f->bytes!=sizeof(*f)||!board_radio_profile_network_valid(f)||
        !f->platform_authority||
        !evidence(f->platform_evidence)||
        !id->secure_read_record)return 0;
    /* A scan-only profile is not bound to one board: the firmware's binding check reads the chip
     * identity from the secure reader, and the profile's own copy is unused. */
    if(!scan&&memcmp(f->chip_id,id->words,sizeof(f->chip_id)))return 0;
    if(scan){
        if(board_radio_profile_stage_limit(f)==BOARD_RADIO_STAGE_ASSOCIATE&&
            (!f->refill_timeout_us||!f->security_timeout_us||!f->command_timeout_us))return 0;
        if(f->tcp_mode||!f->association_timeout_us||f->startup_deadline_us<f->bootstrap_deadline_us||
            f->startup_deadline_us>f->platform.lifetime_deadline_us)return 0;
    }else if(!f->ce_authority||!f->secure_ce_authority||
        !evidence(f->ce_evidence)||!evidence(f->entropy_evidence)||
        !f->entropy_record||!f->characterization_record||!f->startup_test_record||!f->health_enable_epoch||
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
        !startup_profile_valid(f,expected,id)||!a733_ranges_disjoint(r,sizeof(*r),providers,provider_bytes)||
        !a733_ranges_disjoint(r,sizeof(*r),f,sizeof(*f)))return 0;
    if(board_radio_profile_scan_only(f)){
        /* Scan-only: no crypto engine or entropy source is built, and the first-run guard is asked for
         * until the supply route is confirmed on the board. */
        p->used=1;p->shared=shared;p->profile=f;
        struct board_radio_config sc={.shared=shared,.platform=f->platform,.chip_id=id,.recovery_state=recovery_state,
            .network={.bootstrap=owner,.clients={client}},
            .admit=radio_authorize,.admission_context=p,.startup_deadline_us=f->startup_deadline_us,
            .scan_only=1,.first_run_guard=1,.scan_timeout_us=f->association_timeout_us,
            .associate=board_radio_profile_stage_limit(f)==BOARD_RADIO_STAGE_ASSOCIATE,
            .connect_timeout_us=f->refill_timeout_us,.security_timeout_us=f->security_timeout_us,
            .enumeration_only=board_radio_profile_stage_limit(f)!=BOARD_RADIO_STAGE_SCAN&&
                board_radio_profile_stage_limit(f)!=BOARD_RADIO_STAGE_ASSOCIATE,
            .security={.entropy=os_ce_entropy_bytes,.entropy_record=f->platform_authority,.beacon_record=f->platform_authority,
                .ticks_per_second=1000000,.command_timeout=f->command_timeout_us,.key_mgmt=AIC8800_SUPPLICANT_PSK},
            .entropy_only=board_radio_profile_stage_limit(f)==BOARD_RADIO_STAGE_ENTROPY,
            .four_bit=board_radio_profile_stage_limit(f)==BOARD_RADIO_STAGE_FIRMWARE, /* the firmware upload needs 4 bits at 25 MHz */
            .report_memory={report_allocate,report_release,NULL}};
        sc.session=(struct a733_radio_session_config){.owner_task=owner,.owner=f->session_owner,
            .bootstrap_deadline=f->bootstrap_deadline_us,.runtime_lease_us=f->runtime_lease_us,
            .renew_margin_us=f->renew_margin_us,.transport=f->transport,.bootstrap=f->bootstrap,
            .firmware={.identity=f->radio_identity,.profile=1},.runtime={.binding_timeout=f->binding_timeout_us},.wifi=f->wifi,
            .enumeration_only=sc.enumeration_only,
            .identity_only=board_radio_profile_stage_limit(f)>=BOARD_RADIO_STAGE_IDENTITY&&
                board_radio_profile_stage_limit(f)!=BOARD_RADIO_STAGE_ASSOCIATE,
            .firmware_only=board_radio_profile_stage_limit(f)==BOARD_RADIO_STAGE_FIRMWARE};
        for(unsigned i=0;i<4;++i)sc.session.firmware.blobs[i]=(struct aic8800_firmware_blob){
            (const uint8_t *)(uintptr_t)f->firmware_address[i],(size_t)f->firmware_bytes[i]};
        if(!board_radio_init(r,&sc))return 0;
        r->startup_profile=f;return 1;
    }
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
 atomic_store_explicit(&r->failed,1,memory_order_release);
#ifdef SUNSTONE_A733_RADIO_IRQ
 xTaskNotifyGive(r->events.owner);
#endif
 return -1;}
int board_radio_client_result(const struct board_radio *r,unsigned *verified)
{
    if(!r||r->self!=r||!r->constructed||!verified||!a733_ranges_disjoint(r,sizeof(*r),verified,sizeof(*verified)))return -1;
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
    uint64_t ticket;int submitted=board_radio_submit(r,&request,&ticket);
#ifdef SUNSTONE_A733_RADIO_IRQ
    r->client_retry=!submitted;
#endif
    if(submitted){r->client_ticket=ticket;r->client_pending=1;}
    return 0;
}
int board_radio_client_poll(struct board_radio *r)
{
    /* A scan-only session has no network and no client exchange: the client task idles. Reporting failure
     * here would revoke the owner's recovery state before its first poll. */
    if(r&&r->self==r&&r->constructed&&r->config.scan_only)return 0;
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
    int submitted=board_radio_submit(r,&copy,&ticket);
#ifdef SUNSTONE_A733_RADIO_IRQ
    r->client_retry=!submitted;
#endif
    if(submitted){r->client_ticket=ticket;r->client_pending=1;}
    return 0;
}
