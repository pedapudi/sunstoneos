#include "bluetooth.h"
#include "a733_storage_digest.h"
#ifdef SUNSTONE_BLUETOOTH_HEADSET
#include "headset.h"
#ifdef SUNSTONE_BLUETOOTH_MSBC
#include "playback.h"
#endif
#endif
#ifdef SUNSTONE_BLUETOOTH_RECOGNITION
#include "recognition-models.h"
#endif
#include <string.h>
#include "bt_memory.h"
static int present(const uint8_t *p,size_t n)
{unsigned value=0;for(size_t i=0;i<n;++i)value|=p[i];return value!=0;}
int bluetooth_profile_valid(const struct bluetooth_profile *p,const uint8_t digest[32])
{
    if(!p||!digest||p->magic!=BLUETOOTH_PROFILE_MAGIC||!((p->version==1&&p->bytes==624)||(p->version==2&&p->bytes==752)||(p->version==3&&p->bytes==832)||(p->version==4&&p->bytes==sizeof(*p)))||
        !p->topology||!p->sid_authority||!p->radio_authority||!p->uart_authority||!p->wake_authority||
        !present(p->wiring_evidence,32)||!present(p->sid_evidence,32)||!present(p->fresh_transport_evidence,32)||
        p->radio.topology.identity!=p->topology||p->radio.handoff||p->radio.deadline_us||p->radio.lifetime_deadline_us||
        p->uart.handoff||p->uart.startup_deadline_us||p->uart.route!=A733_UART1_BLUETOOTH||
        p->uart.format.port!=1||p->uart.format.baud!=1500000||p->uart.format.clock_hz!=24000000||
        p->uart.format.data_bits!=8||p->uart.format.stop_half_bits!=2||p->uart.format.parity||!p->uart.format.auto_flow||
        !p->radio.owner||!p->uart.owner||p->radio.owner==p->uart.owner||!p->session_owner||
        p->wake.authority!=p->wake_authority||!p->pm_latch.identity||!p->pm_latch.owner||
        (p->pm_latch.known_mask!=0x3f
#ifdef SUNSTONE_A733_OS
         &&p->pm_latch.known_mask!=0
#endif
        )||(p->pm_latch.value&~p->pm_latch.known_mask)||
        !p->command_timeout_us||p->command_timeout_us>5000000||!p->frame_timeout_us||
        p->frame_timeout_us>p->command_timeout_us||!p->binding_timeout_us||
        !p->runtime_lease_us||!p->renew_margin_us||p->renew_margin_us>=p->runtime_lease_us)return 0;
    if(p->version==4){
        const struct a733_radio_bluetooth_stop *stop=&p->bluetooth_stop;
        if(!stop->authority||stop->topology!=p->topology||!present(stop->wiring_evidence,32)||
           stop->expected.function!=1||stop->expected.pull>2||stop->expected.drive>3||
           stop->inherited_level>1||stop->reserved||!(p->pm_latch.known_mask&4)||
           ((p->pm_latch.value>>2)&1)!=stop->inherited_level)return 0;
    }
    if(p->version>=2){
#ifndef SUNSTONE_BLUETOOTH_HEADSET
        return 0;
#else
        const struct bluetooth_headset_profile *h=&p->headset;
#ifndef SUNSTONE_BLUETOOTH_A2DP
        if(h->mode==6)return 0;
#endif
#ifndef SUNSTONE_BLUETOOTH_MSBC
#ifdef SUNSTONE_BLUETOOTH_A2DP
        if(p->version!=2||h->mode!=6||!h->incoming||(h->samples&1))return 0;
#else
        if(h->mode>=3)return 0;
#endif
#endif
        if(!((h->mode==1)||(h->mode==6&&p->version==2)||(h->mode==4&&p->version==2)||((h->mode==2||h->mode==3||h->mode==5)&&p->version>=3))||h->incoming>1||!present(h->peer,6)||h->pin[16]||h->allow_just_works>1||
            h->minimum_security<2||h->minimum_security>3||!h->server_channel||h->server_channel>30||
            !h->scheduling_authority||!present(h->scheduling_evidence,32)||!h->receive_latency_us||
            !h->response_timeout_us||h->receive_latency_us>=1000000||h->response_timeout_us>=1000000||
            !h->samples||h->samples>24000||(h->mode>=3&&h->mode!=6&&h->samples%120)||h->mtu<48||h->mtu>512||h->accept_slots<0x00a0||
            h->page_interval<0x12||h->page_interval>0x1000||(h->page_interval&1)||
            h->page_window<0x11||h->page_window>h->page_interval||!h->line_timeout_us||
            !h->capture_timeout_us||h->capture_timeout_us>5000000||h->peer_page_scan_mode>2||h->peer_role_switch>1||h->reserved2)return 0;
#endif
    }
    if(p->version>=3){
#ifdef SUNSTONE_BLUETOOTH_RECOGNITION
        for(unsigned i=0;i<2;++i)if(!p->recognition.bytes[i]||p->recognition.bytes[i]>BLUETOOTH_MODEL_SLOT||
            p->recognition.bytes[i]!=bluetooth_model_bytes[i]||
            memcmp(p->recognition.sha256[i],bluetooth_model_sha256[i],32))return 0;
#elif !defined(SUNSTONE_BLUETOOTH_CAPTURE_PROFILE)
        return 0;
#endif
        /* Capture authenticates the complete profile below. The application
         * manifest owns the model identities in its compatibility tail. */
    }
    uint8_t actual[32];storage_digest_bytes(p,p->bytes,actual);if(memcmp(actual,digest,32))return 0;
    for(unsigned i=0;i<4;++i){
        if(p->firmware_address[i]<UINT64_C(0x43010000)||p->firmware_address[i]>=UINT64_C(0x43100000)||
            !p->firmware_bytes[i]||p->firmware_bytes[i]>UINT64_C(0x43100000)-p->firmware_address[i])return 0;
        for(unsigned j=0;j<i;++j)if(p->firmware_address[i]<p->firmware_address[j]+p->firmware_bytes[j]&&
            p->firmware_address[j]<p->firmware_address[i]+p->firmware_bytes[i])return 0;
    }
    return 1;
}
static int platform_id(void *cookie,uint8_t bytes[16])
{
    const struct board_radio_chip_id *id=cookie;if(!id||!id->secure_read_record||!bytes)return 0;
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)bytes[4*i+j]=(uint8_t)(id->words[i]>>(8*j));return 1;
}
static int stop_admitted(const struct bluetooth_application *a,const struct a733_radio_platform *p)
{
    static const struct a733_radio_bluetooth_stop absent;
    const struct a733_radio_bluetooth_stop *expected=a->profile->version==4?&a->profile->bluetooth_stop:&absent;
    return !memcmp(&p->bluetooth_stop,expected,sizeof(*expected));
}
static int radio_admit(void *cookie,const struct a733_radio_platform_config *c,const struct a733_clock_token *token)
{
    struct bluetooth_application *a=cookie;
    return a->self==a&&a->profile->radio_authority&&a->profile->wake_authority&&
        !(atomic_load_explicit(a->recovery_state,memory_order_acquire)&4)&&
        !memcmp(c,&a->radio_config,sizeof(*c))&&token->owner==c->owner&&
        !memcmp(&a->radio.bluetooth,&a->profile->wake,sizeof(a->radio.bluetooth))&&
        stop_admitted(a,&a->radio);
}
static int uart_admit(void *cookie,const struct a733_uart_platform_config *c,const struct a733_clock_token *token,
    enum a733_uart_admission admission)
{
    struct bluetooth_application *a=cookie;
    /* UART setup precedes module enable/firmware startup. The authenticated
     * fresh-transport evidence excludes an inherited transmitting controller. */
    unsigned permitted=admission==A733_UART_ADMIT_START?!a->stage:
        admission==A733_UART_ADMIT_STOP&&atomic_load_explicit(&a->stop_release,memory_order_acquire)&&
        atomic_load_explicit(&a->stop_peer,memory_order_acquire)==1;
    return a->self==a&&permitted&&a->profile->uart_authority&&
        !(atomic_load_explicit(a->recovery_state,memory_order_acquire)&4)&&
        !memcmp(c,&a->uart_config,sizeof(*c))&&token->owner==c->owner;
}
static int binding_current(void *cookie,uint64_t firmware,uint64_t uart)
{
    struct bluetooth_application *a=cookie;
    /* The owner preflights both real live guards immediately before this one
     * bounded HCI step. No resource can be released/replaced by another owner.
     * Irreversible shared faults still revoke this short-lived admission. */
    return a&&a->self==a&&a->iteration_live&&xTaskGetCurrentTaskHandle()==a->owner&&
        firmware==a->firmware_generation&&firmware==a->module_session->borrow.generation&&
        uart==a->uart_generation&&uart==a->uart.clock.id&&!a->module_session->fault&&!a->module_radio->fault&&!a->uart.fault&&
        !atomic_load_explicit(&a->shared->counter.failed,memory_order_acquire)&&
        !atomic_load_explicit(&a->shared->clocks.failed,memory_order_acquire)&&
        !(atomic_load_explicit(a->recovery_state,memory_order_acquire)&4);
}
/* Supervised offsets of the firmware bound and of HCI identification from the
 * schedule origin. Recognition and wideband playback reserve the final 20 s. */
