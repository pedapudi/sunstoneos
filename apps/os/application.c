/* Application execution and the EL0-safe calls. Device ownership stays in the
 * USB, capture, thermal and clock service tasks. */
#include "os.h"
#include "application.h"
#include "os_space.h"
#include "sunstone_notify.h"
#include "../board/application.h"
#include "../board/session.h"
#include "a733_mapping_check.h"
#include "a733_storage_digest.h"
#include "sunstone_crumb.h"
#include "sunstone_deadline.h"
#include "sunstone_wifi_credential.h"
#ifdef SUNSTONE_A733_OS_GPU
#include "gpu-share.h"
#include "pvr_compute_queue.h"
extern const struct ss_gpu_share ss_gpu_share __attribute__((weak));
int os_gpu_queue_quiesce(void);
static struct ss_grant peer_grant;
static TaskHandle_t peer_task;
static StaticTask_t peer_task_state;
static StackType_t peer_stack[OS_GPU_PEER_STACK_BYTES/sizeof(StackType_t)];
static atomic_uint peer_ready,peer_done;
static struct pvr_compute_queue *peer_queue;
int pvr_compute_queue_runtime_program(const struct gpu_queue_program *program)
{return !peer_task || program==ss_gpu_share.program;}
int os_application_gpu_share_bind(struct pvr_compute_queue *q)
{
    if (!peer_task) return 1;
    if (!pvr_compute_queue_client_register(q,peer_task)) return 0;
    peer_queue=q; return 1;
}
#else
int os_application_gpu_share_bind(struct pvr_compute_queue *q){(void)q;return 1;}
#endif
#include "boot.h"
#ifdef SUNSTONE_A733_OS_USB_CLASSES
#include "sunstone_usb_service.h"
#endif
#ifdef SUNSTONE_NEWLIB
#include "runtime.h"
#endif
#include <string.h>
/* The headset library's capture length: absent (null) in an image that does not link it. */
extern int os_headset_capture_frames(uint32_t *) __attribute__((weak));
#include "npu.h"
#include "cpu-return.h"
static struct ss_grant grant;
static atomic_uint prepared,finished,usb_ready,activity_generation;
static atomic_uint_fast64_t observed_until;
static struct a733_typec_token attachment;
static TaskHandle_t main_task;
/* The main task, for the virtual board's tests that give it a notification the OS would not (platform/qemu-os). */
TaskHandle_t os_application_main_task(void){return main_task;}
/* 1 when the caller runs at EL1: a privileged task (the NPU's bootstrap, client and worker among them) or an exception
 * handler. The port's task register, TPIDRRO_EL0, carries the bit; EL0 can read it, and the interrupt and supervisor
 * entries set it. Before the first task, a zero TCB field identifies EL1 startup. The port installs a nonzero TCB
 * before returning to an application task at EL0, where this function returns 0. */
int os_task_at_el1(void)
{
    uint64_t tag=ullPortGetTaskRegister();
    return !(tag&portTASK_REGISTER_TCB_MASK)||(int)((tag>>portTASK_REGISTER_EL1_BIT)&1u);
}
static atomic_uint_fast64_t finish_by;
const struct ss_grant *ss_grant(void)
{
    if (!atomic_load_explicit(&prepared,memory_order_acquire)) return NULL;
#ifdef SUNSTONE_A733_OS_GPU
    if (peer_task && xTaskGetCurrentTaskHandle()==peer_task) return &peer_grant;
#endif
    return &grant;
}
int os_application_finished(void)
{return atomic_load_explicit(&finished,memory_order_acquire);}
int os_application_succeeded(void)
{return os_application_finished()&&((const struct ss_result *)ss_result_record)->status==SS_RESULT_FINISHED;}
void os_application_failed(const struct ss_failure *failure,uint64_t epoch,uint64_t generation)
{
    struct ss_result *r=(void *)ss_result_record;
    if(ss_result_capacity<sizeof(*failure))return;
    r->magic=SS_RESULT_MAGIC;r->version=SS_RESULT_VERSION;r->bytes=sizeof(*r)+ss_result_capacity;
    r->epoch=epoch;r->generation=generation;
    memcpy(r+1,failure,sizeof(*failure));r->payload_bytes=sizeof(*failure);
    atomic_thread_fence(memory_order_release);
    r->status=SS_RESULT_FAILED;
    atomic_store_explicit(&finished,1,memory_order_release);
}
/* The OS's wake-ups of an application task (docs/notifications.md): they use the runtime's index, never the
 * application's index 0, and reach the task only while it sleeps in an OS wait that registered here, so that a
 * deadline wait of the application does not end because of an OS event. Sized by the generated tables: one entry per
 * application task. */
