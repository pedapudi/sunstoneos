#include "application.h"
#ifdef SUNSTONE_BOARD_USB2_IRQ
#include "host_irq.h"
#endif
#include "session.h"
#include "cached.h"
#include "a733_memory_uncached.h"
#include "platform.h"
#include "a733_storage_digest.h"
#include "aarch64_device_memory.h"
#include <string.h>
#if defined(SUNSTONE_BOARD_SUPERVISED) && defined(SUNSTONE_BOARD_USB3)
/* Immutable capability, retained like existing application geometry records. */
__attribute__((used,retain)) const uint64_t board_usb3_hid_capability[4]={UINT64_C(0x4153373353484431),1,8,64};
#endif
struct board_application board;
struct board_provider providers;
static struct board_radio radio;
static struct board_radio_profile admitted_profile;
#ifdef SUNSTONE_BOARD_SUPERVISED
static struct board_radio_profile runtime_profile;
static uint8_t runtime_profile_digest[32];
#endif
#ifndef SUNSTONE_BOARD_SUPERVISED
static uint64_t seen_progress;
#endif
static uint64_t service_deadline(void)
{return atomic_load_explicit(&board.live_until,memory_order_acquire)-10000;}
int board_live(void *unused)
{(void)unused;return !atomic_load_explicit(&board.fatal,memory_order_acquire)&&
    a733_smp_counter()/24<atomic_load_explicit(&board.live_until,memory_order_acquire)&&
    a733_smp_counter()/24<board.session.deadline_us;}
void board_fail(void *unused)
{(void)unused;atomic_fetch_or_explicit(&board.watchdog_state,4,memory_order_release);
 atomic_store_explicit(&board.fatal,1,memory_order_release);}
/* This hook is used by the existing cached EL1/EL3 fatal paths. It only closes
 * atomic admission. The independent watchdog owns recovery from stopped CPUs. */
