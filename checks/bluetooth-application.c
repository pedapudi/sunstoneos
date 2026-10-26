/* Exercise only the new application connection. Existing HCI cases are
 * compiled but never called; UART model/wire helpers are reused unchanged. */
#define main bluetooth_archived_hci_main
#include "bt-hci.c"
#undef main
#include "../apps/bluetooth/bluetooth.c"
#include "../apps/bluetooth/result.h"
static struct bluetooth_application app;
static struct bluetooth_profile profile;
static struct a733_native_usb providers;
static struct board_radio_chip_id chip={7,{1,2,3,4}};
static struct fixture wire;
static atomic_uint recovery;
static TaskHandle_t owner=(TaskHandle_t)(uintptr_t)1,current_task;
static BaseType_t scheduler=taskSCHEDULER_NOT_STARTED;
static unsigned radio_polls,radio_retains,uart_retains,bootstrap_ready,uart_busy,constructor_calls;
static unsigned fast_requests,fast_refused,radio_progress,radio_tick,ready_on_poll,clock_fail;
static enum a733_radio_session_result radio_status=A733_SESSION_PENDING;
TaskHandle_t xTaskGetCurrentTaskHandle(void){return current_task;}
BaseType_t xTaskGetSchedulerState(void){return scheduler;}
int a733_counter_clock_now(void *p,uint64_t *t){assert(p==&providers.counter);*t=wire.now;return !clock_fail;}
int a733_radio_platform_native_init(struct a733_radio_platform_native *n,struct a733_platform_locks *l,void *task,
    int (*admit)(void *,const struct a733_radio_platform_config *,const struct a733_clock_token *),void *cookie)
{assert(l==&providers.locks&&task==owner);n->admit=admit;n->admission_cookie=cookie;return 1;}
int a733_uart_platform_native_init(struct a733_uart_platform_native *n,struct a733_platform_locks *l,void *task,unsigned port,
    int (*admit)(void *,const struct a733_uart_platform_config *,const struct a733_clock_token *,enum a733_uart_admission),void *cookie)
{assert(l==&providers.locks&&task==owner&&port==1);n->admit=admit;n->admission_cookie=cookie;return 1;}
int a733_uart_platform_init(struct a733_uart_platform *u,const struct a733_uart_platform_config *c,
    const struct a733_uart_platform_ops *ops,void *cookie,struct a733_clock_leases *clocks,struct a733_pins *pins)
{
    (void)ops;assert(u==&app.uart&&cookie==&app.uart_native&&clocks==&providers.clocks&&pins==&providers.pins);
    u->config=*c;u->clock.id=9;u->clock.owner=c->owner;
    assert(app.uart_native.admit(app.uart_native.admission_cookie,c,&u->clock,A733_UART_ADMIT_START)==1);
    struct a733_uart_ops native={read32,write32,now};
    assert(a733_uart_begin(&u->uart,&native,&wire,&c->format,c->startup_deadline_us)==A733_UART_CONFIGURING);
    ++constructor_calls;return 1;
}
enum a733_uart_platform_result a733_uart_platform_poll(struct a733_uart_platform *u)
{assert(u==&app.uart);return a733_uart_poll(&u->uart)==A733_UART_READY?A733_UART_PLATFORM_READY:A733_UART_PLATFORM_RETAINED;}
enum a733_uart_platform_result a733_uart_platform_live(struct a733_uart_platform *u,uint64_t d)
{assert(u==&app.uart&&d==app.deadline);u->uart.deadline=d;return uart_busy?A733_UART_PLATFORM_BUSY:A733_UART_PLATFORM_READY;}
struct a733_uart *a733_uart_platform_controller(struct a733_uart_platform *u){assert(u==&app.uart);return &u->uart;}
enum a733_uart_platform_result a733_uart_platform_retain(struct a733_uart_platform *u)
{assert(u==&app.uart&&(atomic_load(&recovery)&4));++uart_retains;return A733_UART_PLATFORM_RETAINED;}
int a733_radio_platform_init(struct a733_radio_platform *p,const struct a733_radio_platform_config *c,
    const struct a733_radio_platform_ops *ops,void *cookie,struct a733_clock_leases *clocks,
    struct a733_pins *pins,struct a733_rail_leases *rails,struct a733_pmic *pmic)
{(void)ops;assert(p==&app.radio&&cookie==&app.radio_native&&clocks==&providers.clocks&&
 pins==&providers.pins&&rails==&providers.rails&&pmic==&providers.pmic);p->config=*c;return 1;}
int a733_radio_platform_four_bit(struct a733_radio_platform *p)
{assert(p==&app.radio&&!p->four_bit);++fast_requests;if(fast_refused)return 0;p->four_bit=1;return 1;}
int a733_radio_platform_bluetooth(struct a733_radio_platform *p,const struct a733_radio_bluetooth_pins *b)
{assert(p==&app.radio&&b==&profile.wake);p->bluetooth=*b;return 1;}
enum a733_radio_platform_result a733_radio_platform_bluetooth_wake(struct a733_radio_platform *p,
    const struct a733_radio_borrow *b,uint64_t d,unsigned *v)
{assert(p==&app.radio&&b==&app.session.borrow&&d==app.session.deadline);*v=0;return A733_RADIO_READY;}
int a733_radio_session_init_bootstrap(struct a733_radio_session *s,struct a733_radio_platform *p,const struct a733_radio_session_config *c)
{
    assert(s==&app.session&&p==&app.radio&&c->owner_task==owner&&!c->wifi.rf_profile_record&&p->four_bit);
    s->config=*c;s->deadline=c->bootstrap_deadline;return 1;
}
enum a733_radio_session_result a733_radio_session_poll(struct a733_radio_session *s)
{assert(s==&app.session&&app.uart.uart.state==A733_UART_READY);++radio_polls;wire.now+=radio_tick;
 if(radio_progress&1)++app.radio.host.progress;if(radio_progress&2)++s->transport.progress;
 if(radio_progress&4)++app.radio.stage;if(radio_progress&8)++s->stage;
 if(ready_on_poll==radio_polls)s->dispatcher.initialized=1;return radio_status;}