unsigned sunstone_deadline_notify_index(void)
{
    /* An application task, tagged when the application starts, waits on the runtime's index; an OS service task on its own. */
    return xTaskGetApplicationTaskTag(NULL)?SS_NOTIFY_DEADLINE:SS_NOTIFY_OWN;
}
extern const unsigned os_wake_capacity;
extern TaskHandle_t os_wake_task[];
extern unsigned os_wake_gave[];
/* A wake-up of the main task that finds it with no registered wait is kept here, and the main task's next OS wait
 * consumes it right after it registers, so an event between the caller's test and the registration is not lost. */
static atomic_uint main_wake_pending;
static void os_wake(TaskHandle_t task)
{
    if(!task)return;
    unsigned found=0;
    taskENTER_CRITICAL();
    for(unsigned i=0;i<os_wake_capacity;++i)if(os_wake_task[i]==task){found=1;os_wake_gave[i]=1;xTaskNotifyGiveIndexed(task,SS_NOTIFY_DEADLINE);}
    if(!found&&task==main_task)atomic_store_explicit(&main_wake_pending,1,memory_order_release);
    taskEXIT_CRITICAL();
}
/* The OS services that the application's end waits for (the NPU's retirement) wake the main task through here. */
void os_application_wake_main(void){os_wake(main_task);}
/* The same wake-up from an interrupt handler, for a device whose completion interrupt ends an application task's wait
 * (the header I2C class). The kernel critical section is the one os_wake takes; the notification is given from the ISR and
 * the caller yields at the end of its handler when *woken is set. */
void os_application_wake_task_from_isr(TaskHandle_t task,BaseType_t *woken)
{
    if(!task)return;
    unsigned found=0;
    UBaseType_t mask=taskENTER_CRITICAL_FROM_ISR();
    for(unsigned i=0;i<os_wake_capacity;++i)
        if(os_wake_task[i]==task){found=1;os_wake_gave[i]=1;vTaskNotifyGiveIndexedFromISR(task,SS_NOTIFY_DEADLINE,woken);}
    if(!found&&task==main_task)atomic_store_explicit(&main_wake_pending,1,memory_order_release);
    taskEXIT_CRITICAL_FROM_ISR(mask);
}
/* From a task: the wake-up os_wake gives the main task, for any application task. */
void os_application_wake_task(TaskHandle_t task){os_wake(task);}
static unsigned os_wait_begin(TaskHandle_t task)
{
    unsigned slot=os_wake_capacity;
    taskENTER_CRITICAL();
    for(unsigned i=0;i<os_wake_capacity&&slot==os_wake_capacity;++i)if(!os_wake_task[i]){os_wake_task[i]=task;os_wake_gave[i]=0;slot=i;}
    taskEXIT_CRITICAL();
    return slot;
}
static void os_wait_end(unsigned slot)
{
    if(slot>=os_wake_capacity)return;
    taskENTER_CRITICAL();
    os_wake_task[slot]=NULL;
    /* What the OS gave after the wait returned would end the application's next wait on index 0. */
    if(os_wake_gave[slot])(void)ulTaskNotifyTakeIndexed(SS_NOTIFY_DEADLINE,pdTRUE,0);
    os_wake_gave[slot]=0;
    taskEXIT_CRITICAL();
}
void os_application_usb_ready(void)
{atomic_store_explicit(&usb_ready,1,memory_order_release);os_wake(main_task);}
int ss_live(void)
{
    if(!board_live(NULL)||os_application_finished()||atomic_load_explicit(&board.result,memory_order_acquire)==2)return -1;
    if(!ss_grant()||((grant.devices&SS_DEVICE_USB_DEVICE)&&
       !atomic_load_explicit(&usb_ready,memory_order_acquire)))return 0;
    if(!(grant.devices&SS_DEVICE_USB_STORAGE))return atomic_load_explicit(&board.result,memory_order_acquire)==3;
    return a733_smp_counter()/24<atomic_load_explicit(&observed_until,memory_order_acquire);
}
static _Atomic uint32_t progress_site;
__attribute__((noinline)) void ss_progress(void)
{
    atomic_store_explicit(&progress_site,(uint32_t)(uintptr_t)__builtin_return_address(0),memory_order_relaxed);
    board_service_progress(OS_SERVICE_APPLICATION);
}
uint32_t os_application_progress_site(void){return atomic_load_explicit(&progress_site,memory_order_relaxed);}
uint64_t ss_counter_ticks(void)
{
    uint64_t now;__asm__ volatile("isb\n mrs %0,cntpct_el0":"=r"(now)::"memory");return now;
}
void os_application_usb_changed(void)
{os_wake(main_task);}
/* A wake-up reaches only a task that has registered its wait (os_wake). An event that happens after the caller tested its
 * condition and before it registered finds no waiter, and the wait would last to its deadline. unless() tests the
 * condition again once the wait is registered: the event is then either seen by that test or delivered to the waiter. */
