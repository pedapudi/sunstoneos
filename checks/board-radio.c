/* Application orchestration only. Lower protocol proofs are reused; their
 * observations below are explicit checkpoints, not RF or entropy evidence. */
#include "../apps/board/radio.c"
#include "../drivers/net/a733_radio_result.c"
#include <assert.h>
#include <stdio.h>
static struct board_radio app;
static struct a733_native_usb shared;
static struct a733_ce_entropy source;
static struct a733_ce_session engine;
static const struct board_radio_chip_id id={7,{0x11223344,0x55667788,0x99aabbcc,0xddeeff00}};
static TaskHandle_t task=(TaskHandle_t)(uintptr_t)1;
static BaseType_t scheduler=taskSCHEDULER_NOT_STARTED;
static uint64_t time_us=100;
#ifdef EVENT_MISC_RADIO_COMPOSITION
static unsigned event_clock_cost,event_session_step;
#endif
static unsigned abort_model,abort_steps_left;
static unsigned entropy_calls,session_calls,retained,connects,security_calls,network_calls,submitted,received;
static unsigned entropy_retains;
static atomic_uint recovery_state;
static int entropy_failed,boundary;
static unsigned fast_requests,fast_refused;
static int client_model;
static unsigned listener_variant,listener_closes;
/* Socket requests that time out while the link is lost: the first SEND and
 * RECEIVE time out with nothing moved, the second with one late byte. */
static unsigned exchange_timeouts,timed_sends,timed_receives;
static struct sunstone_net_request copied_request;
static uint64_t copied_ticket;
static enum a733_radio_session_result session_result=A733_SESSION_MANAGEMENT;
/* ASan retains the unused native startup callback table. These boundaries
 * must never execute in the profile/coordinator check; native image owns them. */
int a733_platform_lock_enter(struct a733_platform_locks *p,enum a733_platform_lock l,uint64_t d)
{(void)p;(void)l;(void)d;assert(0);return 0;}
void a733_platform_lock_leave(struct a733_platform_locks *p,enum a733_platform_lock l)
{(void)p;(void)l;assert(0);}
int a733_platform_lock_retain(struct a733_platform_locks *p,enum a733_platform_lock l)
{(void)p;(void)l;assert(0);return 0;}
int a733_ce_native_read(void *p,uintptr_t a,uint32_t *v){(void)p;(void)a;(void)v;assert(0);return 0;}
int a733_ce_native_write(void *p,uintptr_t a,uint32_t v){(void)p;(void)a;(void)v;assert(0);return 0;}
int a733_ce_native_sync(void *p,struct a733_ce_memory *m,enum a733_ce_sync s)
{(void)p;(void)m;(void)s;assert(0);return 0;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return task;}
BaseType_t xTaskGetSchedulerState(void){return scheduler;}
int a733_counter_clock_now(void *p,uint64_t *n){assert(p==&shared.counter);*n=time_us;
#ifdef EVENT_MISC_RADIO_COMPOSITION
 time_us+=event_clock_cost;
#endif
 return 1;}
int a733_radio_entropy_init(struct a733_radio_entropy *e,const struct a733_radio_entropy_config *c)
{assert(e==&app.entropy&&c->source==&source&&c->clock==&shared.counter);atomic_init(&e->fault,0);return 1;}
enum a733_radio_entropy_result a733_radio_entropy_poll(struct a733_radio_entropy *e)
{assert(e==&app.entropy);++entropy_calls;if(entropy_failed){atomic_store(&e->fault,1);return A733_RADIO_ENTROPY_BUSY;}
 return A733_RADIO_ENTROPY_READY;}
enum a733_radio_entropy_result a733_radio_entropy_retain(struct a733_radio_entropy *e)
{assert(e==&app.entropy&&(atomic_load(&recovery_state)&4));
 unsigned stale=1;assert(!atomic_compare_exchange_strong(&recovery_state,&stale,3)&&(stale&4));
 ++entropy_retains;atomic_store(&e->fault,1);
 return entropy_retains==1?A733_RADIO_ENTROPY_BUSY:A733_RADIO_ENTROPY_RETAINED;}
int a733_radio_entropy_bytes(void *p,uint8_t *o,size_t n){(void)p;(void)o;(void)n;return 0;}
int a733_radio_entropy_random32(void *p,uint32_t *o){(void)p;(void)o;return 0;}
int a733_radio_entropy_sequence(void *p,uint32_t a,uint16_t b,uint32_t c,uint16_t d,uint32_t *o)
{(void)p;(void)a;(void)b;(void)c;(void)d;(void)o;return 0;}
int a733_radio_platform_native_init(struct a733_radio_platform_native *n,struct a733_platform_locks *l,
    void *owner,int (*admit)(void *,const struct a733_radio_platform_config *,const struct a733_clock_token *),void *p)
{assert(n==&app.native&&l==&shared.locks&&owner==task&&admit);(void)p;return 1;}
int a733_radio_platform_init(struct a733_radio_platform *p,const struct a733_radio_platform_config *c,
    const struct a733_radio_platform_ops *o,void *cookie,struct a733_clock_leases *cl,
    struct a733_pins *pi,struct a733_rail_leases *ra,struct a733_pmic *pm)
{assert(p==&app.platform&&c==&app.config.platform&&o==&app.native.operations&&cookie==&app.native&&
 cl==&shared.clocks&&pi==&shared.pins&&ra==&shared.rails&&pm==&shared.pmic);p->config=*c;return 1;}
int a733_radio_platform_four_bit(struct a733_radio_platform *p)
{assert(p==&app.platform&&!p->four_bit);++fast_requests;if(fast_refused)return 0;p->four_bit=1;return 1;}
int a733_radio_session_init(struct a733_radio_session *s,struct a733_radio_platform *p,
    const struct a733_radio_session_config *c)
{
    assert(s==&app.session&&p==&app.platform&&c->owner_task==task&&(p->four_bit||c->enumeration_only));
    uint8_t output[16];assert(c->runtime.read_platform_id(c->runtime.context,output));
    assert(output[0]==0x44&&output[3]==0x11&&output[15]==0xdd);
    s->deadline=9000;return 1;
}
enum a733_radio_session_result a733_radio_session_poll(struct a733_radio_session *s)
{assert(s==&app.session&&(app.config.scan_only||entropy_calls>session_calls));++session_calls;
#ifdef EVENT_MISC_RADIO_COMPOSITION
 if(event_session_step){++s->stage;return A733_SESSION_PENDING;}
#endif
 return session_result;}
enum a733_radio_session_result a733_radio_session_retain(struct a733_radio_session *s)
{assert(s==&app.session);++retained;s->fault=1;
 if(app.platform.refused)app.platform.fault=1;
 if(abort_model&&!app.platform.aborting&&!app.platform.abort_done){app.platform.aborting=1;app.platform.deadline=time_us+2000000;}
 return A733_SESSION_RETAINED;}
/* The association stage's collaborators: the stored credential, the CE reservoir and the supplicant's checks. */
static struct aic8800_wifi_target assoc_target;
static struct aic8800_supplicant_config assoc_security;
static unsigned entropy_begins,entropy_steps,entropy_is_ready,entropy_is_failed;
static const uint8_t cred_ssid[14]={'w','i','p','e','-','p','r','o','b','e','-','n','e','t'},
    cred_pmk[32]={0x31,0x38,0x3f,0x46,0x4d,0x54,0x5b,0x62,0x69,0x70,0x77,0x7e,0x85,0x8c,0x93,0x9a,
    0xa1,0xa8,0xaf,0xb6,0xbd,0xc4,0xcb,0xd2,0xd9,0xe0,0xe7,0xee,0xf5,0xfc,0x03,0x0a};
static unsigned cred_present=1;
void os_ce_entropy_begin(void){++entropy_begins;}
void os_ce_entropy_step(void){++entropy_steps;}
int os_ce_entropy_ready(void){return entropy_is_ready;}
int os_ce_entropy_failed(void){return entropy_is_failed;}
/* The reservoir holds ce_bytes bytes; each draw takes from it, all or nothing, as apps/os/ce_probe.c does. */
static size_t ce_bytes;static unsigned ce_draws;
int os_ce_entropy_bytes(void *c,uint8_t *o,size_t n)
{(void)c;if(n>ce_bytes)return 0;for(size_t i=0;i<n;++i)o[i]=(uint8_t)(0xa0+ce_draws*n+i);ce_bytes-=n;++ce_draws;return 1;}
int ss_wifi_credential(const uint8_t **ssid,size_t *n,const uint8_t **pmk,const uint8_t **pw,size_t *pn)
{
    *ssid=NULL;*n=0;*pmk=NULL;*pw=NULL;*pn=0;
    if(!cred_present)return -2;
    *ssid=cred_ssid;*n=sizeof(cred_ssid);*pmk=cred_pmk;return 0;
}
/* The RSN element the modeled beacons carry: version 1, group CCMP, pairwise CCMP, key management PSK, no capability
 * bits. Cases replace it, and test_parse_result stands in for wpa_parse_wpa_ie_rsn's result. */
static const uint8_t rsn_psk_ccmp[22]={48,20,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,2,0,0};
static const uint8_t *test_rsn=rsn_psk_ccmp,*test_vendor;
static size_t test_rsn_bytes=sizeof(rsn_psk_ccmp),test_vendor_bytes;
static int test_parse_result;
int aic8800_supplicant_rsn_parse(const uint8_t *element,size_t bytes)
{assert(element&&bytes>=2);return test_parse_result;}
/* Valid: a nonzero entropy callback and records, an element the parser accepts, and one that meets every RSN
 * condition of the PSK test. checks/radio-rsn-summary.c checks that the classifier agrees with the real test. */
