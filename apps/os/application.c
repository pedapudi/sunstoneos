/* Application execution and the EL0-safe calls. Device ownership stays in the
 * USB, capture, thermal and clock service tasks. */
#include "os.h"
#include "application.h"
#include "../board/application.h"
#include "../board/session.h"
#include "a733_mapping_check.h"
#include "a733_storage_digest.h"
#include "sunstone_crumb.h"
#include "boot.h"
#ifdef SUNSTONE_A733_OS_USB_CLASSES
#include "sunstone_usb_service.h"
#endif
#ifdef SUNSTONE_NEWLIB
#include "runtime.h"
#endif
#include <string.h>
#ifdef SUNSTONE_A733_OS_HEADSET
#include "../bluetooth/bluetooth.h"
#endif
#ifdef SUNSTONE_A733_OS_CPU_RETURN
#include "cpu-return.h"
#endif
static struct ss_grant grant;
static atomic_uint prepared,finished,usb_ready,activity_generation;
static atomic_uint_fast64_t observed_until;
static struct a733_typec_token attachment;
static TaskHandle_t main_task;
static atomic_uint_fast64_t finish_by;
const struct ss_grant *ss_grant(void)
{return atomic_load_explicit(&prepared,memory_order_acquire)?&grant:NULL;}
int os_application_finished(void)
{return atomic_load_explicit(&finished,memory_order_acquire);}
void os_application_usb_ready(void)
{atomic_store_explicit(&usb_ready,1,memory_order_release);}
int ss_live(void)
{
    if(!board_live(NULL)||os_application_finished()||atomic_load_explicit(&board.result,memory_order_acquire)==2)return -1;
    if(!ss_grant()||!atomic_load_explicit(&usb_ready,memory_order_acquire))return 0;
    if(!(grant.devices&SS_DEVICE_USB_STORAGE))return atomic_load_explicit(&board.result,memory_order_acquire)==3;
    return a733_smp_counter()/24<atomic_load_explicit(&observed_until,memory_order_acquire);
}
void ss_progress(void){board_service_progress(OS_SERVICE_APPLICATION);}
void os_application_usb_changed(void)
{if(main_task)xTaskNotifyGive(main_task);}
void ss_usb_wait(uint32_t wait_us)
{
    TickType_t ticks=pdMS_TO_TICKS(((uint64_t)wait_us+999)/1000);
    (void)ulTaskNotifyTake(pdTRUE,ticks?ticks:1);
}
int ss_throttled(void){return os_thermal_throttled();}
int ss_clock_ready(void)
{
#ifdef SUNSTONE_A733_OS_CPU_CLOCK
    return os_cpu_clock_ready();
#else
    return 1;
#endif
}
void ss_activity_begin(void)
{
#ifdef SUNSTONE_A733_OS_CPU_CLOCK
    void os_thermal_record_begin(void);
    atomic_fetch_add_explicit(&activity_generation,1,memory_order_relaxed);
    os_thermal_record_begin();
#endif
}
void ss_activity_end(int success)
{
#ifdef SUNSTONE_A733_OS_CPU_CLOCK
    void os_thermal_record_end(int);os_thermal_record_end(success);
#else
    (void)success;
#endif
}
int ss_clock_info(char *line,size_t bytes)
{
#ifdef SUNSTONE_A733_OS_CPU_CLOCK
    int os_cpu_clock_line(char *,size_t);return os_cpu_clock_line(line,bytes);
#else
    (void)line;(void)bytes;return 0;
#endif
}
size_t ss_heap_peak(void)
{
#ifdef SUNSTONE_NEWLIB
    return runtime_heap_peak();
#else
    return 0;
#endif
}
unsigned os_application_activity_generation(void)
{return atomic_load_explicit(&activity_generation,memory_order_relaxed);}
/* Only the USB2 owner accesses mutable attachment state. */
void os_application_host_observed(int ready)
{
    BOARD_PROGRESS(BOARD_SERVICE_USB2);
    if(!ready)return;
#ifdef SUNSTONE_A733_OS_USB_CLASSES
    if(!(grant.devices&SS_DEVICE_USB_STORAGE)){
        atomic_store_explicit(&board.result,3,memory_order_release);return;
    }
#endif
    enum a733_usb2_device_kind expected=(grant.devices&SS_DEVICE_USB_KEYBOARD)?A733_USB2_DEVICE_DUAL:A733_USB2_DEVICE_MSC;
    if(a733_usb2_device_kind(&board.host)!=expected){board_fail(NULL);return;}
    int live=board.host.core.ops.live?board.host.core.ops.live(board.host.core.context,
        board.host.core.generation,board.host.platform.config.lifetime_deadline):-1;
    if(live<0){atomic_store_explicit(&observed_until,0,memory_order_release);return;}
    if(!live)return;
    attachment=board.host.platform.attachment;
    atomic_store_explicit(&observed_until,attachment.observed_us+A733_TYPEC_FRESH_US,memory_order_release);
}
int os_application_export_live(void)
{
    if(!board_live(NULL)||atomic_load_explicit(&board.result,memory_order_acquire)==2)return -1;
    if(!(grant.devices&SS_DEVICE_USB_STORAGE))return atomic_load_explicit(&usb_ready,memory_order_acquire)?1:0;
    if(a733_smp_counter()/24>=atomic_load_explicit(&observed_until,memory_order_acquire))return 0;
    struct a733_typec_token fresh;
    int r=a733_typec_snapshot(&board.typec.provider,&fresh,board.session.deadline_us-10000);
    if(r!=1)return r;
    return fresh.provider==attachment.provider&&fresh.owner==attachment.owner&&fresh.generation==attachment.generation&&
        fresh.polarity==attachment.polarity?1:-1;
}
int os_application_task_privileged(void (*entry)(void *),void *context)
{(void)entry;(void)context;return 0;}
int os_application_pins(struct a733_pin_handoff *pins)
{
    const struct ss_manifest *m=ss_application.manifest;
    if(!pins||!os_session_admitted())return 0;
    if(!m)return 1;
    for(unsigned i=0;i<m->region_count;++i){
        const struct ss_region *r=&m->regions[i];
        struct a733_mapping_rule rule={r->access&SS_ACCESS_WRITE?A733_MAPPING_READ_WRITE:A733_MAPPING_READ,
            1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},1,1};
        if(!a733_mapping_check_pages(aarch64_device_translate,r->address,r->address,r->bytes,&rule))return 0;
        if((r->access&SS_ACCESS_GPU_WRITE)&&!(r->access&SS_ACCESS_WRITE)){
            /* The GPU output must also refuse privileged writes, which is
             * stronger than the EL0 read-only contract. */
            for(uint64_t off=0;off<r->bytes;off+=4096){struct aarch64_translation t;
                if(aarch64_device_translate(r->address+off,1,&t))return 0;}
        }
    }
    return 1;
}
static void main_entry(void *unused)
{
    (void)unused;
    int live;
    while(!(live=ss_live())){ss_progress();vTaskDelay(1);}
    if(live<0)ss_fail(0x750);
    const struct ss_manifest *m=ss_application.manifest;
    unsigned input=0,count=0;
    const struct ss_input_fact *facts=os_session_inputs(&count);
    for(unsigned i=0;i<m->region_count;++i)if(m->regions[i].kind==SS_REGION_INPUT){
        if(input>=count)ss_fail(0x751);
        sunstone_crumb(SUNSTONE_CRUMB_ASSET_START,input);
        struct storage_digest h;storage_digest_init(&h);
        const uint8_t *p=grant.regions[i].base;size_t left=grant.regions[i].bytes;
        while(left){size_t n=left>4096?4096:left;
            if(ss_live()!=1||!storage_digest_add(&h,p,n))ss_fail(0x752);
            p+=n;left-=n;ss_progress();}
        uint8_t hash[32];storage_digest_end(&h,hash);
        sunstone_crumb(SUNSTONE_CRUMB_ASSET_DONE,input);
        if(memcmp(hash,facts[input++].sha256,32))ss_fail(0x753);
    }
    uint64_t now=a733_smp_counter()/24;
    if(m->run_deadline_us){
        if(now>UINT64_MAX-m->run_deadline_us)ss_fail(0x754);
        uint64_t limit=now+m->run_deadline_us;
        if(!grant.finish_by_us||limit<grant.finish_by_us)grant.finish_by_us=limit;
    }
    atomic_store_explicit(&finish_by,grant.finish_by_us,memory_order_release);
    ss_application.main(&grant);ss_fail(0x755);
}
int os_application_start(void)
{return !ss_application.manifest||atomic_load_explicit(&prepared,memory_order_acquire);}
int os_application_deadline(void)
{
    uint64_t limit=atomic_load_explicit(&finish_by,memory_order_acquire);
    return !limit||os_application_finished()||a733_smp_counter()/24<limit;
}
int os_application_prepare(void)
{
    const struct os_session *s=os_session_admitted();const struct ss_manifest *m=ss_application.manifest;
    if(m&&(m->priority>=configMAX_PRIORITIES||m->workers>=configNUMBER_OF_CORES||
           m->stack_bytes<sizeof(StackType_t)||m->stack_bytes%sizeof(StackType_t)))return 0;
    if(!s||atomic_load_explicit(&prepared,memory_order_acquire))return 0;
    if(!m){
#ifdef SUNSTONE_SMP_NEWLIB
        void runtime_smp_init(void);runtime_smp_init();
#endif
#ifdef SUNSTONE_NEWLIB
        runtime_init();
#endif
        return 1;
    }
    grant.devices=os_session_devices();grant.region_count=m->region_count;
#ifdef A733_A76_ADMITTED
    grant.main_core=m->cluster==SS_CLUSTER_A76?6:2;
#else
    grant.main_core=2;
#endif
    unsigned workers=m->workers;
    unsigned pair=grant.main_core+1;
    if(workers){grant.worker_cores=1u<<pair;--workers;}
    for(unsigned core=0;workers&&core<8;++core)
        if(core!=grant.main_core&&core!=pair){grant.worker_cores|=1u<<core;--workers;}
    unsigned count=0,input=0;const struct ss_input_fact *facts=os_session_inputs(&count);
    for(unsigned i=0;i<m->region_count;++i){grant.regions[i].base=(void *)(uintptr_t)m->regions[i].address;
        grant.regions[i].bytes=m->regions[i].kind==SS_REGION_INPUT?facts[input++].bytes:m->regions[i].bytes;}
#if defined(SUNSTONE_A733_OS_AUDIO)
    if(grant.devices&SS_DEVICE_AUDIO_I2S)grant.capture_frames=s->audio.capture_samples;
#endif
#ifdef SUNSTONE_A733_OS_HEADSET
    if(grant.devices&SS_DEVICE_AUDIO_HEADSET){
        const struct bluetooth_profile *p=os_headset_profile();
        if(!p||p->headset.samples>UINT32_MAX/2)return 0;
        grant.capture_frames=2*p->headset.samples;
        if(grant.capture_frames>m->audio.max_frames)return 0;
    }
#endif
#ifndef SUNSTONE_BOARD_RESIDENT
    /* board.session holds the contract-derived absolute deadline. The
     * sealed session returned by os_session_admitted retains zero times. */
    if(board.session.deadline_us<=m->return_reserve_us)return 0;
    grant.finish_by_us=board.session.deadline_us-m->return_reserve_us;
#endif
    atomic_store_explicit(&prepared,1,memory_order_release);
    main_task=xTaskCreateStaticAffinitySet(main_entry,m->name,m->stack_bytes/sizeof(StackType_t),NULL,m->priority,
        ss_application.stack,ss_application.task,(UBaseType_t)1<<grant.main_core);
    if(!main_task)return 0;
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
    os_recognizer=main_task;
#endif
#ifdef SUNSTONE_SMP_NEWLIB
    void runtime_smp_init(void);runtime_smp_init();
#endif
#ifdef SUNSTONE_NEWLIB
    runtime_init();
#endif
    if(ss_application.init)ss_application.init();
    return 1;
}
/* The first-failure record names the detail and the caller of ss_fail. */
__attribute__((noinline)) _Noreturn void ss_fail(uint32_t detail)
{
    board_fail_at(SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_APPLICATION<<12|(detail&0xfff),detail,
        __builtin_return_address(0));
    for(;;)vTaskSuspend(NULL);
}
_Noreturn void ss_finish(const void *data,size_t bytes)
{
    if((!data&&bytes)||bytes>ss_result_capacity||os_application_finished()||
       (grant.finish_by_us&&a733_smp_counter()/24>=grant.finish_by_us))ss_fail(0x756);
#ifdef SUNSTONE_A733_OS_CPU_RETURN
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
    if(!os_gpu_retire())ss_fail(0x757);
#endif
#endif
#ifdef SUNSTONE_A733_OS_USB_CLASSES
    os_usb_service_retire();
    while(!os_usb_service_idle()){
        if(ss_live()<0)ss_fail(0x758);
        ss_progress();vTaskDelay(1);
    }
#endif
    struct ss_result *r=(void *)ss_result_record;
    r->magic=SS_RESULT_MAGIC;r->version=SS_RESULT_VERSION;r->bytes=sizeof(*r)+ss_result_capacity;
    r->epoch=board.session.epoch;r->generation=board.session.generation;
    r->payload_bytes=bytes;if(bytes)memcpy(r+1,data,bytes);
    r->status=SS_RESULT_FINISHED;
    atomic_store_explicit(&finished,1,memory_order_release);
#ifdef SUNSTONE_A733_OS_CPU_RETURN
    os_cpu_return_begin(data,bytes);
#endif
    for(;;)vTaskSuspend(NULL);
}
/* Mailbox states: empty -> offered -> received -> reply -> delivered -> empty.
 * The USB owner serializes its next offer after delivery. */