static int application_wait_until_unless(uint64_t until,int (*unless)(void))
{
    if(xTaskGetCurrentTaskHandle()==main_task){
        uint64_t limit=board.session.deadline_us;
        uint64_t window=os_services[OS_SERVICE_APPLICATION].window_us;
        uint64_t health=until>UINT64_MAX-window?UINT64_MAX:until+window;
        if(health>limit)health=limit;
        if(!board_service_blocked_until(OS_SERVICE_APPLICATION,health))return SUNSTONE_DEADLINE_ERROR;
    }
    const unsigned slot=os_wait_begin(xTaskGetCurrentTaskHandle());
    const int kept=xTaskGetCurrentTaskHandle()==main_task&&atomic_exchange_explicit(&main_wake_pending,0,memory_order_acq_rel);
    const int result=kept||(unless&&unless())?0:sunstone_deadline_wait_until(until);
    os_wait_end(slot);
    return result;
}
static int application_wait_until(uint64_t until){return application_wait_until_unless(until,NULL);}
/* The OS wait with a condition test after registration (see application_wait_until_unless). */
int os_application_wait_unless(uint64_t until,int (*unless)(void)){return application_wait_until_unless(until,unless);}
static int application_entry_ready(void)
{
#ifdef SUNSTONE_A733_USB_FAILURE_TEST
    /* The empty-port diagnostic waits for the two owner checkpoints before
     * application entry. An attachment is neither expected nor synthesized. The
     * USB0 readiness dependency applies only when that service is granted. */
    if(ss_live()<0)return -1;
    return ss_grant()&&(!(grant.devices&SS_DEVICE_USB_DEVICE)||
        atomic_load_explicit(&usb_ready,memory_order_acquire));
#else
    return ss_live();
#endif
}
static int live_changed(void){return application_entry_ready()!=0;}
void ss_usb_wait(uint32_t wait_us)
{
    uint64_t now=a733_smp_counter()/24;
    if(application_wait_until(now+(wait_us?wait_us:1))==SUNSTONE_DEADLINE_ERROR)ss_fail(0x757);
}
int ss_throttled(void){return os_thermal_throttled();}
void os_wifi_credential_poll(void)
{if((os_application_finished()||!board_live(NULL))&&!os_wifi_credential_wiped())os_wifi_credential_wipe();}
#ifndef SUNSTONE_A733_OS_RADIO_SCAN
void ss_radio_status(struct ss_radio_status *status){*status=(struct ss_radio_status){0};}
#endif
int ss_clock_ready(void)
{
    return os_cpu_clock_ready();
}
void ss_activity_begin(void)
{
    void os_thermal_record_begin(void);
    atomic_fetch_add_explicit(&activity_generation,1,memory_order_relaxed);
    os_thermal_record_begin();
}
void ss_activity_end(int success)
{
    void os_thermal_record_end(int);os_thermal_record_end(success);
}
int ss_clock_info(char *line,size_t bytes)
{
    int os_cpu_clock_line(char *,size_t);return os_cpu_clock_line(line,bytes);
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
        atomic_store_explicit(&board.result,3,memory_order_release);os_wake(main_task);return;
    }