void a733_storage_maintenance_stop(void)
{board_fail(NULL);a733_typec_revoke(&board.typec.provider);}
static _Noreturn void park(void){for(;;)vTaskDelay(1);}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,2);}
static void pmic(void *unused)
{
    (void)unused;for(;;){
        if(!board_live(NULL))park();
        enum a733_pmic_status r=a733_pmic_poll(&providers.shared.pmic);
        if(r==A733_PMIC_READY||r==A733_PMIC_COMPLETE)
            atomic_store_explicit(&providers.shared.pmic_ready,1,memory_order_release);
        else if(r!=A733_PMIC_PENDING&&r!=A733_PMIC_BUSY){board_fail(NULL);park();}
        vTaskDelay(1);
    }
}
static void typec(void *unused)
{
    (void)unused;int started=0;
    for(;;){
        if(!board_live(NULL)||a733_smp_counter()/24>=service_deadline())
            a733_typec_revoke(&board.typec.provider);
        if(!started&&board_live(NULL)&&atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire)){
            if(!a733_typec_begin(&board.typec.provider,board.session.deadline_us))a733_typec_revoke(&board.typec.provider);
            started=1;
        }
        if(started)(void)a733_typec_poll(&board.typec.provider);
        vTaskDelay(1);
    }
}
static void power(void *unused)
{
    (void)unused;for(;;){
        if(!board_live(NULL)||a733_smp_counter()/24>=service_deadline())
            a733_typec_revoke(&board.typec.provider);
        (void)a733_typec_native_power_poll(&board.typec);
        /* Retained or failed USB work never suspends this task. */
        vTaskDelay(1);
    }
}
static void radio_owner(void *unused)
{
    (void)unused;for(;;){
        if(!board_live(NULL)){
            (void)board_radio_external_failure(&radio);
            atomic_store_explicit(&board.radio_result,3,memory_order_release);
        }else if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire)){
            enum board_radio_result r=board_radio_poll(&radio);
            if(r==BOARD_RADIO_RETAINED||r==BOARD_RADIO_INVALID){
                atomic_store_explicit(&board.radio_result,3,memory_order_release);
                atomic_fetch_or_explicit(&board.watchdog_state,4,memory_order_release);
                a733_typec_revoke(&board.typec.provider);
            }
        }
        vTaskDelay(1);
    }
}
static void radio_client(void *unused)
{
    (void)unused;for(;;){
        if(board_live(NULL)){
            int r=board_radio_client_poll(&radio);
            if(r<0){
                atomic_store_explicit(&board.radio_result,3,memory_order_release);
                atomic_fetch_or_explicit(&board.watchdog_state,4,memory_order_release);
                a733_typec_revoke(&board.typec.provider);
            }else if(r==1){
                unsigned verified=0;
                if(board_radio_client_result(&radio,&verified)!=1){vTaskDelay(1);continue;}
                atomic_store_explicit(&board.radio_bytes,verified,memory_order_relaxed);
                unsigned expected=1;
                (void)atomic_compare_exchange_strong_explicit(&board.radio_result,&expected,2,
                    memory_order_release,memory_order_relaxed);
            }
        }
        vTaskDelay(1);
    }
}
#ifndef SUNSTONE_BOARD_SUPERVISED
static void supervise(void *unused)
{
    (void)unused;for(;;){
        if(board_live(NULL)&&atomic_load_explicit(&board.watchdog_state,memory_order_acquire)==1){
            uint64_t progress,operation;
            int r=board_radio_watchdog(&radio,&progress,&operation);
            if(r<0){
                atomic_store_explicit(&board.radio_result,3,memory_order_release);
                atomic_fetch_or_explicit(&board.watchdog_state,4,memory_order_release);
                a733_typec_revoke(&board.typec.provider);
            }else if(r==1&&progress>seen_progress){
                uint64_t now=a733_smp_counter()/24;
                uint64_t until=atomic_load_explicit(&board.live_until,memory_order_acquire);
                if(operation<until)until=operation;
                uint64_t before,after;
                if(now>=until||!a733_watchdog_hw_reload(&board.window,&providers.shared.counter,
                   &providers.shared.locks,board.handles[7],&board.watchdog_state,until,&before,&after)){
                    atomic_fetch_or_explicit(&board.watchdog_state,4,memory_order_release);
                    a733_typec_revoke(&board.typec.provider);
                }else{
                    uint64_t next=before+board.session.minimum_reload_us-board.session.guard_us-1000;
                    if(next>board.session.deadline_us)next=board.session.deadline_us;
                    atomic_store_explicit(&board.live_until,next,memory_order_release);
                    seen_progress=progress;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
#else
static void supervise(void *unused)
{
    (void)unused;for(;;){
        /* Software observation only. A stopped CPU/interconnect can prevent
         * this task; confirmed whole-board power removal owns recovery. */
        if(!board_live(NULL)){
            board_fail(NULL);
            if(a733_board_host_workload(board.session.workload))a733_typec_revoke(&board.typec.provider);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
#endif
static const struct board_radio_profile *radio_profile(void)
{
    const struct a733_board_session *s=&board.session;
    if(s->radio_profile_address!=0x43000000||s->radio_profile_bytes!=sizeof(struct board_radio_profile))return NULL;
    for(uintptr_t page=0x43000000;page<0x43100000;page+=4096){
        struct aarch64_translation t;
        if(!aarch64_device_translate(page,0,&t)||t.physical!=page||t.attributes!=0xff||t.shareability!=3)return NULL;
    }
    const volatile uint8_t *source=(const void *)(uintptr_t)s->radio_profile_address;
    for(size_t i=0;i<sizeof(admitted_profile);++i)((uint8_t *)&admitted_profile)[i]=source[i];
    const struct board_radio_profile *p=&admitted_profile;
    uint8_t hash[32];storage_digest_bytes(p,sizeof(*p),hash);
    if(memcmp(hash,s->radio_profile_digest,32))return NULL;
#ifdef SUNSTONE_BOARD_SUPERVISED
    if(!board_radio_profile_runtime(&runtime_profile,p,s,runtime_profile_digest))return NULL;
    p=&runtime_profile;
#endif
    if(p->platform.handoff!=s->generation||
       p->platform.topology.identity!=s->topology||p->platform.lifetime_deadline_us>s->deadline_us-10000||
       p->ce.handoff!=s->generation||memcmp(p->platform.fabric,s->fabric,sizeof(s->fabric))||
       memcmp(p->platform.fabric_hz,s->fabric_hz,sizeof(s->fabric_hz))||
       memcmp(p->ce.fabric,s->fabric,sizeof(s->fabric))||memcmp(p->ce.fabric_hz,s->fabric_hz,sizeof(s->fabric_hz)))return NULL;
    uint64_t owners[]={s->pmic_owner,s->usb0_owner,s->typec_owner,s->usb2_owner,p->platform.owner,p->ce.owner};
    for(unsigned i=0;i<6;++i){
        if(!owners[i]){if(i==2||i==3){if(!a733_board_host_workload(s->workload))continue;}return NULL;}
        for(unsigned j=0;j<i;++j)if(owners[i]==owners[j])return NULL;
    }
    const struct board_radio_chip_id *id=board_radio_chip_id();
    if(!id||id->secure_read_record!=s->sid_authority)return NULL;
    return p;
}
int a733_storage_native_task_privileged(void (*entry)(void *),void *context)
{
#ifdef SUNSTONE_BOARD_APPLICATION
    if(board_application_task_privileged(entry,context)==1)return 1;
#endif
return !context&&(entry==pmic||entry==typec||entry==power||entry==board_host_task||entry==board_usb0_task||entry==radio_owner||entry==radio_client||entry==supervise);}
static int host_admit(void *context,const struct a733_usb2_platform_config *c)
{
    struct board_application *b=context;
    return b==&board&&board_live(NULL)&&!memcmp(c,&b->host_config,sizeof(*c))&&
        a733_board_session_valid(
#ifdef SUNSTONE_BOARD_SUPERVISED
            &b->admitted_session,
#else
            &b->session,
#endif
            a733_smp_counter())?1:-1;
}
void a733_smp_diagnostic_start(void)
{
    if(!a733_smp_device_preparation_admitted())platform_panic("board bootstrap",1);
    const struct a733_board_session *source=board_session_snapshot();
    if(!source)platform_panic("board sealed session",2);
    board.session=*source;
    if(!a733_board_session_valid(&board.session,a733_smp_counter())||
       board.session.epoch!=a733_cached_runtime[0].epoch||
#ifndef SUNSTONE_BOARD_SUPERVISED
       board.session.deadline_us*24!=a733_cached_runtime[0].diagnostic_deadline
#else
       a733_cached_runtime[0].startup_deadline<A733_BOARD_STARTUP_SECONDS*A733_SMP_COUNTER_HZ
#endif
       )platform_panic("board session",2);
#ifdef SUNSTONE_BOARD_SUPERVISED
    board.admitted_session=board.session;
    board.session.session_start_ticks=a733_cached_runtime[0].startup_deadline-A733_BOARD_STARTUP_SECONDS*A733_SMP_COUNTER_HZ;
    board.session.deadline_us=a733_cached_runtime[0].diagnostic_deadline/24;
#endif
    struct a733_board_session *s=&board.session;
    atomic_init(&board.radio_result,a733_board_radio_workload(s->workload)?1:0);
#ifndef SUNSTONE_BOARD_SUPERVISED
    board.window=(struct a733_watchdog_window){s->deadline_us,s->guard_us,s->rtc_control,s->rtc_divider,s->rtc_source};
    atomic_init(&board.watchdog_state,1);
    uint64_t initial=s->reload_before_ticks/24+s->minimum_reload_us-s->guard_us-1000;
    if(initial>s->deadline_us)initial=s->deadline_us;
    atomic_init(&board.live_until,initial);
    if(!a733_watchdog_hw_admit(&board.window))platform_panic("board watchdog",3);
    /* USB-only admission uses one fixed arm window. Radio renewals require
     * completed nonempty transfers under fixed operation/session deadlines. */
#else
    atomic_init(&board.watchdog_state,1);
    atomic_init(&board.live_until,s->deadline_us);
    atomic_init(&board.result,a733_board_host_workload(s->workload)?0:3);
#endif
    struct a733_native_usb_config b={.identity=s->generation};
    b.pmic.owner=s->pmic_owner;b.pmic.identity=s->generation;b.pmic.startup_deadline=s->deadline_us;
    for(unsigned i=0;i<2;++i)b.pmic.pins[i]=(struct a733_pin_config){s->pmic_pins[i][0],s->pmic_pins[i][1],s->pmic_pins[i][2]};
    b.pins.authorized[A733_PIN_L]=UINT32_C(3);
    if(a733_board_host_workload(s->workload))b.pins.authorized[A733_PIN_L]|=(UINT32_C(1)<<8)|(UINT32_C(1)<<9)|(UINT32_C(1)<<12)|(UINT32_C(1)<<13);
    b.pins.latch[0]=(struct a733_pin_latch_handoff){s->latch_identity,s->latch_owner,s->latch_known_mask,s->latch_value};
    if(a733_board_radio_workload(s->workload)){
        b.pins.authorized[A733_PIN_G]=0x3f;b.pins.authorized[A733_PIN_M]=3;
        b.pins.latch[1]=(struct a733_pin_latch_handoff){s->latch_m_identity,s->latch_m_owner,s->latch_m_known_mask,s->latch_m_value};
    }
    b.usb.owner=s->usb0_owner;b.usb.handoff=s->generation;b.usb.deadline_us=s->deadline_us;b.usb.cold_reference=s->usb0_cold_reference;
    for(unsigned i=0;i<4;++i)b.usb_supply_uv[i]=s->usb_supply_uv[i];
    for(unsigned i=0;i<3;++i){b.usb.fabric[i]=s->fabric[i];b.usb.rate_hz[i]=s->fabric_hz[i];}
#ifdef SUNSTONE_BOARD_APPLICATION
    if(board_application_pins(&b.pins)!=1)platform_panic("board application pins",10);
#endif
    if(!a733_native_usb_init(&providers.shared,&b,board_live,board_fail,NULL))platform_panic("board shared providers",4);
    TaskFunction_t functions[]={pmic,board_usb0_task,typec,power,board_host_task,radio_owner,radio_client,supervise};
    const char *names[]={"board-pmic","board-usb0","board-typec","board-power","board-usb2","board-radio","board-net","board-watchdog"};
    for(unsigned i=0;i<8;++i){
        if(i>=2&&i<=4&&!a733_board_host_workload(s->workload))continue;
        if(i>=5&&!a733_board_radio_workload(s->workload)
#ifdef SUNSTONE_BOARD_SUPERVISED
           &&i!=7
#endif
           )continue;
        board.handles[i]=xTaskCreateStatic(functions[i],names[i],2048,NULL,i==2||i==3?5:4,board.stacks[i],&board.tasks[i]);
        if(!board.handles[i])platform_panic("board tasks",5);
#ifdef SUNSTONE_BOARD_USB2_IRQ
        if(i==4)vTaskCoreAffinitySet(board.handles[i],1);
#endif
    }
    if(!a733_native_usb_bind(&providers.shared,board.handles[0],board.handles[1]))platform_panic("board bind",6);
    if(a733_board_host_workload(s->workload)){
    struct a733_typec_native_config tc={.clocks=&providers.shared.clocks,.pins=&providers.shared.pins,
        .rails=&providers.shared.rails,.pmic=&providers.shared.pmic,.locks=&providers.shared.locks,
        .provider_task=board.handles[2],.power_task=board.handles[3]};
    tc.electrical=(struct a733_typec_config){.owner=s->typec_owner,.generation=s->generation,
        .qualification=s->topology,.recovery=s->recovery_authority,.source_min_mv=s->source_min_mv,
        .source_max_mv=s->source_max_mv,.source_ma=s->source_ma,
        .zero_threshold_max_mv=s->zero_threshold_max_mv,.device_id=(uint8_t)s->fusb_device_id};
    for(unsigned i=0;i<4;++i)tc.expected[i]=(struct a733_pin_config){s->typec_pins[i][0],s->typec_pins[i][1],s->typec_pins[i][2]};
    if(!a733_typec_native_init(&board.typec,&tc))platform_panic("board typec",7);
    struct a733_usb2_platform_config *hc=&board.host_config;
    *hc=(struct a733_usb2_platform_config){.owner=s->usb2_owner,.identity=s->generation,.topology=s->topology,
        .deadline=s->deadline_us-10000,.lifetime_deadline=s->deadline_us-10000,.pck_client=s->pck_client,.revision=s->usb2_revision,.superspeed=s->workload==6||s->workload==8};
    memcpy(hc->fabric,s->fabric,sizeof(hc->fabric));memcpy(hc->fabric_hz,s->fabric_hz,sizeof(hc->fabric_hz));
    memcpy(hc->supply_uv,s->usb_supply_uv,sizeof(hc->supply_uv));
    struct device_memory_pool *pool=a733_memory_uncached_init();
    if(!pool||!a733_usb2_native_init(&board.host,hc,&providers.shared.locks,board.handles[4],pool,
       &providers.shared.clocks,&providers.shared.rails,&providers.shared.pmic,&providers.shared.gpu,
       &board.typec.provider,host_admit,&board))platform_panic("board usb2",8);
#ifdef SUNSTONE_BOARD_USB2_IRQ
    if(!board_host_irq_bind())platform_panic("board usb2 irq",8);
#endif
    }
    if(a733_board_radio_workload(s->workload)){
        const struct board_radio_profile *profile=radio_profile();
        if(!profile||!board_radio_startup(&radio,&providers.shared,&providers.ce,&providers,sizeof(providers),
           profile,
#ifdef SUNSTONE_BOARD_SUPERVISED
           runtime_profile_digest,
#else
           s->radio_profile_digest,
#endif
           board_radio_chip_id(),board.handles[5],board.handles[6],&board.watchdog_state))
            platform_panic("board radio",9);
    }
#ifdef SUNSTONE_BOARD_APPLICATION
    if(board_application_start()!=1)platform_panic("board application startup",11);
#endif
    vTaskStartScheduler();platform_panic("board scheduler returned",9);
}
