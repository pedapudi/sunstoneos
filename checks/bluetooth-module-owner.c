/* The Bluetooth application as a second client of a module that a full Wi-Fi
 * session owns (bluetooth_application_attach), and the resident lifetime.
 * apps/bluetooth/bluetooth.c runs unchanged with the real UART, H4 and HCI
 * over the bt-hci wire model; the module platform, session and native adapter
 * are modeled at their interfaces. The cases check that the attached client
 * refuses a module its profile does not name, authenticates the wake pins in
 * the added admission, never polls or retains the module, gives up when the
 * module's firmware is not ready by its own bound, identifies the controller
 * on the shared firmware generation, moves the schedule origin to firmware
 * readiness in the resident lifetime, and ends on a module fault while
 * leaving the module and the board's latch alone. */
#define main bluetooth_archived_hci_main
#include "bt-hci.c"
#undef main
#include "../apps/bluetooth/bluetooth.c"
static struct bluetooth_application app;
static struct bluetooth_profile profile;
static struct a733_native_usb providers;
static struct board_radio_chip_id chip={7,{1,2,3,4}};
static struct fixture wire;
static atomic_uint latch,board_latch;
static struct a733_radio_platform_native native;
static struct a733_radio_platform platform;
static struct a733_radio_session module;
static TaskHandle_t owner=(TaskHandle_t)(uintptr_t)1,current_task;
static BaseType_t scheduler=taskSCHEDULER_NOT_STARTED;
static unsigned bootstrap_ready,uart_retains,wake_reads;
TaskHandle_t xTaskGetCurrentTaskHandle(void){return current_task;}
BaseType_t xTaskGetSchedulerState(void){return scheduler;}
int a733_counter_clock_now(void *p,uint64_t *t){assert(p==&providers.counter);*t=wire.now;return 1;}
int a733_radio_platform_native_init(struct a733_radio_platform_native *n,struct a733_platform_locks *l,void *task,
    int (*admit)(void *,const struct a733_radio_platform_config *,const struct a733_clock_token *),void *cookie)
{assert(l==&providers.locks&&task==owner);n->admit=admit;n->admission_cookie=cookie;n->initialized=1;return 1;}
int a733_radio_platform_native_extend(struct a733_radio_platform_native *n,
    int (*admit)(void *,const struct a733_radio_platform_config *,const struct a733_clock_token *),void *cookie)
{assert(n==&native&&scheduler==taskSCHEDULER_NOT_STARTED);n->extension=admit;n->extension_cookie=cookie;return 1;}
int a733_uart_platform_native_init(struct a733_uart_platform_native *n,struct a733_platform_locks *l,void *task,unsigned port,
    int (*admit)(void *,const struct a733_uart_platform_config *,const struct a733_clock_token *,enum a733_uart_admission),void *cookie)
{assert(l==&providers.locks&&task==owner&&port==1);n->admit=admit;n->admission_cookie=cookie;return 1;}
int a733_uart_platform_init(struct a733_uart_platform *u,const struct a733_uart_platform_config *c,
    const struct a733_uart_platform_ops *ops,void *cookie,struct a733_clock_leases *clocks,struct a733_pins *pins)
{
    (void)ops;assert(u==&app.uart&&cookie==&app.uart_native&&clocks==&providers.clocks&&pins==&providers.pins);
    u->config=*c;u->clock.id=9;u->clock.owner=c->owner;
    assert(app.uart_native.admit(app.uart_native.admission_cookie,c,&u->clock,A733_UART_ADMIT_START)==1);
    struct a733_uart_ops ops_={read32,write32,now};
    assert(a733_uart_begin(&u->uart,&ops_,&wire,&c->format,c->startup_deadline_us)==A733_UART_CONFIGURING);
    return 1;
}
enum a733_uart_platform_result a733_uart_platform_poll(struct a733_uart_platform *u)
{assert(u==&app.uart);return a733_uart_poll(&u->uart)==A733_UART_READY?A733_UART_PLATFORM_READY:A733_UART_PLATFORM_RETAINED;}
enum a733_uart_platform_result a733_uart_platform_live(struct a733_uart_platform *u,uint64_t d)
{assert(u==&app.uart&&d==app.deadline);u->uart.deadline=d;return A733_UART_PLATFORM_READY;}
struct a733_uart *a733_uart_platform_controller(struct a733_uart_platform *u){assert(u==&app.uart);return &u->uart;}
enum a733_uart_platform_result a733_uart_platform_retain(struct a733_uart_platform *u)
{assert(u==&app.uart&&(atomic_load(app.recovery_state)&4));++uart_retains;return A733_UART_PLATFORM_RETAINED;}
int a733_radio_platform_init(struct a733_radio_platform *p,const struct a733_radio_platform_config *c,
    const struct a733_radio_platform_ops *ops,void *cookie,struct a733_clock_leases *clocks,
    struct a733_pins *pins,struct a733_rail_leases *rails,struct a733_pmic *pmic)
{(void)ops;(void)clocks;(void)pins;(void)rails;(void)pmic;assert(p==&app.radio&&cookie==&app.radio_native);p->config=*c;return 1;}
int a733_radio_platform_bluetooth(struct a733_radio_platform *p,const struct a733_radio_bluetooth_pins *b)
{assert(p==app.module_radio&&b==&profile.wake&&!p->bluetooth.authority);p->bluetooth=*b;return 1;}
enum a733_radio_platform_result a733_radio_platform_bluetooth_wake(struct a733_radio_platform *p,
    const struct a733_radio_borrow *b,uint64_t d,unsigned *v)
{assert(p==&platform&&b==&module.borrow&&d==module.deadline);++wake_reads;*v=0;return A733_RADIO_READY;}
int a733_radio_session_init_bootstrap(struct a733_radio_session *s,struct a733_radio_platform *p,const struct a733_radio_session_config *c)
{assert(s==&app.session&&p==&app.radio);s->config=*c;s->deadline=c->bootstrap_deadline;return 1;}
/* The module owner polls, drains and retains its own session. */
enum a733_radio_session_result a733_radio_session_poll(struct a733_radio_session *s){assert(s!=&module);return A733_SESSION_PENDING;}
enum a733_radio_session_result a733_radio_session_retain(struct a733_radio_session *s){assert(s!=&module);return A733_SESSION_RETAINED;}
int a733_radio_session_take_event(struct a733_radio_session *s,struct aic8800_dispatch_event *e){assert(s!=&module);(void)e;return 0;}
int a733_radio_session_take_record(struct a733_radio_session *s,struct aic8800_dispatch_record *r){assert(s!=&module);(void)r;return 0;}
int a733_radio_session_bootstrap(struct a733_radio_session *s,uint64_t *generation)
{
    assert(s==app.module_session);if(s->fault)return -1;if(!bootstrap_ready)return 0;
    s->borrow.generation=4;*generation=4;return 1;
}
static uint8_t digest[32];
static struct fixture fresh_wire;
/* A full Wi-Fi session on the owner task, before any poll, and a profile that
 * names its module. */