static struct { atomic_uint state;_Atomic(TaskHandle_t) waiter;struct ss_host_request request;
    struct ss_host_reply reply;uint8_t payload[SS_HOST_PAYLOAD_BYTES]; } mailbox;
int os_host_offer(const struct ss_host_request *request)
{
    const struct ss_manifest *m=ss_application.manifest;
    if(!m||!request||request->op<m->host_op_first||request->op-m->host_op_first>=m->host_op_count||
       atomic_load_explicit(&mailbox.state,memory_order_seq_cst))return 0;
    mailbox.request=*request;atomic_store_explicit(&mailbox.state,1,memory_order_seq_cst);
    TaskHandle_t waiter=atomic_load_explicit(&mailbox.waiter,memory_order_seq_cst);
    if(waiter)xTaskNotifyGive(waiter);return 1;
}
int ss_host_recv(struct ss_host_request *request,uint32_t wait_us)
{
    if(!request||!board_live(NULL))return -1;
    /* Publish waiter before checking the request to avoid a lost wake-up. */
    atomic_store_explicit(&mailbox.waiter,xTaskGetCurrentTaskHandle(),memory_order_seq_cst);
    unsigned state=atomic_load_explicit(&mailbox.state,memory_order_seq_cst);
    if(state==0&&wait_us){
        TickType_t ticks=pdMS_TO_TICKS(((uint64_t)wait_us+999)/1000);
        (void)ulTaskNotifyTake(pdTRUE,ticks?ticks:1);
        state=atomic_load_explicit(&mailbox.state,memory_order_seq_cst);
    }
    if(!board_live(NULL))return -1;
    if(state!=1)return 0;
    *request=mailbox.request;atomic_store_explicit(&mailbox.state,2,memory_order_release);return 1;
}
int ss_host_send(const struct ss_host_reply *reply)
{
    if(!reply||reply->bytes>SS_HOST_PAYLOAD_BYTES||(!reply->data&&reply->bytes)||
       atomic_load_explicit(&mailbox.state,memory_order_seq_cst)!=2)return -1;
    mailbox.reply=*reply;if(reply->bytes)memcpy(mailbox.payload,reply->data,reply->bytes);
    mailbox.reply.data=mailbox.payload;atomic_store_explicit(&mailbox.state,3,memory_order_release);return 1;
}
int os_host_reply(struct ss_host_reply *reply,uint8_t payload[SS_HOST_PAYLOAD_BYTES])
{
    if(atomic_load_explicit(&mailbox.state,memory_order_seq_cst)!=3)return 0;
    *reply=mailbox.reply;if(reply->bytes)memcpy(payload,mailbox.payload,reply->bytes);return 1;
}
void os_host_delivered(void){atomic_store_explicit(&mailbox.state,0,memory_order_release);}