static uint64_t bootstrap_offset(const struct bluetooth_profile *p)
{return (p->version>=3||(p->version==2&&p->headset.mode==4))?30000000:40000000;}
static uint64_t hci_offset(const struct bluetooth_profile *p)
{return (p->version>=3||(p->version==2&&p->headset.mode==4))?35000000:p->version==2?45000000:55000000;}
/* Checks and fields shared by both constructors, then the UART provider. */
static int prepare(struct bluetooth_application *a,const struct bluetooth_profile *p,
    struct a733_native_usb *shared,const struct board_radio_chip_id *id,TaskHandle_t owner,
    atomic_uint *recovery,uint64_t start,uint64_t end,const uint8_t digest[32])
{
    if(!a||a->self||!p||!shared||!shared->used||!id||id->secure_read_record!=p->sid_authority||!owner||!recovery||
        xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||end<=start||
        (end-start!=BLUETOOTH_SUPERVISED_US&&end!=BLUETOOTH_RESIDENT_END_US)||
        !bluetooth_profile_valid(p,digest)||
        !bt_range_disjoint(a,sizeof(*a),p,sizeof(*p))||!bt_range_disjoint(a,sizeof(*a),shared,sizeof(*shared))||
        !bt_range_disjoint(a,sizeof(*a),id,sizeof(*id))||!bt_range_disjoint(a,sizeof(*a),recovery,sizeof(*recovery)))return 0;
    a->self=a;a->profile=p;a->shared=shared;a->chip_id=id;a->owner=owner;a->recovery_state=recovery;
    a->resident=end==BLUETOOTH_RESIDENT_END_US;
    a->started=start;a->deadline=end-10000;a->hci_deadline=start+hci_offset(p);
    a->bootstrap_deadline=start+(a->resident?BLUETOOTH_RESIDENT_BOOTSTRAP_US:bootstrap_offset(p));
    atomic_init(&a->failed,1);atomic_init(&a->result_lock,0);atomic_init(&a->result,0);
    atomic_init(&a->stop_release,0);atomic_init(&a->stop_peer,0);
    a->uart_config=p->uart;a->uart_config.handoff=shared->boot.identity;a->uart_config.startup_deadline_us=start+2000000;
    for(unsigned i=0;i<4;++i)a->slots[i]=(struct bt_h4_storage){a->packets[i],sizeof(a->packets[i])};
    return a733_uart_platform_native_init(&a->uart_native,&shared->locks,owner,1,uart_admit,a)&&
        a733_uart_platform_init(&a->uart,&a->uart_config,&a->uart_native.operations,&a->uart_native,&shared->clocks,&shared->pins);
}
int bluetooth_application_init(struct bluetooth_application *a,const struct bluetooth_profile *p,
    struct a733_native_usb *shared,const struct board_radio_chip_id *id,TaskHandle_t owner,
    atomic_uint *recovery,uint64_t start,uint64_t end,const uint8_t digest[32])
{
    if(!prepare(a,p,shared,id,owner,recovery,start,end,digest))return 0;
    a->module_radio=&a->radio;a->module_session=&a->session;
    a->radio_config=p->radio;a->radio_config.handoff=shared->boot.identity;
    a->radio_config.deadline_us=a->bootstrap_deadline;a->radio_config.lifetime_deadline_us=a->deadline;
    if(!a733_radio_platform_native_init(&a->radio_native,&shared->locks,owner,radio_admit,a)||
        !a733_radio_platform_init(&a->radio,&a->radio_config,&a->radio_native.operations,&a->radio_native,
            &shared->clocks,&shared->pins,&shared->rails,&shared->pmic)||
        !a733_radio_platform_four_bit(&a->radio)||
        !a733_radio_platform_bluetooth(&a->radio,&p->wake)||
        (p->version==4&&!a733_radio_platform_bluetooth_stop(&a->radio,&p->bluetooth_stop)))return 0;
    a->session_config=(struct a733_radio_session_config){.owner_task=owner,.owner=p->session_owner,
        .bootstrap_deadline=a->radio_config.deadline_us,.runtime_lease_us=p->runtime_lease_us,.renew_margin_us=p->renew_margin_us,
        .transport=p->transport,.bootstrap=p->bootstrap,.firmware={.identity=p->identity,.profile=1},
        .runtime={platform_id,(void *)id,p->binding_timeout_us}};
    for(unsigned i=0;i<4;++i)a->session_config.firmware.blobs[i]=(struct aic8800_firmware_blob){
        (const uint8_t *)(uintptr_t)p->firmware_address[i],(size_t)p->firmware_bytes[i]};
    if(!a733_radio_session_init_bootstrap(&a->session,&a->radio,&a->session_config))return 0;
    atomic_store_explicit(&a->failed,0,memory_order_release);return 1;
}
/* The attached module's own admission checks its platform configuration;
 * this added one authenticates the Bluetooth wake extension. The pins belong
 * to the module platform once attached, so a client failure does not refuse
 * them: that would end the module and Wi-Fi with the client. */