int aic8800_supplicant_config_valid(const struct aic8800_supplicant_config *c)
{
    struct a733_radio_rsn s;a733_radio_rsn_summarize(&s,c->ap_rsn,c->ap_rsn_bytes);
    return c->entropy&&c->entropy_record&&c->beacon_record&&!test_parse_result&&a733_radio_rsn_classify(&s)==A733_RADIO_RSN_PASSED;
}
void aic8800_security_zero(void *p,size_t n){memset(p,0,n);}
int a733_radio_session_recover(struct a733_radio_session *s,const struct aic8800_wifi_target *t,
    const struct aic8800_supplicant_config *c,uint64_t reason,uint64_t deadline)
{(void)s;(void)t;(void)c;(void)reason;(void)deadline;return 0;}
int a733_radio_session_connect(struct a733_radio_session *s,const struct aic8800_wifi_target *t,uint64_t deadline)
{if(app.config.associate){(void)deadline;assoc_target=*t;++connects;s->wifi.link=AIC8800_WIFI_CONNECTING;
 /* What the management layer and transport keep: the stored target, the connect command and the queued records. */
 s->wifi.target=*t;memcpy(s->wifi.request+20,t->ssid,t->ssid_bytes);memcpy(s->transport.tx+7,t->ssid,t->ssid_bytes);
 memcpy((uint8_t *)&s->dispatcher+16,t->ssid,t->ssid_bytes);memcpy(s->wifi.station_payload+3,t->ssid,t->ssid_bytes);return 1;}
 assert(s==&app.session&&t==&app.config.target&&deadline==1100);++connects;s->wifi.link=AIC8800_WIFI_CONNECTING;return 1;}
int a733_radio_session_start_security(struct a733_radio_session *s,const struct aic8800_supplicant_config *c,uint64_t d)
{if(app.config.associate){(void)d;assoc_security=*c;++security_calls;s->wifi.link=AIC8800_WIFI_CONNECTING;
 /* The supplicant keeps the configuration and derives keys into its slots; the transport queues key records. */
 s->supplicant.config=*c;memcpy(s->supplicant.small[0]+40,c->pmk,32);memcpy(s->supplicant.large[0]+100,c->pmk,32);
 memcpy(s->transport.rx+9,c->pmk,32);memcpy(s->security.keys,c->pmk,32);
 s->transport.security=&s->security;s->transport.supplicant=&s->supplicant;return 1;}
 assert(s==&app.session&&c->entropy==a733_radio_entropy_bytes&&c->entropy_context==&app.entropy&&d==2100);
 ++security_calls;return boundary;}
/* The address stage's network: it starts once the session admits it, and the IP task's fence completes on request. */
static unsigned network_admit=1,net_offline_calls,net_offline_ok=1,park_calls,park_ok=1,net_has_address=1,net_events;
int sunstone_net_offline(struct sunstone_net *s)
{assert(s==&app.session.sockets&&app.config.address);++net_offline_calls;if(!net_offline_ok)return 0;atomic_store(&s->offline,1);return 1;}
int a733_radio_session_park_network(struct a733_radio_session *s)
{assert(s==&app.session);++park_calls;return park_ok&&atomic_load(&s->sockets.offline);}
int aic8800_network_address(const struct aic8800_network *n,uint8_t v[4])
{assert(n==&app.session.sockets.network&&!v);return net_has_address;}
void sunstone_net_event(void){++net_events;}
int a733_radio_session_start_network(struct a733_radio_session *s,const struct sunstone_net_config *c)
{if(app.config.address){
 /* The address stage's network: DHCP, and random words from the owner's pre-drawn ring, not the qualified source. */
 assert(s==&app.session&&c->network.transport==&s->transport&&c->network.dispatcher==&s->dispatcher&&c->network.dhcp==1&&
    c->network.entropy_context==&app&&c->network.random32&&c->network.random32!=a733_radio_entropy_random32&&
    c->network.sequence!=a733_radio_entropy_sequence&&c->bootstrap==task&&c->clients[0]==task);
 if(!network_admit)return 0;
 ++network_calls;s->sockets.initialized=1;return 1;}
 assert(s==&app.session&&c->network.transport==&s->transport&&c->network.dispatcher==&s->dispatcher&&
 c->network.random32==a733_radio_entropy_random32&&c->network.sequence==a733_radio_entropy_sequence&&
 c->network.entropy_context==&app.entropy);++network_calls;return 1;}
int aic8800_network_ready(const struct aic8800_network *n){assert(n==&app.session.sockets.network);return 1;}
/* Staged-service model: scan results delivered as events, then a finished scan. */
static unsigned staged,guard_calls,scan_calls,take_scan_calls,scan_events_left,scan_finished,scan_admit=1,allocations;
int a733_radio_platform_first_run_guard(struct a733_radio_platform *p)
{assert(p==&app.platform);++guard_calls;return 1;}
int a733_radio_session_scan(struct a733_radio_session *s,uint64_t deadline)
{assert(s==&app.session&&deadline==1100);++scan_calls;return scan_admit;}
int a733_radio_session_take_scan(struct a733_radio_session *s,struct aic8800_wifi_scan_summary *out)
{
    assert(s==&app.session);++take_scan_calls;
    if(!scan_finished)return 0;
    *out=(struct aic8800_wifi_scan_summary){.received=3,.reported_results=3,.accepted=1};return 1;
}
/* Scan events come from the real producers. Each sighting is a SCANU_RESULT_IND (0x1004) record as the firmware
 * delivers it (struct scanu_result_ind of the vendor's lmac_msg.h: length, frame control, centre frequency, band,
 * station index, interface, RSSI, then the beacon), placed in the transport's receive buffer. The real SDIO codec
 * decodes it, the real Wi-Fi scan parser takes it, and the real dispatcher turns it into the event the application
 * takes, so the event's metadata and payload are exactly what aic8800_dispatch.c builds. Only the transport around
 * them is modelled: it holds one received aggregate and follows the runtime branch of the transport's parse(). */
static struct sdio_card scan_card;
static struct sunxi_sdio scan_host;
static uint64_t scan_now=1000;
static const struct aic8800_dispatch_event *last_event;
int aic8800_transport_claim_receive(struct aic8800_sdio_transport *t,const void *owner,size_t bytes)
{if(!t->initialized||t->receive_owner||!owner||!bytes)return 0;t->receive_owner=owner;t->receive_owner_bytes=bytes;return 1;}
int aic8800_transport_receive_view(const struct aic8800_sdio_transport *t,const void *owner,const uint8_t **record,size_t *available)
{if(t->receive_owner!=owner||t->held||t->cursor>=t->rx_bytes)return 0;*record=t->rx+t->cursor;*available=t->rx_bytes-t->cursor;return 1;}
int aic8800_transport_retire_record(struct aic8800_sdio_transport *t,const void *owner,const uint8_t *record,size_t consumed)
{if(t->receive_owner!=owner||t->held||record!=t->rx+t->cursor||consumed>t->rx_bytes-t->cursor)return 0;t->cursor+=consumed;return 1;}
void aic8800_transport_poll_receive(struct aic8800_sdio_transport *t,const void *owner)
{
    if(t->receive_owner!=owner||t->held||t->cursor>=t->rx_bytes)return;
    struct aic8800_sdio_response r;
    enum aic8800_codec_result c=aic8800_sdio_decode_identity_response(t->rx+t->cursor,t->rx_bytes-t->cursor,AIC8800_IDENTITY_CHIP,&r);
    if(c==AIC8800_CODEC_UNRELATED)c=aic8800_wifi_response(t->wifi,t,&r,++scan_now);
    if(t->wifi->state==AIC8800_WIFI_FAULT){t->state=AIC8800_TRANSPORT_FAULT;return;}
    if(c==AIC8800_CODEC_OK)t->cursor+=r.consumed;
    else if(c==AIC8800_CODEC_UNRELATED){t->unrelated=r;t->held=1;}
    else t->state=AIC8800_TRANSPORT_FAULT;
}
int aic8800_transport_peek(const struct aic8800_sdio_transport *t,struct aic8800_sdio_response *out)
{if(!t->held)return 0;*out=t->unrelated;return 1;}
int aic8800_transport_peek_data(const struct aic8800_sdio_transport *t,struct aic8800_data_frame *out){(void)t;(void)out;return 0;}
int aic8800_transport_consume_receive(struct aic8800_sdio_transport *t,const void *owner)
{
    if(t->receive_owner!=owner||!t->held||t->state==AIC8800_TRANSPORT_FAULT)return 0;
    aic8800_wifi_consumed(t->wifi,t,t->unrelated.record);t->cursor+=t->unrelated.consumed;t->held=0;return 1;
}
void aic8800_transport_fault_receive(struct aic8800_sdio_transport *t,const void *owner)
{if(t->receive_owner==owner)t->state=AIC8800_TRANSPORT_FAULT;}
/* The session's receive side after interface setup: the transport in its runtime state, the Wi-Fi worker waiting on
 * a scan it started with the real request path, and the dispatcher owning receive. */
static void scan_receive_start(struct a733_radio_session *s)
{
    memset(&s->transport,0,sizeof(s->transport));memset(&s->wifi,0,sizeof(s->wifi));memset(&s->dispatcher,0,sizeof(s->dispatcher));
    scan_card.host=&scan_host;
    struct aic8800_sdio_transport *t=&s->transport;
    t->initialized=1;t->state=AIC8800_TRANSPORT_RUNTIME;t->card=&scan_card;t->wifi=&s->wifi;
    struct aic8800_wifi_config c={.rf_profile_record=1,.channel_policy_record=1,.command_timeout=1000000,.profile=1,.channel_count=2,
        .channels={{.frequency=2437,.band=0,.flags=1},{.frequency=5180,.band=1,.flags=1}}};
    assert(aic8800_wifi_init(&s->wifi,&c)&&aic8800_wifi_claim(&s->wifi,t,scan_now,UINT64_MAX/2));
    s->wifi.state=AIC8800_WIFI_READY;s->wifi.vif=0;s->wifi.max_vifs=1; /* interface added */
    assert(aic8800_wifi_scan(&s->wifi,t,++scan_now,UINT64_MAX/2));
    aic8800_wifi_step(&s->wifi,t,++scan_now);assert(s->wifi.state==AIC8800_WIFI_REQUEST);
    assert(aic8800_wifi_sent(&s->wifi,t)&&s->wifi.state==AIC8800_WIFI_WAITING&&s->wifi.expected_cfm==0x1009);
    assert(aic8800_dispatch_init(&s->dispatcher,t));
}
/* One SCANU_RESULT_IND aggregate: a 16-byte control header, the 12-byte indication head and a beacon with 36 bytes
 * of header and fixed fields, an SSID element, any vendor element, then any RSN element. */
