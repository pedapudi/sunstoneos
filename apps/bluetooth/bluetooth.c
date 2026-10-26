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
    if(!p||!digest||p->magic!=BLUETOOTH_PROFILE_MAGIC||!((p->version==1&&p->bytes==624)||(p->version==2&&p->bytes==752)||(p->version==3&&p->bytes==sizeof(*p)))||
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
    if(p->version>=2){
#ifndef SUNSTONE_BLUETOOTH_HEADSET
        return 0;
#else
        const struct bluetooth_headset_profile *h=&p->headset;
#ifndef SUNSTONE_BLUETOOTH_MSBC
        if(h->mode>=3)return 0;
#endif
        if(!((h->mode==1)||(h->mode==4&&p->version==2)||((h->mode==2||h->mode==3||h->mode==5)&&p->version==3))||h->incoming>1||!present(h->peer,6)||h->pin[16]||h->allow_just_works>1||
            h->minimum_security<2||h->minimum_security>3||!h->server_channel||h->server_channel>30||
            !h->scheduling_authority||!present(h->scheduling_evidence,32)||!h->receive_latency_us||
            !h->response_timeout_us||h->receive_latency_us>=1000000||h->response_timeout_us>=1000000||
            !h->samples||h->samples>24000||(h->mode>=3&&h->samples%120)||h->mtu<48||h->mtu>512||h->accept_slots<0x00a0||
            h->page_interval<0x12||h->page_interval>0x1000||(h->page_interval&1)||
            h->page_window<0x11||h->page_window>h->page_interval||!h->line_timeout_us||
            !h->capture_timeout_us||h->capture_timeout_us>5000000||h->peer_page_scan_mode>2||h->peer_role_switch>1||h->reserved2)return 0;
#endif
    }
    if(p->version==3){
#ifndef SUNSTONE_BLUETOOTH_RECOGNITION
        return 0;
#else
        for(unsigned i=0;i<2;++i)if(!p->recognition.bytes[i]||p->recognition.bytes[i]>BLUETOOTH_MODEL_SLOT||
            p->recognition.bytes[i]!=bluetooth_model_bytes[i]||
            memcmp(p->recognition.sha256[i],bluetooth_model_sha256[i],32))return 0;
#endif
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
static int radio_admit(void *cookie,const struct a733_radio_platform_config *c,const struct a733_clock_token *token)
{
    struct bluetooth_application *a=cookie;
    return a->self==a&&a->profile->radio_authority&&a->profile->wake_authority&&
        !(atomic_load_explicit(a->recovery_state,memory_order_acquire)&4)&&
        !memcmp(c,&a->radio_config,sizeof(*c))&&token->owner==c->owner&&
        !memcmp(&a->radio.bluetooth,&a->profile->wake,sizeof(a->radio.bluetooth));
}
static int uart_admit(void *cookie,const struct a733_uart_platform_config *c,const struct a733_clock_token *token,
    enum a733_uart_admission admission)
{
    struct bluetooth_application *a=cookie;
    /* UART setup precedes module enable/firmware startup. The authenticated
     * fresh-transport evidence excludes an inherited transmitting controller. */
    return a->self==a&&admission==A733_UART_ADMIT_START&&!a->stage&&a->profile->uart_authority&&
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
{return (p->version==3||(p->version==2&&p->headset.mode==4))?30000000:40000000;}
static uint64_t hci_offset(const struct bluetooth_profile *p)
{return (p->version==3||(p->version==2&&p->headset.mode==4))?35000000:p->version==2?45000000:55000000;}
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
        !a733_radio_platform_bluetooth(&a->radio,&p->wake))return 0;
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
        !memcmp(&a->module_radio->bluetooth,&a->profile->wake,sizeof(a->profile->wake));
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
int bluetooth_application_poll(struct bluetooth_application *a)
{
    if(!a||a->self!=a||xTaskGetCurrentTaskHandle()!=a->owner)return -1;
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
        enum a733_radio_session_result s=a733_radio_session_poll(&a->session);
        if(s==A733_SESSION_RETAINED||s==A733_SESSION_INVALID)return bluetooth_application_retain(a);
        if(a->session.dispatcher.initialized){
            (void)a733_radio_session_take_event(&a->session,&a->event);
            (void)a733_radio_session_take_record(&a->session,&a->diagnostic);
        }
    }
    uint64_t generation;int ready=a733_radio_session_bootstrap(a->module_session,&generation);
    if(ready<0)return bluetooth_application_retain(a);
    if(!ready)return a->stage==1&&now>=a->bootstrap_deadline?bluetooth_application_retain(a):0;
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
        if(a->resident){
            /* Place the origin where the supervised firmware bound would fall. */
            uint64_t offset=bootstrap_offset(a->profile);
            a->started=now>offset?now-offset:0;a->hci_deadline=a->started+hci_offset(a->profile);
        }
        a->firmware_generation=generation;
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
{return a&&a->self==a&&a->profile->version==3&&!atomic_load_explicit(&a->failed,memory_order_acquire)&&
    !(atomic_load_explicit(a->recovery_state,memory_order_acquire)&4);}
#endif