static int wake_admit(void *cookie,const struct a733_radio_platform_config *c,const struct a733_clock_token *token)
{
    struct bluetooth_application *a=cookie;
    return a->self==a&&a->attached&&c==&a->module_radio->config&&token->owner==c->owner&&
        a->profile->radio_authority&&a->profile->wake_authority&&
        !memcmp(&a->module_radio->bluetooth,&a->profile->wake,sizeof(a->profile->wake))&&
        stop_admitted(a,a->module_radio);
}
int bluetooth_application_attach(struct bluetooth_application *a,const struct bluetooth_profile *p,
    struct a733_native_usb *shared,const struct board_radio_chip_id *id,TaskHandle_t owner,
    atomic_uint *latch,uint64_t start,uint64_t end,const uint8_t digest[32],
    struct a733_radio_platform_native *native,struct a733_radio_session *module)
{
    if(!a||a->self||!p||!native||!module||module->self!=module||module->fault||module->bootstrap_only||
        !module->platform||module->platform->cookie!=native||native->owner_task!=(void *)owner||
        module->config.owner_task!=owner||module->config.owner!=p->session_owner||
        module->platform->config.owner!=p->radio.owner||module->platform->config.topology.identity!=p->topology||
        memcmp(&module->config.firmware.identity,&p->identity,sizeof(p->identity))||
        !bt_range_disjoint(a,sizeof(*a),module,sizeof(*module))||!bt_range_disjoint(a,sizeof(*a),native,sizeof(*native))||
        !bt_range_disjoint(a,sizeof(*a),module->platform,sizeof(*module->platform)))return 0;
    for(unsigned i=0;i<4;++i)if((uintptr_t)module->config.firmware.blobs[i].data!=p->firmware_address[i]||
        module->config.firmware.blobs[i].bytes!=p->firmware_bytes[i])return 0;
    if(!prepare(a,p,shared,id,owner,latch,start,end,digest))return 0;
    a->module_radio=module->platform;a->module_session=module;a->attached=1;
    if(!a733_radio_platform_bluetooth(a->module_radio,&p->wake)||
        (p->version==4&&!a733_radio_platform_bluetooth_stop(a->module_radio,&p->bluetooth_stop))||
        !a733_radio_platform_native_extend(native,wake_admit,a))return 0;
    atomic_store_explicit(&a->failed,0,memory_order_release);return 1;
}
int bluetooth_application_retain(struct bluetooth_application *a)
{
    if(!a||a->self!=a||xTaskGetCurrentTaskHandle()!=a->owner)return -1;
    atomic_fetch_or_explicit(a->recovery_state,4,memory_order_acq_rel);
    atomic_store_explicit(&a->failed,1,memory_order_release);a->iteration_live=0;
#ifdef SUNSTONE_BLUETOOTH_HEADSET
    (void)bluetooth_headset_retain(a);
#endif
#ifdef SUNSTONE_BLUETOOTH_RECOGNITION
    bluetooth_recognition_abort();
#endif
    bt_h4_fail(&a->hci.transport);
    (void)a733_uart_platform_retain(&a->uart);
    /* An attached client leaves the module to its owner. */
    if(!a->attached)(void)a733_radio_session_retain(&a->session);
    return -1;
}
/* Before HCI starts, repeat whole session steps while they advance, as the
 * board radio owner does. The between-step budget permits one bounded step
 * beyond 500 us. Waiting work yields without spinning; runtime stays one poll. */