static void reset(void)
{
    memset(&app,0,sizeof(app));memset(&native,0,sizeof(native));memset(&platform,0,sizeof(platform));
    memset(&module,0,sizeof(module));atomic_init(&latch,0);atomic_init(&board_latch,1);
    native.owner_task=owner;native.initialized=1;
    platform.cookie=&native;platform.config.owner=20;platform.config.topology.identity=2;
    module.self=&module;module.platform=&platform;module.config.owner_task=owner;module.config.owner=22;
    module.config.firmware.identity=profile.identity;module.deadline=90000000;
    for(unsigned i=0;i<4;++i)module.config.firmware.blobs[i]=(struct aic8800_firmware_blob){
        (const uint8_t *)(uintptr_t)profile.firmware_address[i],(size_t)profile.firmware_bytes[i]};
    scheduler=taskSCHEDULER_NOT_STARTED;current_task=NULL;bootstrap_ready=0;
}
static int attach(uint64_t start,uint64_t end)
{return bluetooth_application_attach(&app,&profile,&providers,&chip,owner,&latch,start,end,digest,&native,&module);}
/* Polls until the six identification commands complete. */
static void identify(void)
{
    const uint16_t commands[]={0x0c03,0x1001,0x1002,0x1003,0x1005,0x1009};
    const unsigned lengths[]={1,9,65,9,8,7};
    for(unsigned index=0;index<6;++index){
        size_t before=wire.tn;
        for(unsigned n=0;app.hci.command!=BT_HCI_WAITING;++n){assert(n<20);assert(bluetooth_application_poll(&app)>=0);}
        assert(wire.tn==before+4&&wire.tx[before+1]==(uint8_t)commands[index]);
        cc(&wire,commands[index],1,lengths[index],0);
        for(unsigned n=0;app.hci.stage==index;++n){assert(n<20);assert(bluetooth_application_poll(&app)>=0);}
    }
    struct bt_hci_identity identity;assert(bluetooth_application_result(&app,&identity)==1&&identity.valid==63);
}
int main(void)
{
    wire.now=1;wire.room=1024;providers.used=1;providers.boot.identity=99;
    profile=(struct bluetooth_profile){.magic=BLUETOOTH_PROFILE_MAGIC,.version=1,.bytes=624,
        .topology=2,.sid_authority=7,.radio_authority=3,.uart_authority=4,.wake_authority=5,
        .wiring_evidence={1},.sid_evidence={2},.fresh_transport_evidence={3},.pm_latch={6,7,0x3f,0},
        .radio={.owner=20,.topology={.identity=2}},.wake={.authority=5,.desired={{1,0,0},{0,0,0}}},
        .uart={.owner=21,.route=A733_UART1_BLUETOOTH,.format={1,8,2,0,1,24000000,1500000,0}},
        .identity={.chip_word=0x7,.subrevision_word=0x20},
        .firmware_address={0x43010000,0x43020000,0x43030000,0x43040000},.firmware_bytes={1708,24744,23288,341260},
        .session_owner=22,.runtime_lease_us=10000000,.renew_margin_us=3000000,
        .command_timeout_us=500000,.frame_timeout_us=100000,.binding_timeout_us=1000000};
    storage_digest_bytes(&profile,profile.bytes,digest);fresh_wire=wire;

    /* A module the profile does not name, a bootstrap-only or faulted session,
     * another owner task or adapter, and another lifetime are refused before
     * any provider is built. */
#define REFUSED(change) do{reset();change;assert(!attach(0,60000000)&&!app.self&&!native.extension);}while(0)
    REFUSED(module.bootstrap_only=1);
    REFUSED(module.fault=1);
    REFUSED(module.config.owner_task=(TaskHandle_t)(uintptr_t)2);
    REFUSED(native.owner_task=(TaskHandle_t)(uintptr_t)2);
    REFUSED(platform.cookie=&app);
    REFUSED(module.config.owner=23);
    REFUSED(platform.config.owner=24);
    REFUSED(platform.config.topology.identity=3);
    REFUSED(module.config.firmware.identity.chip_word^=1);
    REFUSED(module.config.firmware.blobs[3].bytes-=4);
    REFUSED(module.config.firmware.blobs[1].data+=4096);
    reset();assert(!attach(0,59000000)&&!native.extension);

    /* Supervised attachment: the wake pins join the module platform and the
     * added admission accepts only them, with the client latch clear. */
    reset();assert(attach(0,60000000)==1&&app.attached&&app.module_session==&module&&app.module_radio==&platform);
    assert(native.extension&&native.extension_cookie==&app&&platform.bluetooth.authority==5&&app.bootstrap_deadline==40000000);
    struct a733_clock_token token={.owner=20};
    assert(native.extension(native.extension_cookie,&platform.config,&token)==1);
    platform.bluetooth.desired[0].pull^=1;assert(!native.extension(native.extension_cookie,&platform.config,&token));
    platform.bluetooth.desired[0].pull^=1;token.owner=21;assert(!native.extension(native.extension_cookie,&platform.config,&token));
    token.owner=20;atomic_store(&latch,4);assert(!native.extension(native.extension_cookie,&platform.config,&token));
    atomic_store(&latch,0);
    /* The module's firmware is not ready by the client's bound: the client
     * retains its own transports; the module and the board latch stay. */
    scheduler=taskSCHEDULER_RUNNING;current_task=owner;
    assert(bluetooth_application_poll(&app)==0&&app.stage==1);
    wire.now=39999999;assert(bluetooth_application_poll(&app)==0);
    wire.now=40000000;assert(bluetooth_application_poll(&app)==-1&&(atomic_load(&latch)&4)&&uart_retains==1&&
        !module.fault&&atomic_load(&board_latch)==1&&!wake_reads);

    /* Resident attachment identifies the controller on the module's firmware
     * generation. Readiness at 50 s moves the origin to 10 s, where the
     * supervised 40 s firmware bound would fall: HCI identification is due at
     * 65 s. Nothing in the client polls or retains the module. */
    reset();wire=fresh_wire;
    assert(attach(0,BLUETOOTH_RESIDENT_END_US)==1&&app.resident&&app.bootstrap_deadline==120000000&&
        app.deadline==BLUETOOTH_RESIDENT_END_US-10000);
    scheduler=taskSCHEDULER_RUNNING;current_task=owner;
    assert(bluetooth_application_poll(&app)==0&&app.stage==1);
    wire.now=50000000;assert(bluetooth_application_poll(&app)==0);
    bootstrap_ready=1;identify();
    assert(app.started==10000000&&app.hci_deadline==65000000&&app.firmware_generation==4&&wake_reads);
    /* A module fault ends the client without touching the module again. */
    module.fault=1;unsigned retains=uart_retains;
    assert(bluetooth_application_poll(&app)==-1&&(atomic_load(&latch)&4)&&uart_retains==retains+1&&
        atomic_load(&board_latch)==1);
    puts("PASS Bluetooth module client: named module only, wake pins in the added admission, own bound and latch, shared firmware, resident origin, module fault ends the client");
    return 0;
}