static size_t scan_record(uint8_t *p,uint16_t frequency,uint8_t band,int8_t rssi,const uint8_t bssid[6],
    const uint8_t *ssid,size_t ssid_bytes)
{
    uint8_t frame[160]={0x80,0};memset(frame+4,0xff,6);memcpy(frame+10,bssid,6);memcpy(frame+16,bssid,6);
    frame[32]=100;frame[34]=0x11; /* beacon interval 100 TU, ESS and privacy */
    size_t at=36;frame[at]=0;frame[at+1]=(uint8_t)ssid_bytes;memcpy(frame+at+2,ssid,ssid_bytes);at+=2+ssid_bytes;
    if(test_vendor){memcpy(frame+at,test_vendor,test_vendor_bytes);at+=test_vendor_bytes;}
    if(test_rsn){memcpy(frame+at,test_rsn,test_rsn_bytes);at+=test_rsn_bytes;}
    assert(at<=sizeof(frame));
    size_t parameters=12+at,length=12+parameters,total=4+((length+3)&~(size_t)3);
    memset(p,0,total+4); /* a zero length word ends the aggregate */
    p[0]=(uint8_t)length;p[1]=(uint8_t)(length>>8);p[2]=0x91;p[4]=0x04;p[5]=0x10;
    p[10]=(uint8_t)parameters;p[11]=(uint8_t)(parameters>>8);
    uint8_t *q=p+16;q[0]=(uint8_t)at;q[1]=(uint8_t)(at>>8);q[2]=0x80;q[4]=(uint8_t)frequency;q[5]=(uint8_t)(frequency>>8);
    q[6]=band;q[7]=0xff;q[8]=0;q[9]=(uint8_t)rssi;memcpy(q+12,frame,at);
    return total;
}
int a733_radio_session_take_event(struct a733_radio_session *s,struct aic8800_dispatch_event *e)
{
    assert(s==&app.session&&e==&app.startup_event);
    if(!staged)return 1;
    if(!scan_events_left)return 0;
    if(s->dispatcher.transport!=&s->transport)scan_receive_start(s);
    unsigned n=scan_events_left--; /* 3, 2, 1: the third sighting repeats the first access point. */
    static const uint8_t other_2[3]={'a','-','2'},other_5[3]={'b','-','5'};
    const uint8_t *ssid=n==2?other_5:other_2;size_t ssid_bytes=3;
    if(app.config.associate&&n!=2){ssid=cred_ssid;ssid_bytes=sizeof(cred_ssid);}
    struct aic8800_sdio_transport *t=&s->transport;
    t->rx_bytes=scan_record(t->rx,n==2?5180:2437,n==2?1:0,(int8_t)(-40-(int)n),
        (const uint8_t *)(n==2?"\x02\x00\x00\x00\x00\x02":"\x02\x00\x00\x00\x00\x01"),ssid,ssid_bytes);
    t->cursor=0;
    aic8800_dispatch_poll(&s->dispatcher);
    assert(t->state==AIC8800_TRANSPORT_RUNTIME&&!s->dispatcher.faulted&&s->dispatcher.event_count==1&&!t->held);
    assert(aic8800_dispatch_take_event(&s->dispatcher,e));
    /* As aic8800_dispatch.c builds it: no frame pointer, the frame in the payload. */
    assert(e->kind==AIC8800_DISPATCH_SCAN&&!e->metadata.scan.frame&&e->bytes==e->metadata.scan.frame_bytes&&
        e->metadata.scan.has_ssid&&e->metadata.scan.ssid_bytes==ssid_bytes&&!memcmp(e->metadata.scan.ssid,ssid,ssid_bytes));
    last_event=e;
    return 1;
}
/* A bump arena: the freestanding image headers used by sibling checks provide no malloc. */
static _Alignas(16) uint8_t report_arena[8192];
static size_t report_used;
static void *arena_allocate(void *context,size_t bytes)
{
    assert(context==&allocations);++allocations;
    bytes=(bytes+15u)&~15u;if(bytes>sizeof(report_arena)-report_used)return NULL;
    void *p=report_arena+report_used;report_used+=bytes;return p;
}
static void arena_release(void *context,void *memory){assert(context==&allocations);(void)memory;}
/* The controller view at a fault: the SDIO interrupt latched and unmasked. */
static unsigned irq_views;
int a733_radio_platform_sdio_irq_view(struct a733_radio_platform *p,uint32_t *raw,uint32_t *mask)
{assert(p==&app.platform);++irq_views;*raw=1u<<16;*mask=1u<<16;return 1;}
/* Shutdown model: the platform steps PENDING twice, then DOWN. */
static unsigned shutdown_calls,shutdown_ok=1,platform_steps;
int a733_radio_platform_shutdown(struct a733_radio_platform *p,const struct a733_radio_borrow *t,uint64_t d)
{assert(p==&app.platform&&t==&app.session.borrow&&d);++shutdown_calls;return shutdown_ok;}
enum a733_radio_platform_result a733_radio_platform_poll(struct a733_radio_platform *p)
{assert(p==&app.platform);
 if(p->aborting){
     if(abort_steps_left&&--abort_steps_left==0){p->aborting=0;p->abort_done=1;p->abort_flags|=16u;p->fault=1;return A733_RADIO_RETAINED;}
     return A733_RADIO_PENDING;
 }
 return ++platform_steps<3?A733_RADIO_PENDING:A733_RADIO_DOWN;}
/* The scan-only startup never reaches the crypto engine or its mutex: these must not run. */
QueueHandle_t xQueueCreateMutexStatic(const uint8_t t,StaticQueue_t *q){(void)t;(void)q;assert(0);return NULL;}
BaseType_t xQueueSemaphoreTake(QueueHandle_t q,TickType_t t){(void)q;(void)t;assert(0);return 0;}
BaseType_t xQueueGenericSend(QueueHandle_t q,const void *i,TickType_t t,const BaseType_t p){(void)q;(void)i;(void)t;(void)p;assert(0);return 0;}
enum a733_clock_result a733_clock_acquire(struct a733_clock_leases *l,uint64_t o,a733_clock_mask_t s,a733_clock_mask_t e,uint64_t d,struct a733_clock_token *t)
{(void)l;(void)o;(void)s;(void)e;(void)d;(void)t;assert(0);return A733_CLOCK_FAULT;}
int a733_ce_entropy_native_init(struct a733_ce_entropy *e,const struct a733_ce_config *c,const struct a733_ce_ops *o,void *x,
    struct a733_clock_leases *l,const struct a733_clock_token *t)
{(void)e;(void)c;(void)o;(void)x;(void)l;(void)t;assert(0);return 0;}
/* The profile-level startup takes its scan list from the FreeRTOS heap. */
void *pvPortMalloc(size_t bytes){return arena_allocate(&allocations,bytes);}
void vPortFree(void *memory){arena_release(&allocations,memory);}
int a733_radio_session_take_record(struct a733_radio_session *s,struct aic8800_dispatch_record *d)
{assert(s==&app.session&&d==&app.diagnostic);return 1;}
int sunstone_net_submit(struct sunstone_net *s,const struct sunstone_net_request *r,uint64_t *ticket)
{assert(s==&app.session.sockets&&(app.config.address?r->operation==SUNSTONE_NET_RESOLVE&&r->timeout==app.config.resolve_timeout&&
    r->bytes==11&&!memcmp(r->payload,"example.com",11):r->timeout==3));++submitted;*ticket=42+submitted-1;
 copied_request=*r;copied_ticket=*ticket;return 1;}
/* The address stage's resolve reply: none until address_reply is set; the answer is a fixed address that the record and
 * the board object must not keep. */