#endif
    enum a733_usb2_device_kind expected=(grant.devices&SS_DEVICE_USB_KEYBOARD)?A733_USB2_DEVICE_DUAL:A733_USB2_DEVICE_MSC;
    if(a733_usb2_device_kind(&board.host)!=expected){board_fail(NULL);return;}
    int live=board.host.core.ops.live?board.host.core.ops.live(board.host.core.context,
        board.host.core.generation,board.host.platform.config.lifetime_deadline):-1;
    if(live<0){atomic_store_explicit(&observed_until,0,memory_order_release);os_wake(main_task);return;}
    if(!live)return;
    attachment=board.host.platform.attachment;
    atomic_store_explicit(&observed_until,attachment.observed_us+A733_TYPEC_FRESH_US,memory_order_release);
    os_wake(main_task);

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
/* The interconnect studies (apps/dram-nsi and apps/dram-qos) access the NSI registers and the Arm counters from
 * their worker tasks, which therefore run at EL1. The symbol is weak so that no other image links the application. */
extern void dram_nsi_worker(void *) __attribute__((weak));
extern void dram_qos_worker(void *) __attribute__((weak));
/* The SD owner task of a write-capable image touches the CCU and the controller, so it runs at EL1 (apps/os/sd_owner.c). */
extern int os_sd_owner_task_privileged(void (*)(void *),void *) __attribute__((weak));
/* The task of the control-interrupt measurement (runtime/control_irq.c, SUNSTONE_CONTROL_TASK_EL1) runs at EL1 to separate the
 * cost of the supervisor calls that an EL0 task makes to mask interrupts. The symbol is weak. */
