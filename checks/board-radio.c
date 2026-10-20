/* Application orchestration only. Lower protocol proofs are reused; their
 * observations below are explicit checkpoints, not RF or entropy evidence. */
#include "../apps/board/radio.c"
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
static unsigned entropy_calls,session_calls,retained,connects,security_calls,network_calls,submitted,received;
static unsigned entropy_retains;
static atomic_uint recovery_state;
static int entropy_failed,boundary;
static int client_model;
static unsigned listener_variant,listener_closes;
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
int a733_counter_clock_now(void *p,uint64_t *n){assert(p==&shared.counter);*n=time_us;return 1;}
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
int a733_radio_session_init(struct a733_radio_session *s,struct a733_radio_platform *p,
    const struct a733_radio_session_config *c)
{
    assert(s==&app.session&&p==&app.platform&&c->owner_task==task);
    uint8_t output[16];assert(c->runtime.read_platform_id(c->runtime.context,output));
    assert(output[0]==0x44&&output[3]==0x11&&output[15]==0xdd);
    s->deadline=9000;return 1;
}
enum a733_radio_session_result a733_radio_session_poll(struct a733_radio_session *s)
{assert(s==&app.session&&entropy_calls>session_calls);++session_calls;return session_result;}
enum a733_radio_session_result a733_radio_session_retain(struct a733_radio_session *s)
{assert(s==&app.session);++retained;s->fault=1;return A733_SESSION_RETAINED;}
int a733_radio_session_connect(struct a733_radio_session *s,const struct aic8800_wifi_target *t,uint64_t deadline)
{assert(s==&app.session&&t==&app.config.target&&deadline==1100);++connects;s->wifi.link=AIC8800_WIFI_CONNECTING;return 1;}
int a733_radio_session_start_security(struct a733_radio_session *s,const struct aic8800_supplicant_config *c,uint64_t d)
{assert(s==&app.session&&c->entropy==a733_radio_entropy_bytes&&c->entropy_context==&app.entropy&&d==2100);
 ++security_calls;return boundary;}
int a733_radio_session_start_network(struct a733_radio_session *s,const struct sunstone_net_config *c)
{assert(s==&app.session&&c->network.transport==&s->transport&&c->network.dispatcher==&s->dispatcher&&
 c->network.random32==a733_radio_entropy_random32&&c->network.sequence==a733_radio_entropy_sequence&&
 c->network.entropy_context==&app.entropy);++network_calls;return 1;}
int aic8800_network_ready(const struct aic8800_network *n){assert(n==&app.session.sockets.network);return 1;}
int a733_radio_session_take_event(struct a733_radio_session *s,struct aic8800_dispatch_event *e)
{assert(s==&app.session&&e==&app.startup_event);return 1;}
int a733_radio_session_take_record(struct a733_radio_session *s,struct aic8800_dispatch_record *d)
{assert(s==&app.session&&d==&app.diagnostic);return 1;}
int sunstone_net_submit(struct sunstone_net *s,const struct sunstone_net_request *r,uint64_t *ticket)
{assert(s==&app.session.sockets&&r->timeout==3);++submitted;*ticket=42+submitted-1;
 copied_request=*r;copied_ticket=*ticket;return 1;}
int sunstone_net_receive(struct sunstone_net *s,struct sunstone_net_reply *r,TickType_t wait)
{
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
 if(client_model&&copied_request.operation==SUNSTONE_NET_SEND)r->stack_result=2;
 if(client_model&&copied_request.operation==SUNSTONE_NET_RECEIVE){
    r->bytes=2;memcpy(r->payload,app.startup_profile->response+app.client_received,2);
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
    puts("PASS board radio: provider wiring, observed WPA2 checkpoints, copied sockets and retained entropy fault");
    return 0;
}