#define BLUETOOTH_BOOTSTRAP_POLL_BUDGET_US 500u
static int poll_module(struct bluetooth_application *a,uint64_t *now)
{
    if(a->stage!=1){
        enum a733_radio_session_result r=a733_radio_session_poll(&a->session);
        return r!=A733_SESSION_RETAINED&&r!=A733_SESSION_INVALID;
    }
    for(uint64_t start=*now;;){
        uint64_t host=a->radio.host.progress,transport=a->session.transport.progress;
        unsigned platform=a->radio.stage,session=a->session.stage;
        enum a733_radio_session_result r=a733_radio_session_poll(&a->session);
        if(r==A733_SESSION_RETAINED||r==A733_SESSION_INVALID)return 0;
        if(!a733_counter_clock_now(&a->shared->counter,now))return 0;
        if(a->session.dispatcher.initialized||r!=A733_SESSION_PENDING||*now>=a->bootstrap_deadline||
           *now-start>=BLUETOOTH_BOOTSTRAP_POLL_BUDGET_US||
           (host==a->radio.host.progress&&transport==a->session.transport.progress&&
            platform==a->radio.stage&&session==a->session.stage))return 1;
    }
}
enum { STOP_NONE,STOP_UART_START,STOP_PROTOCOL,STOP_RESET,STOP_MODULE,
       STOP_RX,STOP_UART,STOP_DONE,STOP_FAULT };