int a733_radio_session_bootstrap(struct a733_radio_session *s,uint64_t *generation)
{assert(s==&app.session);if(!bootstrap_ready)return 0;s->borrow.generation=4;*generation=4;return 1;}
enum a733_radio_session_result a733_radio_session_retain(struct a733_radio_session *s)
{assert(s==&app.session&&(atomic_load(&recovery)&4));++radio_retains;return A733_SESSION_RETAINED;}
int a733_radio_session_take_event(struct a733_radio_session *s,struct aic8800_dispatch_event *e){(void)s;(void)e;return 0;}
int a733_radio_session_take_record(struct a733_radio_session *s,struct aic8800_dispatch_record *r){(void)s;(void)r;return 0;}
#ifndef BLUETOOTH_APPLICATION_FIXTURE_ENTRY
#define BLUETOOTH_APPLICATION_FIXTURE_ENTRY main
#endif
int BLUETOOTH_APPLICATION_FIXTURE_ENTRY(int argc,char **argv)
{
    wire.now=1;wire.room=1024;providers.used=1;providers.boot.identity=99;atomic_init(&recovery,1);
    profile=(struct bluetooth_profile){.magic=BLUETOOTH_PROFILE_MAGIC,.version=1,.bytes=624,
        .topology=2,.sid_authority=7,.radio_authority=3,.uart_authority=4,.wake_authority=5,
        .wiring_evidence={1},.sid_evidence={2},.fresh_transport_evidence={3},.pm_latch={6,7,0x3f,0},
        .radio={.owner=20,.topology={.identity=2}},.wake={.authority=5,.desired={{1,0,0},{0,0,0}}},
        .uart={.owner=21,.route=A733_UART1_BLUETOOTH,.format={1,8,2,0,1,24000000,1500000,0}},
        .firmware_address={0x43010000,0x43020000,0x43030000,0x43040000},.firmware_bytes={1708,24744,23288,341260},
        .session_owner=22,.runtime_lease_us=10000000,.renew_margin_us=3000000,
        .command_timeout_us=500000,.frame_timeout_us=100000,.binding_timeout_us=1000000};
    uint8_t digest[32];storage_digest_bytes(&profile,profile.bytes,digest);assert(bluetooth_profile_valid(&profile,digest));
    digest[0]^=1;assert(!bluetooth_profile_valid(&profile,digest));digest[0]^=1;
    assert(bluetooth_application_init(&app,&profile,&providers,&chip,owner,&recovery,0,60000000,digest)&&constructor_calls==1);
    scheduler=taskSCHEDULER_RUNNING;current_task=owner;
    assert(bluetooth_application_poll(&app)==0&&!radio_polls);
    unsigned accesses=wire.accesses;assert(bluetooth_application_poll(&app)==0&&wire.accesses==accesses&&!wire.tn);
    bootstrap_ready=1;uart_busy=1;
    assert(bluetooth_application_poll(&app)==0&&wire.accesses==accesses&&!wire.tn);uart_busy=0;
    const uint16_t commands[]={0x0c03,0x1001,0x1002,0x1003,0x1005,0x1009};
    const unsigned lengths[]={1,9,65,9,8,7};
    for(unsigned index=0;index<6;++index){
        size_t before=wire.tn;
        for(unsigned n=0;app.hci.command!=BT_HCI_WAITING;++n){assert(n<20);assert(bluetooth_application_poll(&app)>=0);}
        assert(wire.tn==before+4&&wire.tx[before]==1&&wire.tx[before+1]==(uint8_t)commands[index]&&
            wire.tx[before+2]==(uint8_t)(commands[index]>>8)&&wire.tx[before+3]==0);
        cc(&wire,commands[index],1,lengths[index],0);
        for(unsigned n=0;app.hci.stage==index;++n){assert(n<20);assert(bluetooth_application_poll(&app)>=0);}
    }
    struct bt_hci_identity identity;assert(bluetooth_application_result(&app,&identity)==1&&identity.valid==63&&
        app.firmware_generation==4&&app.uart_generation==9&&app.host_wake==0);
    uint8_t frame[177];size_t frame_bytes=0;
    assert(bluetooth_result_frame(frame,&frame_bytes,1,31,37,digest,4,9,&identity)&&frame_bytes==177);
    assert(argc==3);FILE *file=fopen(argv[1],"wb");assert(file&&fwrite(frame,1,frame_bytes,file)==frame_bytes&&!fclose(file));
    file=fopen(argv[2],"wb");assert(file&&fwrite(&profile,1,profile.bytes,file)==profile.bytes&&!fclose(file));
    unsigned count=wire.accesses,polls=radio_polls;
    atomic_fetch_or(&recovery,4);assert(bluetooth_application_poll(&app)==-1&&wire.accesses==count&&radio_polls==polls);
    assert(bluetooth_application_result(&app,&identity)==-1&&radio_retains==1&&uart_retains==1);
    assert(bluetooth_application_poll(&app)==-1&&wire.accesses==count&&radio_polls==polls);
    puts("PASS Bluetooth application: real UART/H4/HCI identify only after bootstrap/live guards; retained cutoff performs no device work");
    return 0;
}