extern int ulSunstoneControlTaskPrivileged(void (*)(void *),void *) __attribute__((weak));
int os_application_task_privileged(void (*entry)(void *),void *context)
{return (dram_nsi_worker&&entry==dram_nsi_worker)||(dram_qos_worker&&entry==dram_qos_worker)||
        (os_sd_owner_task_privileged&&os_sd_owner_task_privileged(entry,context))||
        (ulSunstoneControlTaskPrivileged&&ulSunstoneControlTaskPrivileged(entry,context));}
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
#ifdef SUNSTONE_A733_OS_GPU
static int peer_within_session(void)
{
    uint64_t limit=atomic_load_explicit(&finish_by,memory_order_acquire);
    if (!limit || board.session.deadline_us<limit) limit=board.session.deadline_us;
    return ss_live()>=0 && limit && a733_smp_counter()/24<limit;
}
static void peer_entry(void *unused)
{
    (void)unused;
    if(!sunstone_deadline_register())ss_fail(0x758);
    while (!atomic_load_explicit(&peer_ready,memory_order_acquire)) {
        if (!peer_within_session()) ss_fail(0x759);
        if(sunstone_deadline_wait_until(board.session.deadline_us)==SUNSTONE_DEADLINE_ERROR)ss_fail(0x758);
    }
    ss_gpu_share.main(&peer_grant);
    /* Returning from the entry does not release a GPU-owned buffer. */
    if (!peer_queue) ss_fail(0x75a);
    while (atomic_load(&peer_queue->peer.client_owed)) {
        if (!peer_within_session()) ss_fail(0x75b);
        struct gpu_queue_context c={peer_queue->generation,1};
        struct gpu_queue_record record;
        int r=gpu_queue_wait(c,&record,1);
        if (r!=GPU_QUEUE_OK && r!=GPU_QUEUE_TIMEOUT && r!=GPU_QUEUE_FAULT && r!=GPU_QUEUE_REFUSED)
            ss_fail(0x75c);
        ss_progress();
    }
    atomic_store_explicit(&peer_done,1,memory_order_release);
    for (;;) vTaskSuspend(NULL);
}
#endif
static void main_entry(void *unused)
{
    (void)unused;
    if(!board_service_deadline_register(OS_SERVICE_APPLICATION))ss_fail(0x756);
    int live;
    while(!(live=application_entry_ready())){
        ss_progress();
        if(application_wait_until_unless(board.session.deadline_us-10000,live_changed)==SUNSTONE_DEADLINE_ERROR)ss_fail(0x757);
        if(a733_smp_counter()/24>=board.session.deadline_us-10000&&application_entry_ready()!=1)ss_fail(0x750);
    }
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
#ifdef SUNSTONE_A733_OS_GPU
    if (peer_task) {
        peer_grant.finish_by_us=grant.finish_by_us;
        atomic_store_explicit(&peer_ready,1,memory_order_release);
        xTaskNotifyGive(peer_task);
    }
#endif
#ifdef SUNSTONE_A733_USB_FAILURE_TEST
    /* Only this diagnostic waits for empty-port service startup. Keep the
     * private owner predicate out of the application's public ABI. */
    extern int os_usb_failure_ready(void);
    for(;;){
        if(ss_live()<0||!os_application_deadline())ss_fail(0x750);
        if(os_usb_failure_ready())break;
        ss_progress();vTaskDelay(1);
    }
#endif
    ss_application.main(&grant);ss_fail(0x755);
}
int os_application_start(void)
{return !ss_application.manifest||atomic_load_explicit(&prepared,memory_order_acquire);}
int os_application_deadline(void)
{
    uint64_t limit=atomic_load_explicit(&finish_by,memory_order_acquire);
    return !limit||os_application_finished()||a733_smp_counter()/24<limit;
}
int os_large_prepare(void);
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
    /* An application on the A76 cores is placed there or the start is refused: the thermal start, which runs after this
     * and before the scheduler, checks the boot point for it (os_cpu_boot_point_needed) and refuses the start when the
     * check fails. It is never placed on core 2. */
    grant.main_core=m->cluster==SS_CLUSTER_A76?6:2;
    /* The EL0 ring's producer runs on the NPU interrupt's core, a Cortex-A55 (os_npu_queue_core), and the grant names that core.
     * An application on the A55 cluster that the declaration puts on the ring is placed there and submits from its main task.
     * One on the A76 cluster keeps its cores and is refused unless the queue's core is one of its worker cores: it creates its
     * producer task there, in its init hook (the task takes the application's address space). */
    grant.npu_core=SS_NPU_CORE_NONE;
    if(os_npu_queued()){
        if(m->cluster!=SS_CLUSTER_A76)grant.main_core=os_npu_queue_core();
        grant.npu_core=os_npu_queue_core();
    }
    if(!os_worker_cores(grant.main_core,m->workers,&grant.worker_cores))return 0;
    if(grant.npu_core!=SS_NPU_CORE_NONE&&!os_npu_core_granted(grant.main_core,grant.worker_cores,grant.npu_core))return 0;
    unsigned count=0,input=0;const struct ss_input_fact *facts=os_session_inputs(&count);
    grant.regions=ss_grant_spans;
    for(unsigned i=0;i<m->region_count;++i){ss_grant_spans[i].base=(void *)(uintptr_t)m->regions[i].address;
        ss_grant_spans[i].bytes=m->regions[i].kind==SS_REGION_INPUT?facts[input++].bytes:m->regions[i].bytes;}
    if(grant.devices&SS_DEVICE_AUDIO_I2S)grant.capture_frames=s->audio.capture_samples;
    if(grant.devices&SS_DEVICE_AUDIO_HEADSET){
        if(!os_headset_capture_frames||!os_headset_capture_frames(&grant.capture_frames))return 0;
        if(grant.capture_frames>m->audio.max_frames)return 0;
    }
    /* Large regions come from the ledger before any task exists; a refusal stops the application. */
    if(!os_large_prepare())return 0;
    /* board.session holds the contract-derived absolute deadline. The
     * sealed session returned by os_session_admitted retains zero times. A
     * resident session has no end, so its application has no finish time. */
    if(os_lifetime()!=A733_LIFETIME_RESIDENT){
        if(board.session.deadline_us<=m->return_reserve_us)return 0;
        grant.finish_by_us=board.session.deadline_us-m->return_reserve_us;
    }