static int stop_fault(struct bluetooth_application *a)
{
    if(a->stop_phase!=STOP_FAULT){a->stop_fault=a->stop_phase;a->stop_phase=STOP_FAULT;
        (void)bluetooth_application_retain(a);}
    return -1;
}
int bluetooth_application_module_release(struct bluetooth_application *a)
{
    if(!a||a->self!=a||atomic_load_explicit(&a->failed,memory_order_acquire))return -1;
    return atomic_load_explicit(&a->stop_release,memory_order_acquire)?1:0;
}
void bluetooth_application_module_down(struct bluetooth_application *a,int proved)
{
    if(!a||a->self!=a||xTaskGetCurrentTaskHandle()!=a->owner||!a->attached)return;
    /* A later module fault cannot erase a previously published PM2 proof.
     * Conversely, an earlier failed proof cannot become successful. */
    unsigned pending=0;
    (void)atomic_compare_exchange_strong_explicit(&a->stop_peer,&pending,proved==1?1u:2u,
        memory_order_release,memory_order_relaxed);
}
int bluetooth_application_stop(struct bluetooth_application *a,uint64_t deadline)
{
    if(!a||a->self!=a||xTaskGetCurrentTaskHandle()!=a->owner||!deadline)return -1;
    if(a->stop_phase==STOP_FAULT)return -1;
    if(a->stop_phase==STOP_DONE)return atomic_load_explicit(&a->failed,memory_order_acquire)?-1:1;
    uint64_t now;
    if(atomic_load_explicit(&a->failed,memory_order_acquire)||
       (atomic_load_explicit(a->recovery_state,memory_order_acquire)&4)||
       !a733_counter_clock_now(&a->shared->counter,&now))return stop_fault(a);
    if(deadline>a->deadline)deadline=a->deadline;
    if(!a->stop_deadline||deadline<a->stop_deadline)a->stop_deadline=deadline;
    if(now>=a->stop_deadline)return stop_fault(a);
    a->iteration_live=0;
    if(!a->stop_phase){
        a->stop_phase=a->stage?STOP_PROTOCOL:STOP_UART_START;
        /* Stop the standalone SDIO producer immediately. Its accepted request
         * drains while HCI retires; physical teardown waits for release. */
        if(!a->attached&&a733_radio_session_stop(&a->session,a->stop_deadline)!=1)return stop_fault(a);
        if(a->stage>=2){
#ifdef SUNSTONE_BLUETOOTH_HEADSET
            if(a->profile->version>=2&&bluetooth_headset_stop_begin(a,now,a->stop_deadline)!=1)return stop_fault(a);
#endif
            if(bt_hci_scan_disable(&a->hci,a->stop_deadline)!=1)return stop_fault(a);
        }
    }
    if(a->stop_phase==STOP_UART_START){
        enum a733_uart_platform_result r=a733_uart_platform_poll(&a->uart);
        if(r==A733_UART_PLATFORM_RETAINED||r==A733_UART_PLATFORM_INVALID)return stop_fault(a);
        if(r==A733_UART_PLATFORM_READY){a->uart_generation=a->uart.clock.id;a->stage=1;a->stop_phase=STOP_PROTOCOL;}
        return 0;
    }
    if(!a->attached&&a->stop_phase<=STOP_MODULE){
        enum a733_radio_session_result r=a733_radio_session_poll(&a->session);
        if(r==A733_SESSION_RETAINED||r==A733_SESSION_INVALID)return stop_fault(a);
        if(r==A733_SESSION_DOWN){
            unsigned proved=a733_radio_session_peer_stopped(&a->session)==1;
            atomic_store_explicit(&a->stop_peer,proved?1u:2u,memory_order_release);
        }
    }
    if(a->stop_phase==STOP_PROTOCOL||a->stop_phase==STOP_RESET){
        if(a->stage<2){
            /* No HCI packet exists. The module owner still must prove the
             * actual Bluetooth peer stopped before UART can drain. */
            atomic_store_explicit(&a->stop_release,1,memory_order_release);
            a->stop_phase=STOP_MODULE;return 0;
        }
        /* The shared module owner requests its stop before releasing physical
         * resources. A notification can reach this client first. */
        if(a->attached&&!a->module_session->stop_requested)return 0;
        int held=a733_radio_session_stop_held(a->module_session);
        if(held<0)return stop_fault(a);if(!held)return 0;
        enum a733_uart_platform_result u=a733_uart_platform_live(&a->uart,a->stop_deadline);
        if(u==A733_UART_PLATFORM_BUSY)return 0;if(u!=A733_UART_PLATFORM_READY)return stop_fault(a);
        a->iteration_live=1;
        if(a->stop_phase==STOP_PROTOCOL){
            int done;
#ifdef SUNSTONE_BLUETOOTH_HEADSET
            if(a->profile->version>=2)done=bluetooth_headset_stop(a,now,a->stop_deadline);
            else
#endif
            {done=bt_hci_step(&a->hci,64,a->stop_deadline)<0?-1:bt_hci_scan_disabled(&a->hci);}
            a->iteration_live=0;
            if(done<0)return stop_fault(a);if(!done)return 0;
            if(bt_hci_stop(&a->hci,a->stop_deadline)!=1)return stop_fault(a);
            a->stop_phase=STOP_RESET;return 0;
        }
        int r=bt_hci_step(&a->hci,64,a->stop_deadline);
        a->iteration_live=0;
        if(r<0)return stop_fault(a);
        r=bt_hci_stopped(&a->hci);if(r<0)return stop_fault(a);if(!r)return 0;
        atomic_store_explicit(&a->stop_release,1,memory_order_release);
        a->stop_phase=STOP_MODULE;return 0;
    }
    if(a->stop_phase==STOP_MODULE){
        if(!a->attached&&a733_radio_session_stop_release(&a->session)!=1)return stop_fault(a);
        unsigned peer=atomic_load_explicit(&a->stop_peer,memory_order_acquire);
        if(!peer)return 0;if(peer!=1)return stop_fault(a);
        a->stop_phase=STOP_RX;return 0;
    }
    if(a->stop_phase==STOP_RX){
        enum a733_uart_platform_result u=a733_uart_platform_live(&a->uart,a->stop_deadline);
        if(u==A733_UART_PLATFORM_BUSY)return 0;if(u!=A733_UART_PLATFORM_READY)return stop_fault(a);
        /* PM2 reset-low is already authenticated. No peer can add bytes;
         * consume only the finite residual FIFO, preserving line errors. */
        struct a733_uart_byte bytes[64];struct a733_uart_result result;
        struct a733_uart *uart=a733_uart_platform_controller(&a->uart);
        if(!uart||a733_uart_exchange(uart,bytes,64,NULL,0,64,a->stop_deadline,&result)!=A733_UART_READY||result.errors)
            return stop_fault(a);
        if(result.received)return 0;
        if(a733_uart_platform_close(&a->uart,1,a->stop_deadline)!=A733_UART_PLATFORM_PENDING)return stop_fault(a);
        a->stop_phase=STOP_UART;return 0;
    }
    if(a->stop_phase==STOP_UART){
        enum a733_uart_platform_result r=a733_uart_platform_poll(&a->uart);
        if(r==A733_UART_PLATFORM_RETAINED||r==A733_UART_PLATFORM_INVALID)return stop_fault(a);
        if(r==A733_UART_PLATFORM_CLOSED){a->stop_phase=STOP_DONE;return 1;}
        return 0;
    }
    return stop_fault(a);
}
int bluetooth_application_poll(struct bluetooth_application *a)
{
    if(!a||a->self!=a||xTaskGetCurrentTaskHandle()!=a->owner)return -1;
    if(a->stop_phase)return bluetooth_application_stop(a,a->stop_deadline);
#ifdef SUNSTONE_BLUETOOTH_MSBC
    if(bluetooth_playback_mode(a)&&!atomic_load_explicit(&a->result,memory_order_acquire)&&bluetooth_playback_cancelled(a))return bluetooth_application_retain(a);
#endif
    uint64_t now;
    if(atomic_load_explicit(&a->failed,memory_order_acquire)||
        (atomic_load_explicit(a->recovery_state,memory_order_acquire)&4)||
        !a733_counter_clock_now(&a->shared->counter,&now)||now>=a->deadline||
        (a->attached&&(a->module_session->fault||a->module_radio->fault)))return bluetooth_application_retain(a);
    if(!a->stage){
        enum a733_uart_platform_result r=a733_uart_platform_poll(&a->uart);
        if(r==A733_UART_PLATFORM_RETAINED||r==A733_UART_PLATFORM_INVALID)return bluetooth_application_retain(a);
        if(r==A733_UART_PLATFORM_READY){a->uart_generation=a->uart.clock.id;a->stage=1;}
        return 0;
    }
    if(!a->attached){
        /* An attached module's owner polls its session and drains its events. */
        if(!poll_module(a,&now))return bluetooth_application_retain(a);
        if(a->session.dispatcher.initialized){
            (void)a733_radio_session_take_event(&a->session,&a->event);
            (void)a733_radio_session_take_record(&a->session,&a->diagnostic);
        }
    }
    uint64_t generation;int ready=a733_radio_session_bootstrap(a->module_session,&generation);
    if(ready<0)return bluetooth_application_retain(a);
    if(a->stage==1){
        /* Readiness can follow the session's switch to a runtime lease. Bind
         * its first observation to this client's original bootstrap bound. */
        if(!a733_counter_clock_now(&a->shared->counter,&now))return bluetooth_application_retain(a);
        if(!a->firmware_generation){
            if(now>=a->bootstrap_deadline)return bluetooth_application_retain(a);
            if(ready){
                if(!generation)return bluetooth_application_retain(a);
                a->firmware_generation=generation;
                if(a->resident){
                    a->started=now;
                    uint64_t end=a733_deadline_after_saturating(now,hci_offset(a->profile)-bootstrap_offset(a->profile));
                    a->hci_deadline=end<a->deadline?end:a->deadline;
                }
            }
        }
    }
    /* An on-time observation survives UART/wake contention, but neither that
     * contention nor temporary module unavailability extends HCI startup. */
    if(a->firmware_generation&&!atomic_load_explicit(&a->result,memory_order_acquire)&&now>=a->hci_deadline)
        return bluetooth_application_retain(a);
    if(!ready)return 0;
    if(generation!=a->firmware_generation)return bluetooth_application_retain(a);
    enum a733_uart_platform_result u=a733_uart_platform_live(&a->uart,a->deadline);
    if(u==A733_UART_PLATFORM_BUSY)return 0;if(u!=A733_UART_PLATFORM_READY)return bluetooth_application_retain(a);
    enum a733_radio_platform_result wake=a733_radio_platform_bluetooth_wake(a->module_radio,&a->module_session->borrow,
        a->module_session->deadline,&a->host_wake);
    if(wake==A733_RADIO_BUSY)return 0;if(wake!=A733_RADIO_READY)return bluetooth_application_retain(a);
    a->iteration_live=1;
#ifdef SUNSTONE_BLUETOOTH_HEADSET
    if(a->profile->version>=2&&atomic_load_explicit(&a->result,memory_order_acquire)){
        int r=bluetooth_headset_poll(a,now);a->iteration_live=0;
        return r<0?bluetooth_application_retain(a):1;
    }
#endif
    if(a->stage==1){
        a->binding=(struct bt_h4_binding){a733_uart_platform_controller(&a->uart),generation,a->uart_generation,
            a->profile->frame_timeout_us,1500000,binding_current,a};
        if(bt_hci_init(&a->hci,&a->binding,a->slots,4,a->hci_deadline,a->profile->command_timeout_us)!=1)
            return bluetooth_application_retain(a);
        a->stage=2;
    }
    if(generation!=a->firmware_generation||bt_hci_step(&a->hci,64,a->deadline)<0)return bluetooth_application_retain(a);
    a->iteration_live=0;
    const struct bt_h4_record *record=bt_hci_next(&a->hci);
    if(record&&a->profile->version==1){uint64_t serial=record->serial;if(bt_hci_release(&a->hci,serial)!=1)return bluetooth_application_retain(a);
        if(a->discarded_records!=UINT64_MAX)++a->discarded_records;}
    if(a->hci.state==BT_HCI_IDENTIFIED&&!atomic_load_explicit(&a->result,memory_order_acquire)){
        unsigned unlocked=0;if(atomic_compare_exchange_strong_explicit(&a->result_lock,&unlocked,1,memory_order_acquire,memory_order_relaxed)){
            a->identity=a->hci.identity;atomic_store_explicit(&a->result,1,memory_order_release);
            atomic_store_explicit(&a->result_lock,0,memory_order_release);
        }
    }
    return atomic_load_explicit(&a->result,memory_order_acquire)?1:0;
}
int bluetooth_application_result(struct bluetooth_application *a,struct bt_hci_identity *out)
{
    if(!a||a->self!=a||!out||!bt_range_disjoint(a,sizeof(*a),out,sizeof(*out))||
        !bt_range_disjoint(a->shared,sizeof(*a->shared),out,sizeof(*out))||
        !bt_range_disjoint(a->profile,sizeof(*a->profile),out,sizeof(*out)))return -1;
    if(atomic_load_explicit(&a->failed,memory_order_acquire)||(atomic_load_explicit(a->recovery_state,memory_order_acquire)&4))return -1;
    if(!atomic_load_explicit(&a->result,memory_order_acquire))return 0;
    unsigned unlocked=0;if(!atomic_compare_exchange_strong_explicit(&a->result_lock,&unlocked,1,memory_order_acquire,memory_order_relaxed))return 0;
    *out=a->identity;atomic_store_explicit(&a->result_lock,0,memory_order_release);
    return atomic_load_explicit(&a->failed,memory_order_acquire)||
        (atomic_load_explicit(a->recovery_state,memory_order_acquire)&4)?-1:1;
}

#ifdef SUNSTONE_BLUETOOTH_RECOGNITION
int bluetooth_recognition_output(struct bluetooth_application *a,const void *out,size_t bytes)
{return a&&bt_range_disjoint(a,sizeof(*a),out,bytes)&&bt_range_disjoint(a->profile,sizeof(*a->profile),out,bytes)&&
    bt_range_disjoint(a->shared,sizeof(*a->shared),out,bytes);}
int bluetooth_recognition_live(struct bluetooth_application *a)
{return a&&a->self==a&&a->profile->version>=3&&!atomic_load_explicit(&a->failed,memory_order_acquire)&&
    !(atomic_load_explicit(a->recovery_state,memory_order_acquire)&4);}
#endif