static const uint8_t answer[4]={0x5d,0xb8,0xd7,0x0e};
static int address_reply=-1;
int sunstone_net_receive(struct sunstone_net *s,struct sunstone_net_reply *r,TickType_t wait)
{
 if(app.config.address){
    assert(s==&app.session.sockets&&!wait);if(address_reply<0)return 0;
    ++received;memset(r,0,sizeof(*r));r->ticket=copied_ticket;r->result=(enum sunstone_net_result)address_reply;
    if(address_reply==SUNSTONE_NET_OK)memcpy(r->address,answer,4);
    return 1;
 }
 assert(s==&app.session.sockets&&wait==(client_model?0:3));++received;memset(r,0,sizeof(*r));
 r->ticket=copied_ticket;r->result=client_model?SUNSTONE_NET_OK:SUNSTONE_NET_LINK_LOST;r->handle=5;
 if(client_model==2){
    r->handle=copied_request.operation==SUNSTONE_NET_OPEN_TCP?5:copied_request.handle;
    if(copied_request.operation==SUNSTONE_NET_ACCEPT){r->handle=6;r->port=60000;memcpy(r->address,app.startup_profile->peer_address,4);
        if(listener_variant==1)r->address[0]^=1;
        if(listener_variant==2)r->result=SUNSTONE_NET_TIMEOUT;
    }
    if(copied_request.operation==SUNSTONE_NET_RECEIVE&&listener_variant==3)r->result=SUNSTONE_NET_ADDRESS_LOST;
    if(copied_request.operation==SUNSTONE_NET_CLOSE){
        assert(copied_request.handle==(listener_closes?5u:6u)&&!atomic_load(&app.client_status));++listener_closes;
    }
 }
 uint16_t moved=copied_request.bytes<2?copied_request.bytes:2;
 if(exchange_timeouts&&(copied_request.operation==SUNSTONE_NET_SEND||copied_request.operation==SUNSTONE_NET_RECEIVE)){
    unsigned *count=copied_request.operation==SUNSTONE_NET_SEND?&timed_sends:&timed_receives;
    if(++*count<=2){r->result=SUNSTONE_NET_TIMEOUT;moved=(uint16_t)(*count-1);}
 }
 if(client_model==4&&copied_request.operation==SUNSTONE_NET_CONNECT)r->result=SUNSTONE_NET_TIMEOUT;
 if(client_model&&copied_request.operation==SUNSTONE_NET_SEND)r->stack_result=moved;
 if(client_model&&copied_request.operation==SUNSTONE_NET_RECEIVE){
    r->bytes=moved;memcpy(r->payload,app.startup_profile->response+app.client_received,moved);
 }return 1;
}
static int admission(void *p,const struct a733_radio_platform_config *c,const struct a733_clock_token *t)
{(void)p;(void)c;(void)t;return 0;}
int main(void)
{
    struct board_radio_profile admitted={.magic=BOARD_RADIO_PROFILE_MAGIC,.version=3,.bytes=sizeof(admitted),
        .platform_authority=1,.ce_authority=2,.secure_ce_authority=3,.entropy_record=4,
        .characterization_record=5,.startup_test_record=6,.health_enable_epoch=7,
        .platform_evidence={1},.ce_evidence={2},.entropy_evidence={3},
        .platform={.lifetime_deadline_us=10000},.ce={.profile=A733_CE_TRNG_ONLY},
        .startup_deadline_us=8000,.client_deadline_us=9500,.peer_port=80,.request_bytes=4,.response_bytes=4,
        .client_timeout_ticks=3,.firmware_address={0x43010000,0x43020000,0x43030000,0x43040000},
        .firmware_bytes={1708,24744,23288,341260}};
    memcpy(admitted.chip_id,id.words,sizeof(admitted.chip_id));
    uint8_t digest[32];storage_digest_bytes(&admitted,sizeof(admitted),digest);
    assert(startup_profile_valid(&admitted,digest,&id));
    digest[0]^=1;assert(!startup_profile_valid(&admitted,digest,&id));digest[0]^=1;
    struct board_radio_chip_id wrong=id;wrong.words[0]^=1;assert(!startup_profile_valid(&admitted,digest,&wrong));
    admitted.entropy_evidence[0]=0;storage_digest_bytes(&admitted,sizeof(admitted),digest);
    assert(!startup_profile_valid(&admitted,digest,&id));admitted.entropy_evidence[0]=3;
    admitted.ce.configure_health=1;storage_digest_bytes(&admitted,sizeof(admitted),digest);
    assert(!startup_profile_valid(&admitted,digest,&id));admitted.ce.configure_health=0;
    admitted.firmware_address[1]=admitted.firmware_address[0];storage_digest_bytes(&admitted,sizeof(admitted),digest);
    assert(!startup_profile_valid(&admitted,digest,&id));
    shared.used=1;source.engine=&engine;engine.binding.registry=&shared.clocks;
    atomic_init(&recovery_state,1);
    struct board_radio_config c={.shared=&shared,.chip_id=&id,.admit=admission,.recovery_state=&recovery_state,
        .platform={.lifetime_deadline_us=10000},.session={.owner_task=task,.bootstrap_deadline=500},
        .entropy={.source=&source,.owner=task,.clock=&shared.counter,.qualification_record=9},
        .target={.security=AIC8800_WIFI_WPA2_PSK_CCMP,.beacon_evidence=8},
        .security={.beacon_record=8,.entropy_record=9},.network={.bootstrap=task,.network={.entropy_record=9}},
        .startup_deadline_us=8000,.association_timeout_us=1000,.security_timeout_us=2000};
    c.chip_id=NULL;assert(!board_radio_init(&app,&c)&&!app.self);c.chip_id=&id;
    c.security.beacon_record=7;assert(!board_radio_init(&app,&c)&&!app.self);c.security.beacon_record=8;
    assert(board_radio_init(&app,&c));assert(!board_radio_init(&app,&c));
    struct sunstone_net_request request={.timeout=3};uint64_t ticket=0;
    struct sunstone_net_reply reply;
    assert(!board_radio_submit(&app,&request,&ticket));scheduler=taskSCHEDULER_RUNNING;
    task=(TaskHandle_t)(uintptr_t)2;assert(board_radio_poll(&app)==BOARD_RADIO_INVALID&&!entropy_calls);
    task=c.session.owner_task;
    app.session.dispatcher.initialized=1;
    assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&connects==1&&app.startup_events==1&&app.diagnostic_records==1);
    app.session.wifi.link=AIC8800_WIFI_SECURITY_PENDING;
    assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&security_calls==1&&app.stage==CONNECTING);
    boundary=1;assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==SECURING);
    app.session.wifi.link=AIC8800_WIFI_SECURED;app.session.supplicant.state=AIC8800_SUPPLICANT_AUTHORIZED;
    assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&network_calls==1);
    assert(board_radio_poll(&app)==BOARD_RADIO_NETWORK);
    assert(board_radio_submit(&app,&request,&ticket)&&ticket==42&&submitted==1);
    struct board_radio_profile profile={.client_deadline_us=9500,.client_timeout_ticks=3,
        .peer_port=80,.request_bytes=4,.response_bytes=4,.request={1,2,3,4},.response={5,6,7,8}};
    app.startup_profile=&profile;app.config.network.clients[0]=(TaskHandle_t)(uintptr_t)2;
    task=app.config.network.clients[0];client_model=1;
    unsigned iteration=0;
    while(board_radio_client_poll(&app)==0)assert(++iteration<15);
    assert(app.client_stage==5&&app.client_sent==4&&app.client_received==4&&
        copied_request.operation==SUNSTONE_NET_CLOSE&&app.client_handle==5);
    unsigned verified=0;assert(board_radio_client_result(&app,&verified)==1&&verified==4);
    /* SEND and RECEIVE timeouts during a link loss repeat within the client
     * deadline and keep their late bytes; a CONNECT timeout stays terminal. */
    app.client_stage=0;app.client_sent=app.client_received=0;atomic_store(&app.client_status,0);
    exchange_timeouts=1;iteration=0;
    while(board_radio_client_poll(&app)==0)assert(++iteration<25);
    assert(app.client_stage==5&&app.client_sent==4&&app.client_received==4&&timed_sends==4&&timed_receives==4);
    assert(board_radio_client_result(&app,&verified)==1&&verified==4);
    exchange_timeouts=0;client_model=4;app.client_stage=0;app.client_sent=app.client_received=0;
    atomic_store(&app.client_status,0);iteration=0;
    while(board_radio_client_poll(&app)==0)assert(++iteration<5);
    assert(copied_request.operation==SUNSTONE_NET_CONNECT&&atomic_load(&app.client_status)==2&&atomic_load(&app.failed));
    atomic_store(&app.failed,0);atomic_store(&recovery_state,1);
    client_model=0;task=c.session.owner_task;
    uint64_t progress=0,deadline=0;app.session.transport.completed_transfers=3;
    assert(board_radio_poll(&app)==BOARD_RADIO_NETWORK);
    assert(board_radio_watchdog(&app,&progress,&deadline)==1&&progress==3&&deadline==9000);
    entropy_failed=1;unsigned calls=session_calls;
    assert(board_radio_poll(&app)==BOARD_RADIO_RETAINED&&session_calls==calls&&retained==1);
    assert(!board_radio_submit(&app,&request,&ticket));
    assert(board_radio_receive(&app,&reply,3)&&reply.result==SUNSTONE_NET_LINK_LOST);
    assert(board_radio_watchdog(&app,&progress,&deadline)==-1&&board_radio_completed_transfers(&app)==0);
    unsigned polls=entropy_calls;assert(board_radio_poll(&app)==BOARD_RADIO_RETAINED&&entropy_calls==polls&&entropy_retains==2);
    unsigned expected=1;assert(!atomic_compare_exchange_strong(&recovery_state,&expected,3));
    assert((expected&4)&&atomic_load(&app.failed));
    assert(board_radio_external_failure(&app)==BOARD_RADIO_RETAINED&&entropy_calls==polls&&entropy_retains==3);
    /* The staged service with no credential: guard request, scan before anything else, no
     * entropy, connect or network, and a terminal SCANNED result with every access point recorded. */
    memset(&app,0,sizeof(app));staged=1;scan_events_left=3;scan_finished=0;report_used=0;
    unsigned entropy_before=entropy_calls,connects_before=connects,network_before=network_calls,security_before=security_calls;
    struct board_radio_config scan_only={.shared=&shared,.chip_id=&id,.admit=admission,.recovery_state=&recovery_state,
        .platform={.lifetime_deadline_us=10000},.session={.owner_task=task,.bootstrap_deadline=500},
        .startup_deadline_us=8000,.scan_timeout_us=1000,.scan_only=1,.first_run_guard=1,
        .report_memory={arena_allocate,arena_release,&allocations}};
    atomic_store(&recovery_state,1);atomic_store(&app.failed,0);scheduler=taskSCHEDULER_NOT_STARTED;
    scan_only.scan_timeout_us=0;assert(!board_radio_init(&app,&scan_only)&&!app.self);scan_only.scan_timeout_us=1000;
    scan_only.report_memory.allocate=NULL;assert(!board_radio_init(&app,&scan_only)&&!app.self);
    scan_only.report_memory.allocate=arena_allocate;
    assert(board_radio_init(&app,&scan_only)&&guard_calls==1&&board_radio_report(&app));
    scheduler=taskSCHEDULER_RUNNING;app.session.dispatcher.initialized=1;time_us=100;
    /* The net client task has nothing to do and must not fail the session: the first probe run retained the radio
     * at its first poll because this returned -1 and revoked the owner's recovery state. */
    task=(TaskHandle_t)(uintptr_t)5;
    assert(board_radio_client_poll(&app)==0&&!(atomic_load(&recovery_state)&4)&&!atomic_load(&app.failed));
    task=scan_only.session.owner_task;
    assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&scan_calls==1&&app.stage==SCANNING);
    assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==SCANNING&&take_scan_calls==1);
    assert(board_radio_report(&app)->ap_count==2&&board_radio_report(&app)->head.aps_reported==3);
    scan_finished=1;
    assert(board_radio_poll(&app)==BOARD_RADIO_SCANNED&&app.stage==SCANNED);
    assert(board_radio_poll(&app)==BOARD_RADIO_SCANNED&&scan_calls==1);
    {
        const struct a733_radio_report *report=board_radio_report(&app);
        assert(report->head.stage==A733_RADIO_STAGE_SCANNED&&!report->head.failed&&report->head.aps_reported==3);
        /* The scan's record keeps no SSID, though the scan heard both names. */
        static const uint8_t no_name[32];
        assert(report->ap_count==2&&!report->aps[0].ssid_bytes&&!memcmp(report->aps[0].ssid,no_name,32)&&
            report->aps[0].rssi_dbm==-41&&report->aps[0].channel==6);
        assert(!report->aps[1].ssid_bytes&&!memcmp(report->aps[1].ssid,no_name,32)&&report->aps[1].channel==36);
        uint8_t record[512];size_t bytes=a733_radio_report_encode(report,record,sizeof(record));
        assert(bytes==sizeof(report->head)+2*sizeof(struct a733_radio_result_ap)+sizeof(struct a733_radio_scan_counts));
        /* Three sightings of two BSSIDs: listed = sightings - merges. */
        assert(report->scan.sightings==3&&report->scan.merges==1&&report->scan.sightings-report->scan.merges==report->ap_count);
    }
    assert(entropy_calls==entropy_before&&connects==connects_before&&network_calls==network_before&&security_calls==security_before);
    /* Shutdown: refused unless the transport is quiet, then only the platform is polled until DOWN. */
    {
        uint64_t before=app.session.transport.completed_transfers;(void)before;
        app.session.transport.pending=1;app.platform.host.state=SUNXI_SDIO_READY;
        assert(!board_radio_shutdown(&app,5000)&&shutdown_calls==0&&app.stage==SCANNED);
        app.session.transport.pending=0;app.session.transport.held=1;
        assert(!board_radio_shutdown(&app,5000)&&shutdown_calls==0);
        app.session.transport.held=0;shutdown_ok=0;
        assert(!board_radio_shutdown(&app,5000)&&shutdown_calls==1&&app.stage==SCANNED);
        shutdown_ok=1;
        assert(board_radio_shutdown(&app,5000)&&shutdown_calls==2&&app.stage==SHUTTING_DOWN);
        unsigned polls_before=session_calls;
        assert(board_radio_poll(&app)==BOARD_RADIO_SCANNED&&app.stage==SHUTTING_DOWN); /* Still shutting down. */
        assert(board_radio_poll(&app)==BOARD_RADIO_SCANNED&&app.stage==SHUTTING_DOWN);
        assert(board_radio_poll(&app)==BOARD_RADIO_DOWN&&app.stage==DOWN);
        assert(board_radio_poll(&app)==BOARD_RADIO_DOWN&&session_calls==polls_before&&platform_steps>=3);
        assert(!board_radio_shutdown(&app,5000)); /* Once. */
    }
    /* The association stage: scan, pick the credential's SSID with a usable RSN element, wait for the CE entropy, join on the
     * scan channel, start the four-way handshake and stop at SCANNED when the supplicant is authorized. The record names no
     * SSID and no key byte. */
    {
        a733_radio_report_release(&app.report);
        memset(&app,0,sizeof(app));scan_events_left=0;scan_finished=0;report_used=0;scheduler=taskSCHEDULER_NOT_STARTED;
        atomic_store(&recovery_state,1);
        unsigned begins=entropy_begins,secs=security_calls,conns=connects,nets=network_calls;
        struct board_radio_config assoc=scan_only;assoc.associate=1;assoc.connect_timeout_us=500;assoc.security_timeout_us=700;
        assoc.session.wifi.channel_count=2;assoc.session.wifi.channels[0].frequency=2412;assoc.session.wifi.channels[1].frequency=2437;
        assoc.security=(struct aic8800_supplicant_config){.entropy=os_ce_entropy_bytes,.entropy_record=9,.beacon_record=9,
            .ticks_per_second=1000000,.command_timeout=1000};
        assoc.enumeration_only=1;assert(!board_radio_init(&app,&assoc)&&!app.self);assoc.enumeration_only=0;
        assoc.connect_timeout_us=0;assert(!board_radio_init(&app,&assoc)&&!app.self);assoc.connect_timeout_us=500;
        assert(board_radio_init(&app,&assoc));scheduler=taskSCHEDULER_RUNNING;time_us=100;app.session.dispatcher.initialized=1;
        entropy_is_ready=0;entropy_is_failed=0;
        assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==SCANNING&&entropy_begins==begins+1);
        scan_events_left=3;
        assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.assoc.candidates==2&&app.cand_found&&app.cand_frequency==2437);
        /* The candidate's RSN element came from the event's payload: the dispatcher's event carries no frame pointer. */
        assert(last_event&&!last_event->metadata.scan.frame&&app.cand_rsn_bytes==sizeof(rsn_psk_ccmp)&&
            !memcmp(app.cand_rsn,rsn_psk_ccmp,sizeof(rsn_psk_ccmp)));
        scan_finished=1;
        assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==STARTING&&app.assoc.stage==1);
        assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==STARTING&&connects==conns&&entropy_begins==begins+1);
        entropy_is_ready=1;
        assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==CONNECTING&&connects==conns+1&&app.assoc.stage==3);
        assert(assoc_target.channel_index==1&&assoc_target.ssid_bytes==sizeof(cred_ssid)&&
            !memcmp(assoc_target.ssid,cred_ssid,sizeof(cred_ssid))&&
            !memcmp(assoc_target.bssid,"\x02\x00\x00\x00\x00\x01",6)&&assoc_target.security==AIC8800_WIFI_WPA2_PSK_CCMP&&
            assoc_target.connection_authority&&assoc_target.beacon_evidence==assoc_target.connection_authority);
        app.session.wifi.link=AIC8800_WIFI_SECURITY_PENDING;
        assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==SECURING&&security_calls==secs+1&&app.assoc.stage==5);
        assert(!memcmp(assoc_security.pmk,cred_pmk,32));assert(assoc_security.ap_rsn_bytes==sizeof(rsn_psk_ccmp));assert(assoc_security.entropy==os_ce_entropy_bytes);
        uint8_t zero[32]={0};assert(!memcmp(app.config.security.pmk,zero,32)); /* the board's copy is gone once the supplicant has it */
        app.session.supplicant.eapol_rx=2;app.session.supplicant.eapol_tx=2;
        app.session.wifi.link=AIC8800_WIFI_SECURED;app.session.supplicant.state=AIC8800_SUPPLICANT_AUTHORIZED;
        assert(board_radio_poll(&app)==BOARD_RADIO_SCANNED&&app.stage==SCANNED&&network_calls==nets);
        const struct a733_radio_report *report=board_radio_report(&app);
        assert(report->has_assoc&&report->assoc.stage==6&&report->assoc.eapol_rx==2&&report->assoc.eapol_tx==2&&
            report->assoc.link==AIC8800_WIFI_SECURED&&(report->assoc.flags&0x3f)==0x3f&&report->assoc.candidates==2);
        assert(report->ap_count==2&&!report->aps[0].ssid_bytes&&!report->aps[1].ssid_bytes);
        /* The passing candidate's RSN summary: CCMP group and pairwise, PSK, version 1. */
        assert(report->has_rsn&&report->rsn.check==A733_RADIO_RSN_PASSED&&report->rsn.group==4&&report->rsn.pairwise==1u<<4&&
            report->rsn.akm==1u<<2&&report->rsn.version==1&&!report->rsn.capabilities&&!report->rsn.parse_result&&
            report->rsn.flags==(1u|8u|16u));
        uint8_t record[512];size_t bytes=a733_radio_report_encode(report,record,sizeof(record));
        assert(bytes==a733_radio_report_bytes(report)&&bytes==sizeof(report->head)+2*sizeof(struct a733_radio_result_ap)+
            sizeof(struct a733_radio_scan_counts)+sizeof(struct a733_radio_assoc)+sizeof(struct a733_radio_rsn)+
            sizeof(struct aic8800_transport_trace));
        assert(report->has_trace&&!report->has_fault&&(record[37]&32u)&&!memcmp(record+bytes-28,&report->trace,28));
        for(size_t i=0;i+8<=bytes;++i)assert(memcmp(record+i,cred_ssid,8)&&memcmp(record+i,cred_pmk,8));
        a733_radio_report_release(&app.report);
        /* No candidate, and a missing credential, end as a retained failure with the record. */
        memset(&app,0,sizeof(app));scan_events_left=0;scan_finished=0;report_used=0;scheduler=taskSCHEDULER_NOT_STARTED;atomic_store(&recovery_state,1);
        cred_present=0;assert(board_radio_init(&app,&assoc));scheduler=taskSCHEDULER_RUNNING;time_us=100;app.session.dispatcher.initialized=1;
        entropy_is_ready=1;
        assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==SCANNING);
        scan_events_left=3;scan_finished=1;
        (void)board_radio_poll(&app);
        assert(board_radio_poll(&app)==BOARD_RADIO_RETAINED&&!app.cand_found&&connects==conns+1);
        cred_present=1;
        /* Each RSN condition the PSK candidate test refuses, as the record names it. The results carry the credential's
         * SSID, none passes, and the summary comes from the strongest (-41 dBm). */
        {
            static const uint8_t wpa1[]={221,22,0x00,0x50,0xf2,1,1,0,0x00,0x50,0xf2,2,1,0,0x00,0x50,0xf2,2,1,0,0x00,0x50,0xf2,2};
            static const uint8_t mixed[]={48,24,1,0,0x00,0x0f,0xac,2,2,0,0x00,0x0f,0xac,4,0x00,0x0f,0xac,2,1,0,0x00,0x0f,0xac,2,0,0};
            static const uint8_t tkip_pairwise[]={48,20,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,2,1,0,0x00,0x0f,0xac,2,0,0};
            static const uint8_t transition_required[]={48,24,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,4,2,0,0x00,0x0f,0xac,2,0x00,0x0f,0xac,8,0xc0,0};
            static const uint8_t sae_only[]={48,20,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,8,0x80,0};
            static const uint8_t sae_required[]={48,20,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,4,1,0,0x00,0x0f,0xac,8,0xc0,0};
            static const uint8_t short_count[]={48,12,1,0,0x00,0x0f,0xac,4,2,0,0x00,0x0f,0xac,4};
            const struct {const uint8_t *rsn;size_t bytes;const uint8_t *vendor;int parse;unsigned check,group,pairwise,akm,caps;} cases[]={
                {mixed,sizeof(mixed),wpa1,-1,A733_RADIO_RSN_GROUP_NOT_CCMP,2,1u<<4|1u<<2,1u<<2,0},  /* WPA/WPA2 mixed: group TKIP */
                {tkip_pairwise,sizeof(tkip_pairwise),NULL,0,A733_RADIO_RSN_PAIRWISE_NO_CCMP,4,1u<<2,1u<<2,0},
                {transition_required,sizeof(transition_required),NULL,0,A733_RADIO_RSN_MFP_REQUIRED,4,1u<<4,1u<<2|1u<<8,0xc0},
                {sae_required,sizeof(sae_required),NULL,0,A733_RADIO_RSN_MFP_REQUIRED,4,1u<<4,1u<<8,0xc0},
                {sae_only,sizeof(sae_only),NULL,0,A733_RADIO_RSN_NO_PSK,4,1u<<4,1u<<8,0x80},
                {short_count,sizeof(short_count),NULL,-4,A733_RADIO_RSN_MALFORMED,4,0,0,0},
                {rsn_psk_ccmp,sizeof(rsn_psk_ccmp),NULL,-10,A733_RADIO_RSN_OTHER,4,1u<<4,1u<<2,0}, /* the parser refuses */
                {NULL,0,NULL,0,A733_RADIO_RSN_NO_ELEMENT,A733_RADIO_RSN_SUITE_ABSENT,0,0,0},
            };
            for(unsigned k=0;k<sizeof(cases)/sizeof(cases[0]);++k){
                a733_radio_report_release(&app.report);
                memset(&app,0,sizeof(app));scan_events_left=0;scan_finished=0;report_used=0;scheduler=taskSCHEDULER_NOT_STARTED;
                atomic_store(&recovery_state,1);
                test_rsn=cases[k].rsn;test_rsn_bytes=cases[k].bytes;test_vendor=cases[k].vendor;
                test_vendor_bytes=cases[k].vendor?sizeof(wpa1):0;test_parse_result=cases[k].parse;
                assert(board_radio_init(&app,&assoc));scheduler=taskSCHEDULER_RUNNING;time_us=100;app.session.dispatcher.initialized=1;
                entropy_is_ready=1;
                assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==SCANNING);
                scan_events_left=3;scan_finished=1;
                (void)board_radio_poll(&app);
                assert(board_radio_poll(&app)==BOARD_RADIO_RETAINED&&!app.cand_found&&app.assoc.candidates==2&&!(app.assoc.flags&4u));
                const struct a733_radio_report *refused=board_radio_report(&app);
                const struct a733_radio_rsn *s=&refused->rsn;
                if(!(refused->has_rsn&&s->check==cases[k].check&&s->group==cases[k].group&&s->pairwise==cases[k].pairwise&&
                    s->akm==cases[k].akm&&s->capabilities==cases[k].caps&&s->parse_result==(cases[k].rsn?cases[k].parse:0)&&
                    app.rsn_rssi==-41)){
                    fprintf(stderr,"RSN case %u: check %u group %u pairwise %x akm %x caps %x parse %d\n",
                        k,s->check,s->group,s->pairwise,s->akm,s->capabilities,s->parse_result);
                    assert(!"RSN summary");
                }
                uint8_t encoded[1024];uint32_t flags;
                assert(a733_radio_report_encode(refused,encoded,sizeof(encoded))==a733_radio_report_bytes(refused));
                memcpy(&flags,encoded+36,4);assert(flags&2048u); /* the summary follows the association tail */
            }
            test_rsn=rsn_psk_ccmp;test_rsn_bytes=sizeof(rsn_psk_ccmp);test_vendor=NULL;test_vendor_bytes=0;test_parse_result=0;
        }
        /* A CE failure ends the stage as a retained failure, flagged in the record. */
        a733_radio_report_release(&app.report);
        memset(&app,0,sizeof(app));scan_events_left=0;scan_finished=0;report_used=0;scheduler=taskSCHEDULER_NOT_STARTED;atomic_store(&recovery_state,1);
        assert(board_radio_init(&app,&assoc));scheduler=taskSCHEDULER_RUNNING;time_us=100;app.session.dispatcher.initialized=1;
        entropy_is_failed=1;
        assert(board_radio_poll(&app)==BOARD_RADIO_RETAINED&&(app.assoc.flags&64u));
        entropy_is_failed=0;entropy_is_ready=0;

        /* Credential residue on every exit path. Each run holds the test SSID and PMK in the places the real session keeps
         * them (target, connect command, held records, supplicant slots and configuration, key store, owner copies), ends
         * a different way, and then the whole board_radio object and the report arena must hold no 8-byte run of either. */
        enum {OK,HANDSHAKE_TIMEOUT,STARTUP_DEADLINE,BOARD_FAILED,SESSION_RETAINED,CONNECT_TIMEOUT,STOP_OK,STOP_FAULT,PATHS};
        for(unsigned path=0;path<PATHS;++path){
            a733_radio_report_release(&app.report);
            memset(&app,0,sizeof(app));report_used=0;scheduler=taskSCHEDULER_NOT_STARTED;atomic_store(&recovery_state,1);
            platform_steps=0;entropy_is_ready=0;entropy_is_failed=0;scan_events_left=0;scan_finished=0;
            assert(board_radio_init(&app,&assoc));scheduler=taskSCHEDULER_RUNNING;time_us=100;app.session.dispatcher.initialized=1;
            assert(board_radio_poll(&app)==BOARD_RADIO_PENDING);scan_events_left=3;
            (void)board_radio_poll(&app);scan_finished=1;(void)board_radio_poll(&app);entropy_is_ready=1;
            (void)board_radio_poll(&app);assert(app.stage==CONNECTING&&app.cand_found);
            app.session.wifi.link=AIC8800_WIFI_SECURITY_PENDING;
            if(path!=CONNECT_TIMEOUT){(void)board_radio_poll(&app);assert(app.stage==SECURING);}
            /* The two timeouts run the failure shutdown, which reads back every step on its first poll. */
            abort_model=path==HANDSHAKE_TIMEOUT||path==CONNECT_TIMEOUT;abort_steps_left=abort_model;
            memcpy(app.diagnostic.payload,cred_ssid,sizeof(cred_ssid)); /* an owner copy of a record that carried the SSID */
            enum board_radio_result end;
            switch(path){
            case OK:
                app.session.wifi.link=AIC8800_WIFI_SECURED;app.session.supplicant.state=AIC8800_SUPPLICANT_AUTHORIZED;
                app.session.transport.trace_slow_id=36;app.session.transport.trace_slow_ms=5900;
                assert(board_radio_poll(&app)==BOARD_RADIO_SCANNED&&app.stage==SCANNED);
                if(assoc.associate){
                    /* Authorized: the trace alone names the slowest confirmation. */
                    assert(app.report.has_trace&&!app.report.has_fault&&app.report.trace.slow_id==36&&app.report.trace.slow_ms==5900);
                    uint8_t encoded[1024];size_t n=a733_radio_report_encode(&app.report,encoded,sizeof(encoded));
                    assert(n&&!(encoded[37]&16u)&&(encoded[37]&32u)&&!memcmp(encoded+n-28,&app.report.trace,28));
                }
                app.session.transport.pending=0;app.session.transport.held=0;app.platform.host.state=SUNXI_SDIO_READY;shutdown_ok=1;
                assert(board_radio_shutdown(&app,5000));
                do end=board_radio_poll(&app);while(end==BOARD_RADIO_SCANNED);
                assert(end==BOARD_RADIO_DOWN&&app.stage==DOWN&&app.wiped);
                assert(board_radio_poll(&app)==BOARD_RADIO_DOWN); /* polled again after the wipe */
                break;
            case HANDSHAKE_TIMEOUT:{
                /* A key add awaiting its confirmation when the handshake bound passes: the record names the site. */
                app.session.security.state=AIC8800_SECURITY_WAITING;app.session.security.operation=AIC8800_SECURITY_ADD;
                app.session.security.expected_cfm=37;app.session.supplicant.initialized=1;app.session.supplicant.count=1;
                app.session.supplicant.actions[0].kind=AIC8800_SUPPLICANT_KEY;app.session.supplicant.action_active=1;
                app.session.supplicant.completed=1;
                app.session.transport.trace_tx_id=36;app.session.transport.trace_tx_flags=3;app.session.transport.trace_credit=4;
                app.session.transport.trace_status_reads=1;app.session.transport.irq_wait=1;irq_views=0;
                time_us=app.operation_deadline+1;end=board_radio_poll(&app);
                const struct a733_radio_report *failed=&app.report;const struct aic8800_fault_site *f=&failed->fault;
                assert(failed->has_fault&&f->security==AIC8800_SECURITY_WAITING&&f->expected_cfm==37&&!f->cfm_bytes&&
                    (f->supplicant>>4)==1&&f->actions==(1u|2u|8u)&&f->ms==(uint16_t)((app.operation_deadline+1)/1000u));
                uint8_t encoded[1024];size_t n=a733_radio_report_encode(failed,encoded,sizeof(encoded));
                const struct aic8800_transport_trace *tr=&failed->trace;
                assert(failed->has_trace&&irq_views==1&&tr->tx_id==36&&(tr->tx_flags&(1u|2u|8u))==(1u|2u|8u)&&tr->credit==4&&
                    tr->status_reads==1&&!tr->rx_records&&(tr->irq&(2u|4u|8u))==(2u|4u|8u)&&tr->held_type==255u);
                assert(n==a733_radio_report_bytes(failed)&&(encoded[37]&16u)&&(encoded[37]&32u)&&
                    !memcmp(encoded+n-28,tr,28)&&!memcmp(encoded+n-44,f,16));
                break;}
            case STARTUP_DEADLINE:time_us=assoc.startup_deadline_us;end=board_radio_poll(&app);break;
            case BOARD_FAILED:end=board_radio_external_failure(&app);break;
            case SESSION_RETAINED:{
                enum a733_radio_session_result saved_result=session_result;session_result=A733_SESSION_RETAINED;
                end=board_radio_poll(&app);session_result=saved_result;break;}
            case STOP_OK:case STOP_FAULT:{
                unsigned polls=session_calls,steps=platform_steps,retains=retained;
                assert(board_radio_stop_result(&app,A733_SESSION_PENDING)==BOARD_RADIO_INVALID&&!app.wiped);
                assert(board_radio_stop_result(&app,A733_SESSION_DOWN)==BOARD_RADIO_INVALID&&!app.wiped);
                app.session.stop_requested=1;
                if(path==STOP_OK)app.platform.stage=RSTAGE_DOWN;
                else{app.session.fault=1;app.session.fail_line=501;app.session.fail_stage=3;
                    app.session.transport.pending=1;app.session.transport.held=1;app.platform.host.state=SUNXI_SDIO_ACTIVE;}
                struct a733_radio_session preserved=app.session;
                enum a733_radio_session_result stopped=path==STOP_OK?A733_SESSION_DOWN:A733_SESSION_RETAINED;
                end=board_radio_stop_result(&app,stopped);
                assert(end==(path==STOP_OK?BOARD_RADIO_DOWN:BOARD_RADIO_RETAINED));
                assert(board_radio_stop_result(&app,stopped)==end);
                assert(session_calls==polls&&platform_steps==steps&&retained==retains);
                if(path==STOP_FAULT){
                    assert(!app.wiped&&!memcmp(&preserved,&app.session,sizeof(preserved))&&app.platform.host.state==SUNXI_SDIO_ACTIVE);
                    const uint8_t *owner=(const uint8_t *)&app.config.target;
                    for(size_t i=0;i<sizeof(app.config.target);++i)assert(!owner[i]);
                    owner=(const uint8_t *)&app.config.security;
                    for(size_t i=0;i<sizeof(app.config.security);++i)assert(!owner[i]);
                }
                break;}
            default:time_us=app.operation_deadline+1;end=board_radio_poll(&app);break; /* CONNECT_TIMEOUT */
            }
            if(path!=OK&&path!=STOP_OK){assert(end==BOARD_RADIO_RETAINED);if(path!=STOP_FAULT)assert(board_radio_poll(&app)==BOARD_RADIO_RETAINED);}
            if(path==STOP_FAULT)continue; /* Accepted transfer storage remains owned and unchanged. */
            assert(app.wiped);
            if(path==HANDSHAKE_TIMEOUT||path==CONNECT_TIMEOUT){
                /* An association miss with a failure shutdown that reads back every step: the record says the failure
                 * left nothing powered, the board's failure bit stays clear, and the platform is clean, which is what
                 * os_radio_failure_closed needs to close the station and complete the return with the record. */
                assert(abort_model&&app.platform.abort_done&&a733_radio_platform_clean(&app.platform));
                assert(!(atomic_load(&recovery_state)&4));
                const struct a733_radio_report *report=board_radio_report(&app);
                assert(report->head.failed&&(report->head.flags&256u)&&report->assoc.stage);
            }
            /* The report survives the wipe: the failure site and the tail were written before it. */
            assert(board_radio_report(&app)->has_assoc);
            const struct { const uint8_t *memory; size_t bytes; } places[]={{(const uint8_t *)&app,sizeof(app)},{report_arena,sizeof(report_arena)}};
            for(unsigned p=0;p<2;++p){
                const uint8_t *m=places[p].memory;size_t n=places[p].bytes;
                for(size_t i=0;i+8<=n;++i){
                    for(size_t k=0;k+8<=sizeof(cred_ssid);++k)if(!memcmp(m+i,cred_ssid+k,8)){fprintf(stderr,"path %u: SSID residue at %zu in %u\n",path,i,p);assert(!"credential residue");}
                    for(size_t k=0;k+8<=sizeof(cred_pmk);++k)if(!memcmp(m+i,cred_pmk+k,8)){fprintf(stderr,"path %u: PMK residue at %zu in %u\n",path,i,p);assert(!"credential residue");}
                }
            }
        }
        /* The address stage: after authorization the network starts with DHCP once the transport admits it and the IP task
         * has pre-drawn words, the client submits one resolve, and the owner takes a held lease offline through the fence
         * and stops at SCANNED with the address section last in the record. */
        {
            struct board_radio_config addr=assoc;addr.address=1;addr.address_timeout_us=2000;addr.resolve_name="example.com";
            addr.resolve_bytes=11;addr.resolve_timeout=40;addr.network.clients[0]=task;addr.network.bootstrap=task;
            addr.network.network=(struct aic8800_network_config){.dhcp=1,.entropy_record=9,.tx_timeout=5,.queue_timeout=5,
                .address_timeout=5,.lease_cap=10};
            /* Refused: without association, without DHCP, without a name, with a name longer than the network takes. */
            struct board_radio_config bad=addr;bad.associate=0;
            a733_radio_report_release(&app.report);memset(&app,0,sizeof(app));report_used=0;scheduler=taskSCHEDULER_NOT_STARTED;
            assert(!board_radio_init(&app,&bad));memset(&app,0,sizeof(app));
            bad=addr;bad.network.network.dhcp=0;assert(!board_radio_init(&app,&bad));memset(&app,0,sizeof(app));
            bad=addr;bad.resolve_bytes=0;assert(!board_radio_init(&app,&bad));memset(&app,0,sizeof(app));
            bad=addr;bad.resolve_bytes=SUNSTONE_NET_NAME_BYTES;assert(!board_radio_init(&app,&bad));memset(&app,0,sizeof(app));
            bad=addr;bad.address_timeout_us=0;assert(!board_radio_init(&app,&bad));memset(&app,0,sizeof(app));
            bad=addr;bad.network.bootstrap=NULL;assert(!board_radio_init(&app,&bad));memset(&app,0,sizeof(app));
            enum {FENCED,NO_LEASE,DEADLINE,CASES};
            for(unsigned kind=0;kind<CASES;++kind){
                a733_radio_report_release(&app.report);
                memset(&app,0,sizeof(app));report_used=0;scheduler=taskSCHEDULER_NOT_STARTED;atomic_store(&recovery_state,1);
                platform_steps=0;entropy_is_ready=0;entropy_is_failed=0;scan_events_left=0;scan_finished=0;abort_model=0;
                ce_bytes=0;ce_draws=0;network_admit=0;address_reply=-1;net_has_address=1;net_offline_ok=1;park_ok=1;
                unsigned nets=network_calls,subs=submitted,offline_calls=net_offline_calls,events=net_events;
                assert(board_radio_init(&app,&addr));scheduler=taskSCHEDULER_RUNNING;time_us=100;app.session.dispatcher.initialized=1;
                assert(board_radio_poll(&app)==BOARD_RADIO_PENDING);scan_events_left=3;
                (void)board_radio_poll(&app);scan_finished=1;(void)board_radio_poll(&app);entropy_is_ready=1;
                (void)board_radio_poll(&app);assert(app.stage==CONNECTING&&app.cand_found);
                app.session.wifi.link=AIC8800_WIFI_SECURITY_PENDING;(void)board_radio_poll(&app);assert(app.stage==SECURING);
                app.session.wifi.link=AIC8800_WIFI_SECURED;app.session.supplicant.state=AIC8800_SUPPLICANT_AUTHORIZED;
                /* Authorized, but the reservoir is empty: no network yet, and the stage's bound has started. */
                time_us=500;
                assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==SECURING&&app.assoc.stage==6&&
                    app.address_started&&app.operation_deadline==2500&&network_calls==nets&&app.report.has_trace);
                /* Words, but the transport is not yet idle: the next pass tries again. */
                ce_bytes=64;
                assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==SECURING&&network_calls==nets&&
                    atomic_load(&app.net_random_put)==16&&!ce_bytes);
                network_admit=1;
                assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==ONLINE&&network_calls==nets+1&&
                    atomic_load(&app.network_started)&&app.operation_deadline==2500);
                /* The IP task takes words in order; the owner tops the ring up from the reservoir. */
                uint32_t word;struct aic8800_network_config *nc=&app.config.network.network;
                assert(nc->random32(nc->entropy_context,&word)==1&&word==(0xa0u|0xa1u<<8|0xa2u<<16|0xa3u<<24));
                assert(nc->sequence(nc->entropy_context,1,2,3,4,&word)==1&&atomic_load(&app.net_random_take)==2);
                for(unsigned i=2;i<16;++i)assert(nc->random32(nc->entropy_context,&word)==1);
                assert(!nc->random32(nc->entropy_context,&word)&&atomic_load(&app.net_random_short));
                ce_bytes=8;session_result=A733_SESSION_NETWORK;
                /* The client submits one resolve for the stage's name. */
                assert(board_radio_client_poll(&app)==0&&submitted==subs+1&&copied_request.operation==SUNSTONE_NET_RESOLVE&&
                    app.client_pending);
                assert(board_radio_client_poll(&app)==0&&submitted==subs+1); /* no reply yet */
                /* The DHCP and DNS observation the network made, as the record takes it. */
                struct aic8800_address_record *seen=&app.session.sockets.network.address_trace.record;
                *seen=(struct aic8800_address_record){.flags=AIC8800_ADDRESS_PRIVATE|AIC8800_ADDRESS_OFFER_MATCH,.discovers=1,
                    .offers=1,.requests=1,.acks=1,.queries=1,.responses=1,.ms_discover=250,.ms_offer=260,.ms_request=261,
                    .ms_ack=270,.ms_query=300,.ms_response=320,.lease_seconds=86400,.answers=1,.dns_servers=2,.owner_cores=1};
                /* The IP task's counts, which the record copies at the stage's end; empty wakes saturate. */
                struct aic8800_network *ip=&app.session.sockets.network;
                atomic_store(&ip->ip_wakes,70000);atomic_store(&ip->ip_empty_wakes,65536);atomic_store(&ip->ip_cores,4);
                if(kind==DEADLINE){
                    time_us=2499;assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==ONLINE);
                    time_us=2500;assert(board_radio_poll(&app)==BOARD_RADIO_RETAINED);
                    assert(app.report.has_address&&(app.report.address.flags&AIC8800_ADDRESS_DEADLINE)&&!app.report.address.result&&
                        !(app.report.address.flags&AIC8800_ADDRESS_PUBLISHED)&&net_offline_calls==offline_calls);
                    session_result=A733_SESSION_MANAGEMENT;continue;
                }
                assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==ONLINE&&atomic_load(&app.net_random_put)==18);
                address_reply=kind==FENCED?SUNSTONE_NET_OK:SUNSTONE_NET_ADDRESS_LOST;
                assert(board_radio_client_poll(&app)==0&&!app.client_pending&&net_events==events+1&&
                    atomic_load(&app.address_result)==(unsigned)address_reply+1u&&atomic_load(&app.address_resolved)==(kind==FENCED));
                assert(board_radio_client_poll(&app)==0&&submitted==subs+1); /* one resolve only */
                if(kind==NO_LEASE){
                    net_has_address=0;
                    assert(board_radio_poll(&app)==BOARD_RADIO_RETAINED&&net_offline_calls==offline_calls);
                    assert(app.report.has_address&&app.report.address.result==SUNSTONE_NET_ADDRESS_LOST+1u&&
                        !(app.report.address.flags&(AIC8800_ADDRESS_PUBLISHED|AIC8800_ADDRESS_FENCED|AIC8800_ADDRESS_RESOLVED)));
                    session_result=A733_SESSION_MANAGEMENT;continue;
                }
                /* The fence: requested once, then awaited until the IP task has acknowledged it. */
                park_ok=0;
                assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==ONLINE&&net_offline_calls==offline_calls+1);
                park_ok=1;session_result=A733_SESSION_MANAGEMENT;
                assert(board_radio_poll(&app)==BOARD_RADIO_SCANNED&&app.stage==SCANNED&&net_offline_calls==offline_calls+1);
                assert(board_radio_poll(&app)==BOARD_RADIO_SCANNED);
                const struct a733_radio_report *done=board_radio_report(&app);const struct aic8800_address_record *a=&done->address;
                assert(done->has_address&&a->result==SUNSTONE_NET_OK+1u&&a->discovers==1&&a->acks==1&&a->ms_ack==270&&
                    a->lease_seconds==86400&&a->dns_servers==2&&a->answers==1&&a->ip_wakes==70000&&a->ip_empty_wakes==65535&&
                    a->ip_cores==4&&a->owner_cores==1&&
                    a->flags==(AIC8800_ADDRESS_PRIVATE|AIC8800_ADDRESS_OFFER_MATCH|AIC8800_ADDRESS_PUBLISHED|AIC8800_ADDRESS_FENCED|
                        AIC8800_ADDRESS_RESOLVED|AIC8800_ADDRESS_ENTROPY_SHORT));
                uint8_t encoded[1024];size_t n=a733_radio_report_encode(done,encoded,sizeof(encoded));
                assert(n==a733_radio_report_bytes(done)&&(encoded[37]&64u)&&(encoded[37]&32u)&&!memcmp(encoded+n-64,a,64)&&
                    !memcmp(encoded+n-92,&done->trace,28));
                /* The answer's address is in neither the record nor the board object. */
                for(size_t i=0;i+4<=n;++i)assert(memcmp(encoded+i,answer,4));
                for(size_t i=0;i+4<=sizeof(app);++i)assert(memcmp((const uint8_t *)&app+i,answer,4));
            }
            session_result=A733_SESSION_MANAGEMENT;ce_bytes=0;address_reply=-1;
        }
        scheduler=taskSCHEDULER_NOT_STARTED;
    }
    /* A scan that does not finish in time retains the radio with no entropy involved. */
    a733_radio_report_release(&app.report);
    memset(&app,0,sizeof(app));scan_events_left=0;scan_finished=0;report_used=0;scheduler=taskSCHEDULER_NOT_STARTED;atomic_store(&recovery_state,1);
    assert(board_radio_init(&app,&scan_only));scheduler=taskSCHEDULER_RUNNING;time_us=100;app.session.dispatcher.initialized=1;
    assert(board_radio_poll(&app)==BOARD_RADIO_PENDING&&app.stage==SCANNING);
    unsigned retains_before=entropy_retains;time_us=1100;
    assert(board_radio_poll(&app)==BOARD_RADIO_RETAINED&&entropy_retains==retains_before&&board_radio_report(&app)->head.failed==0);
    a733_radio_report_release(&app.report);
    /* The smallest probe: enumeration only. No widening request, no transport or firmware, and the owner
     * reports the terminal result as soon as the session says ENUMERATED. */
    {
        memset(&app,0,sizeof(app));scheduler=taskSCHEDULER_NOT_STARTED;atomic_store(&recovery_state,1);report_used=0;
        unsigned widen_before=fast_requests,saved=session_result,scans=scan_calls;
        struct board_radio_config probe=scan_only;probe.enumeration_only=1;probe.session.enumeration_only=1;
        assert(board_radio_init(&app,&probe)&&fast_requests==widen_before&&!app.platform.four_bit);
        scheduler=taskSCHEDULER_RUNNING;time_us=100;app.session.dispatcher.initialized=1;
        session_result=A733_SESSION_ENUMERATED;
        assert(board_radio_poll(&app)==BOARD_RADIO_SCANNED&&app.stage==SCANNED&&scan_calls==scans); /* No scan was started. */
        assert(board_radio_poll(&app)==BOARD_RADIO_SCANNED);
        session_result=saved;a733_radio_report_release(&app.report);
    }
    /* The firmware stage widens: enumeration_only with four_bit asks for four bits at 25 MHz exactly once. */
    {
        memset(&app,0,sizeof(app));scheduler=taskSCHEDULER_NOT_STARTED;atomic_store(&recovery_state,1);report_used=0;
        unsigned widen_before=fast_requests;
        struct board_radio_config probe=scan_only;probe.enumeration_only=1;probe.four_bit=1;probe.session.enumeration_only=1;
        assert(board_radio_init(&app,&probe)&&fast_requests==widen_before+1&&app.platform.four_bit);
        /* The stored stage numbers do not order the stages: identified (6) comes before firmware (5) in time. */
        a733_radio_report_identity(&app.report,1,2);
        assert(app.report.head.stage==A733_RADIO_STAGE_IDENTIFIED);
        a733_radio_report_firmware(&app.report);
        assert(app.report.head.stage==A733_RADIO_STAGE_FIRMWARE);
        a733_radio_report_identity(&app.report,1,2);
        assert(app.report.head.stage==A733_RADIO_STAGE_FIRMWARE); /* Not lowered by a repeat. */
        a733_radio_report_release(&app.report);
    }
    /* The CE entropy stage: the radio platform and session are never polled, so nothing is powered. */
    {
        memset(&app,0,sizeof(app));scheduler=taskSCHEDULER_NOT_STARTED;atomic_store(&recovery_state,1);report_used=0;
        struct board_radio_config probe=scan_only;probe.enumeration_only=1;probe.entropy_only=1;probe.session.enumeration_only=1;
        assert(board_radio_init(&app,&probe));scheduler=taskSCHEDULER_RUNNING;time_us=100;
        unsigned calls=session_calls,steps=platform_steps,fast=fast_requests;
        assert(board_radio_poll(&app)==BOARD_RADIO_SCANNED&&app.stage==SCANNED);
        assert(board_radio_poll(&app)==BOARD_RADIO_SCANNED&&session_calls==calls&&platform_steps==steps&&fast_requests==fast);
        a733_radio_report_release(&app.report);
    }
    /* A failure with the failure shutdown running: each retain drives it, the failure is final (the board's failure
     * bit set, the session's site in the record) only when it is done, and a shutdown that never finishes is
     * given up on at the board's bound. A guard refusal is final at once and leaves the failure bit alone. */
    for(unsigned model=0;model<3;++model) {
        memset(&app,0,sizeof(app));scheduler=taskSCHEDULER_NOT_STARTED;atomic_store(&recovery_state,1);report_used=0;
        struct board_radio_config probe=scan_only;probe.enumeration_only=1;probe.session.enumeration_only=1;
        assert(board_radio_init(&app,&probe));scheduler=taskSCHEDULER_RUNNING;time_us=100;
        app.session.fail_line=777;app.session.fail_stage=3; /* The session decided: line 777, stage 3. */
        abort_model=model!=2;abort_steps_left=model==0?3:0;  /* 0: finishes in three polls; 1: never finishes. */
        app.platform.refused=model==2;
        if(model==0) {
            assert(board_radio_external_failure(&app)==BOARD_RADIO_PENDING&&!(atomic_load(&recovery_state)&4));
            assert(app.platform.aborting&&!app.platform.abort_done);
            assert(board_radio_external_failure(&app)==BOARD_RADIO_PENDING&&!(atomic_load(&recovery_state)&4));
            assert(board_radio_external_failure(&app)==BOARD_RADIO_RETAINED&&!(atomic_load(&recovery_state)&4)); /* Clean: nothing is powered, so no failure bit. */
            assert(app.platform.abort_done);
        } else if(model==1) {
            assert(board_radio_external_failure(&app)==BOARD_RADIO_PENDING&&!(atomic_load(&recovery_state)&4));
            time_us+=2900000;assert(board_radio_external_failure(&app)==BOARD_RADIO_PENDING);
            time_us+=200000;assert(board_radio_external_failure(&app)==BOARD_RADIO_RETAINED&&(atomic_load(&recovery_state)&4));
            assert(!app.platform.abort_done);
        } else {
            assert(board_radio_external_failure(&app)==BOARD_RADIO_RETAINED&&!(atomic_load(&recovery_state)&4));
        }
        assert(board_radio_report(&app)->head.fail_site==((777u<<16)|0x100u|3u)&&(model==1||board_radio_report(&app)->head.failed));
        a733_radio_report_release(&app.report);
    }
    abort_model=0;
    /* The profile-level entry: no WPA2 target makes a scan-only session with no crypto engine. */
    {
        struct board_radio_profile sp=admitted;sp.version=BOARD_RADIO_PROFILE_SCAN_VERSION;
        for(unsigned i=0;i<4;++i)sp.firmware_address[i]=0x43010000+0x10000*i; /* The overlap test above altered admitted. */
        memset(&sp.target,0,sizeof(sp.target));
        sp.ce_authority=sp.secure_ce_authority=0;memset(sp.ce_evidence,0,sizeof(sp.ce_evidence));
        memset(sp.entropy_evidence,0,sizeof(sp.entropy_evidence));sp.entropy_record=sp.characterization_record=0;
        sp.startup_test_record=sp.health_enable_epoch=0;sp.peer_port=sp.request_bytes=sp.response_bytes=0;
        sp.client_timeout_ticks=0;sp.client_deadline_us=0;sp.association_timeout_us=1000;sp.bootstrap_deadline_us=500;
        uint8_t sd[32];storage_digest_bytes(&sp,sizeof(sp),sd);
        assert(board_radio_profile_scan_only(&sp));assert(board_radio_profile_network_valid(&sp));assert(startup_profile_valid(&sp,sd,&id));assert(!board_radio_profile_scan_only(&admitted));
        sp.association_timeout_us=0;storage_digest_bytes(&sp,sizeof(sp),sd);assert(!startup_profile_valid(&sp,sd,&id));
        sp.association_timeout_us=1000;sp.tcp_mode=1;storage_digest_bytes(&sp,sizeof(sp),sd);assert(!startup_profile_valid(&sp,sd,&id));
        sp.tcp_mode=0;storage_digest_bytes(&sp,sizeof(sp),sd);
        memset(&app,0,sizeof(app));scheduler=taskSCHEDULER_NOT_STARTED;atomic_store(&recovery_state,1);report_used=0;
        struct board_radio_ce_provider ce={0};uint8_t providers[16]={0};unsigned guards=guard_calls,entropy_at=entropy_calls;
        assert(board_radio_startup(&app,&shared,&ce,providers,sizeof(providers),&sp,sd,&id,task,(TaskHandle_t)(uintptr_t)3,&recovery_state));
        assert(app.config.scan_only&&app.config.first_run_guard&&guard_calls==guards+1&&ce.used&&!ce.pool_mutex);
        assert(app.config.scan_timeout_us==1000&&app.startup_profile==&sp&&board_radio_report(&app)&&entropy_calls==entropy_at);
        a733_radio_report_release(&app.report);
    }
    puts("PASS board radio: provider wiring, observed WPA2 checkpoints, copied sockets, repeated exchange timeouts, retained entropy fault and the scan-only staged service");
    return 0;
}