#ifdef SUNSTONE_A733_OS_GPU
    if (&ss_gpu_share) {
        if (!os_gpu_share_admit(&ss_gpu_share,m,&grant,&peer_grant)) return 0;
        grant.worker_cores&=~(1u<<peer_grant.main_core);
        peer_task=xTaskCreateStaticAffinitySet(peer_entry,ss_gpu_share.manifest->name,
            ss_gpu_share.manifest->stack_bytes/sizeof(StackType_t),NULL,ss_gpu_share.manifest->priority,
            peer_stack,&peer_task_state,(UBaseType_t)1<<peer_grant.main_core);
        if (!peer_task) return 0;
    }
#endif
    atomic_store_explicit(&prepared,1,memory_order_release);
    main_task=xTaskCreateStaticAffinitySet(main_entry,m->name,m->stack_bytes/sizeof(StackType_t),NULL,m->priority,
        os_large_main_stack()?(StackType_t *)os_large_main_stack():ss_application.stack,ss_application.task,(UBaseType_t)1<<grant.main_core);
    if(!main_task)return 0;
    ss_large_attach(main_task);     /* the main task runs in the application's address space when it has regions */
#ifdef SUNSTONE_SMP_NEWLIB
    void runtime_smp_init(void);runtime_smp_init();
#endif
#ifdef SUNSTONE_NEWLIB
    /* The static constructors run only here, after os_large_prepare attached the ledger's region as the C library's
     * heap: a constructor that allocates takes its memory from that heap (checks/qemu-os-boot.py,
     * heap-constructor-allocates). Calling runtime_init earlier would take the block from the linked heap and refuse the
     * region. */
    runtime_init();
#endif
    if(ss_application.init){
        /* No task creates the tasks of the init hook (the scheduler has not started), so they take the main task's space. */
        os_space_init_tag=(const void *)xTaskGetApplicationTaskTag(main_task);
        ss_application.init();
        os_space_init_tag=NULL;
    }
    return 1;
}
/* The first-failure record names the detail and the caller of ss_fail. */
/* The application fails at EL0: board_fail must not run the credential wipe (dc civac traps at EL0). The radio owner
 * wipes at EL1 once the board is no longer live (os_wifi_credential_poll). */
extern _Atomic unsigned board_fail_from_el0;
#define OS_FAIL_FROM_EL0() atomic_store_explicit(&board_fail_from_el0,1,memory_order_release)
__attribute__((noinline)) _Noreturn void ss_fail_value(uint32_t detail,uint32_t value)
{
    OS_FAIL_FROM_EL0();
    board_fail_at(SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_APPLICATION<<12|
        (detail&UINT32_C(0x7fff0fff)),value,__builtin_return_address(0));
    for(;;)vTaskSuspend(NULL);
}
__attribute__((noinline)) _Noreturn void ss_fail(uint32_t detail)
{
    OS_FAIL_FROM_EL0();
    board_fail_at(SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_APPLICATION<<12|(detail&0xfff),detail,
        __builtin_return_address(0));
    for(;;)vTaskSuspend(NULL);
}
static int npu_retired(void){return os_npu_complete();}
_Noreturn void ss_finish(const void *data,size_t bytes)
{
    if(xTaskGetCurrentTaskHandle()!=main_task || (!data&&bytes)||bytes>ss_result_capacity||os_application_finished()||
       (grant.finish_by_us&&a733_smp_counter()/24>=grant.finish_by_us))ss_fail(0x756);
#ifdef SUNSTONE_A733_OS_GPU
    while(peer_task&&!atomic_load_explicit(&peer_done,memory_order_acquire)){
        if(!peer_within_session())ss_fail(0x75d);
        ss_progress();vTaskDelay(1);
    }
    if(!os_gpu_queue_quiesce())ss_fail(0x75e);
#endif
    /* The returning lifetime retires its devices before the ROM return. */
    const int returning=os_returning();
#ifdef SUNSTONE_A733_OS_GPU
    if(returning&&!os_gpu_retire())ss_fail(0x757);
#endif
#ifdef SUNSTONE_A733_OS_USB_CLASSES
    os_usb_service_retire();
    while(!os_usb_service_idle()){
        if(ss_live()<0)ss_fail(0x758);
        ss_progress();vTaskDelay(1);
    }
#endif
    /* SDK CLOSE transfers no bootstrap/local-stop authority. Wait for its
     * immutable client report and revalidate the live IRQ/runtime state. */
    while(returning&&!os_npu_retirement_check()){
        /* Give up one second before the supervisor's cutoff, so that this failure, which names the unmet conditions,
         * is the first one recorded rather than the supervisor's. The stop takes milliseconds. */
        if(ss_live()<0||a733_smp_counter()+OS_NPU_RETIREMENT_MARGIN_TICKS>=os_cpu_return_cutoff()||
           (grant.finish_by_us&&a733_smp_counter()/24+OS_NPU_RETIREMENT_MARGIN_TICKS/24>=grant.finish_by_us))
            ss_fail_value(0x75f,os_npu_retirement_state());
        ss_progress();
        /* The client's report and the bootstrap's local stop wake the main task (os_application_wake_main); the wait
         * ends one second before the supervisor's cutoff, where the check above names the unmet conditions. */
        uint64_t giveup=os_cpu_return_cutoff()/24-OS_NPU_RETIREMENT_MARGIN_TICKS/24;
        if(grant.finish_by_us&&grant.finish_by_us-OS_NPU_RETIREMENT_MARGIN_TICKS/24<giveup)giveup=grant.finish_by_us-OS_NPU_RETIREMENT_MARGIN_TICKS/24;
        if(application_wait_until_unless(giveup,npu_retired)==SUNSTONE_DEADLINE_ERROR)ss_fail(0x757);
    }
    /* The Wi-Fi credential does not leave RAM here: the wipe's cache maintenance (dc civac) traps at EL0, where
     * this task runs. The radio owner and the return supervisor, both EL1, wipe once os_application_finished()
     * holds (os_wifi_credential_poll); board_fail wipes directly only from EL1. */
    struct ss_result *r=(void *)ss_result_record;
    r->magic=SS_RESULT_MAGIC;r->version=SS_RESULT_VERSION;r->bytes=sizeof(*r)+ss_result_capacity;
    r->epoch=board.session.epoch;r->generation=board.session.generation;
    r->payload_bytes=bytes;if(bytes)memcpy(r+1,data,bytes);
    r->status=SS_RESULT_FINISHED;
    atomic_store_explicit(&finished,1,memory_order_release);

    if(returning)os_cpu_return_begin(data,bytes);
    for(;;)vTaskSuspend(NULL);
}
/* Mailbox states: empty -> offered -> received -> reply -> delivered -> empty.
 * The USB owner serializes its next offer after delivery. */
static struct { atomic_uint state;_Atomic(TaskHandle_t) waiter;struct ss_host_request request;
    struct ss_host_reply reply;uint8_t payload[SS_HOST_PAYLOAD_BYTES]; } mailbox;
static int host_request_pending(void){return atomic_load_explicit(&mailbox.state,memory_order_seq_cst)!=0;}
int os_host_offer(const struct ss_host_request *request)
{
    const struct ss_manifest *m=ss_application.manifest;
    if(!m||!request||request->op<m->host_op_first||request->op-m->host_op_first>=m->host_op_count||
       atomic_load_explicit(&mailbox.state,memory_order_seq_cst))return 0;
    mailbox.request=*request;atomic_store_explicit(&mailbox.state,1,memory_order_seq_cst);
    TaskHandle_t waiter=atomic_load_explicit(&mailbox.waiter,memory_order_seq_cst);
    os_wake(waiter);return 1;
}
int ss_host_recv(struct ss_host_request *request,uint32_t wait_us)
{
    if(!request||!board_live(NULL))return -1;
    /* Publish waiter before checking the request to avoid a lost wake-up. */
    atomic_store_explicit(&mailbox.waiter,xTaskGetCurrentTaskHandle(),memory_order_seq_cst);
    unsigned state=atomic_load_explicit(&mailbox.state,memory_order_seq_cst);
    /* Sleep until a request arrives, the session ends or wait_us has passed; a wake-up for another reason, such as
     * a USB event the OS reports, does not end the call. */
    for(uint64_t until=a733_smp_counter()/24+wait_us;state==0&&wait_us&&a733_smp_counter()/24<until&&board_live(NULL);){
        /* The offer wakes only a registered waiter: test the mailbox again once registered. */
        if(application_wait_until_unless(until,host_request_pending)==SUNSTONE_DEADLINE_ERROR)return -1;
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
