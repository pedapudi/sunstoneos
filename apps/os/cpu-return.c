/* One finite staged CPU recognition. Service callbacks run on their existing
 * owners. Only the final per-core terminal stops the scheduler context. */
#include "cpu-return.h"
#include "sunstone_wifi_credential.h"
#include "cpu-return-el0.h"
#include "application.h"
#include "os.h"
#include "../board/application.h"
#include "cpu-return-runtime.h"
#include "a733_usb_platform.h"
#include "platform.h"
#include "sunstone_crumb.h"
#include "cached.h"
#include <string.h>
#include "npu.h"
/* The host arms WDT0 in this mode before launch, waits for the session
 * length plus one interval, and reads the breadcrumbs at this address after a
 * watchdog reset (apps/os/cpu-return-host.py). */
__attribute__((used,retain)) const uint64_t os_cpu_return_watchdog[5]={
    UINT64_C(0x31544457534f5243),2,SUNSTONE_CRUMB_WATCHDOG_MODE,SUNSTONE_CRUMB_RTC,A733_BOARD_SUPERVISED_SECONDS
};
static atomic_uint closing,thermal_done,pmic_stop,pmic_done,ready;
/* Failure return (docs/designs/failure-return.md). failing: a failure claimed
 * the return, so the same stop sequence runs as for success and the first
 * failure leaves with it. prefail: that failure came before the scheduler
 * started, when no service task has run and nothing needs stopping. */
static atomic_uint failing,prefail;
static atomic_uint_fast64_t failing_since_us;
static StaticTask_t terminal_state[8];
static StackType_t terminal_stack[8][1024];
static TaskHandle_t terminal_handle[8];
/* The site is the detail; the caller symbolizes the failing check. */
__attribute__((noinline)) static _Noreturn void fail(unsigned detail)
{board_fail_at(SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_CPU_RETURN<<12|(detail&0xfff),0,__builtin_return_address(0));platform_exit(detail);}
/* The cutoff failure says what the session was waiting on: the value is the application's latest progress site (the
 * return address of its last ss_progress call) and the auxiliary field of the site holds the unmet NPU retirement
 * conditions (os_npu_retirement_state, 15 bits; zero for a session that does not grant the NPU). */
__attribute__((noinline)) static _Noreturn void fail_cutoff(void)
{
    unsigned npu=os_granted(OS_GRANT_NPU)?os_npu_retirement_state()&0x7fffu:0;
    board_fail_at(SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_CPU_RETURN<<12|0x73c|(uint32_t)npu<<16,
        os_application_progress_site(),__builtin_return_address(0));
    platform_exit(0x73c);
}
static uint64_t completion_cutoff(void)
{
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
    return (board.session.deadline_us-(OS_CPU_RETURN_RESERVE_US-2000000))*24;
#else
    return os_cpu_return_cutoff();
#endif
}
/* A refusal records why: board_live_refusal() bits, bit 31 for the last
 * 10 ms before the deadline, and the deadline in seconds in the site. */
__attribute__((noinline)) static void bounded(void)
{
    const int late=a733_smp_counter()/24>=board.session.deadline_us-10000;
    if(board_live(NULL)&&!late)return;
    board_fail_at(SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_CPU_RETURN<<12|0x736|(uint32_t)((board.session.deadline_us/1000000)&0x7fff)<<16,
        board_live_refusal()|(late?1u<<31:0),__builtin_return_address(0));
    platform_exit(0x736);
}
uint64_t os_cpu_return_cutoff(void)
{return (board.session.deadline_us-OS_CPU_RETURN_RESERVE_US)*24;}
int os_cpu_return_requested(void){return atomic_load_explicit(&closing,memory_order_acquire);}
/* Task-context gates retain the original NPU fault. The EL3 terminal calls
 * ready() after selecting its Secure Device stack; its health read stays
 * side-effect free because breadcrumb SVC is not available at EL3. */
int os_cpu_return_devices_ready(void)
{return os_npu_retirement_check();}
int os_cpu_return_ready(void)
{
    if(!atomic_load_explicit(&ready,memory_order_acquire))return 0;
    /* Before the scheduler no service task has run, so no device was opened. */
    if(atomic_load_explicit(&prefail,memory_order_acquire))return 1;
    if(os_granted(OS_GRANT_NPU)&&!os_npu_complete())return 0;
    return 1;
}
static void terminal(void *argument)
{
    unsigned core=(unsigned)(uintptr_t)argument;
    if(ulTaskNotifyTake(pdTRUE,portMAX_DELAY)!=1||portGET_CORE_ID()!=core||!os_cpu_return_devices_ready()||!os_cpu_return_ready())fail(0x737);
    sunstone_crumb(SUNSTONE_CRUMB_TERMINAL,core);
    a733_qualification_worker_done(core);
}
int os_cpu_return_task_privileged(void (*entry)(void *),void *argument)
{(void)argument;return entry==terminal;}
int os_cpu_return_start(void)
{
#ifdef SUNSTONE_A733_OS_RADIO_SCAN
    if(board.session.workload!=3||board.session.deadline_us<=OS_CPU_RETURN_RESERVE_US+10000)return 0;
#else
    if(board.session.workload!=2||board.session.deadline_us<=OS_CPU_RETURN_RESERVE_US+10000)return 0;
#endif
    for(unsigned c=0;c<8;++c){
        terminal_handle[c]=xTaskCreateStaticAffinitySet(terminal,"os-cpu-return",1024,(void *)(uintptr_t)c,
            configMAX_PRIORITIES-1,terminal_stack[c],&terminal_state[c],(UBaseType_t)1<<c);
        if(!terminal_handle[c])return 0;
    }
    return 1;
}
/* The recognizer is the sole producer. Its parallel adapter has already
 * collected every job and every worker's explicit terminal acknowledgment. */
void os_cpu_return_begin(const uint8_t *text,size_t bytes)
{
    if((!text&&bytes)||bytes>ss_result_capacity||os_cpu_return_requested()||
       a733_smp_counter()>=completion_cutoff())fail(0x738);
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
    if(!os_gpu_return_ready())fail(0x740);
#endif
    if(!os_cpu_return_devices_ready())fail(0x741);
    const struct ss_result *result=(const void *)ss_result_record;
    if(result->magic!=SS_RESULT_MAGIC||result->status!=SS_RESULT_FINISHED||result->payload_bytes!=bytes)fail(0x738);
    sunstone_crumb(SUNSTONE_CRUMB_OUTPUT,(uint32_t)bytes);
    unsigned none=0;
    if(!atomic_compare_exchange_strong_explicit(&closing,&none,1,memory_order_acq_rel,memory_order_acquire))fail(0x738);
}
int os_cpu_return_failing(void){return atomic_load_explicit(&failing,memory_order_acquire);}
int os_cpu_return_prescheduler_requested(void){return atomic_load_explicit(&prefail,memory_order_acquire);}
/* Interrupts and priorities open on this core: the scheduler can still run a
 * task here, and a pinned terminal can take the core. */
static int core_can_schedule(void)
{
    uint64_t daif,mask;
    __asm__ volatile("mrs %0,daif\n mrs %1,icc_pmr_el1":"=r"(daif),"=r"(mask)::"memory");
    return !(daif&0x80)&&mask>portMAX_API_PRIORITY_MASK;
}
extern volatile uint8_t ucPrimaryCoreInitDoneFlag;
/* Claim the return for the first failure. The caller has recorded the
 * breadcrumb. Returns 1 when the board returns to the ROM (after the
 * scheduler) or when a claimed return is already running, and does not return
 * to a caller before the scheduler. Returns 0 when the failure keeps its
 * watchdog ending. Device failures never claim a return: their owners retain
 * resources that only the watchdog reset releases. */
static int failure_claim(unsigned context,uint32_t site,uint32_t value,const void *caller)
{
    if(atomic_load_explicit(&failing,memory_order_acquire))return 1;
    /* A retained device failure already closed admission: only the watchdog ends it. */
    if(atomic_load_explicit(&board.fatal,memory_order_acquire))return 0;
    unsigned none=0;
    const int before_scheduler=xTaskGetSchedulerState()==taskSCHEDULER_NOT_STARTED;
    if(before_scheduler){
        /* Core 0 before it releases the other cores: all of them wait in the
         * start sequence and take the terminal without a task. */
        if(portGET_CORE_ID()||ucPrimaryCoreInitDoneFlag||a733_smp_counter()>=completion_cutoff())return 0;
    }else{
        if(a733_smp_counter()>=completion_cutoff())return 0;
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
        if(!os_gpu_return_ready())return 0;
#endif
    }
    if(!atomic_compare_exchange_strong_explicit(&closing,&none,1,memory_order_acq_rel,memory_order_acquire))return 0;
    struct ss_failure record={SS_FAILURE_MAGIC,SS_FAILURE_VERSION,before_scheduler?SS_FAILURE_STARTUP:context,
        site,value,(uint64_t)(uintptr_t)caller,(uint32_t)portGET_CORE_ID(),0,a733_smp_counter()};
    atomic_store_explicit(&failing_since_us,a733_smp_counter()/24,memory_order_release);
    /* An application task calls this at EL0: only EL1 code, which runs before
     * the scheduler, may read the monitor's epoch. */
    uint64_t epoch=board.session.epoch;
    if(!epoch&&before_scheduler)epoch=a733_cached_runtime[0].epoch;
    os_application_failed(&record,epoch,board.session.generation);
    /* The host validates the NPU stop report of every failure return of a session that grants the NPU. Before the
     * scheduler no NPU task can run, so a service that has not run (a refusal
     * in os_npu_start, or a failure before it) publishes that nothing was
     * started. After the scheduler started, the service's own client publishes
     * the report when it stops; a bootstrap task that has not run yet could still
     * power the NPU, so no such report is made here. The session may not be decoded yet, so the report is published
     * whatever it grants; the host reads it only for a session that grants the NPU. */
    if(before_scheduler)os_npu_report_unstarted(epoch,board.session.generation);
    atomic_store_explicit(&failing,1,memory_order_release);
    if(!before_scheduler)return 1;
    atomic_store_explicit(&prefail,1,memory_order_release);
    atomic_store_explicit(&ready,1,memory_order_release);
    sunstone_crumb(SUNSTONE_CRUMB_CLOSE,0);
    a733_qualification_worker_done(0);
}
/* Synchronous exceptions from an application task at EL0 (docs/designs/
 * failure-return.md). The EL1 vector calls os_cpu_return_el0_try
 * (apps/os/cpu-return-el0.S), which admits the exception, abandons the faulting
 * context (its stack pointer and registers may be corrupt, and the kernel stack
 * is not used) and returns the task to EL0 in the function below on a reserved
 * stack. A fault that is not admitted ends at the watchdog. */
/* Runs at EL0 on the faulting task, in place of the faulting instruction: the
 * same claim as ss_fail, with the ESR class as the detail, the low ESR syndrome
 * bits as the site's auxiliary field, the faulting address as the value and
 * ELR_EL1 as the caller. The task never runs application code again. The
 * address is meaningful only for instruction aborts, data aborts and PC
 * alignment faults. */
__attribute__((noinline)) _Noreturn void os_cpu_return_el0_fault(uint64_t esr,uint64_t far,uint64_t elr)
{
    const uint32_t site=os_cpu_return_el0_site(esr),value=os_cpu_return_el0_value(esr,far);
    board_fail_note(site,value,(const void *)(uintptr_t)elr);
    if(!failure_claim(SS_FAILURE_FAULT,site,value,(const void *)(uintptr_t)elr))board_fail(NULL);
    for(;;)vTaskSuspend(NULL);
}
/* An application failure (ss_fail) takes the failure return in any phase. */
int os_cpu_return_failure(uint32_t site,uint32_t value,const void *caller)
{
    if(((site>>12)&15u)!=SUNSTONE_CRUMB_FAIL_APPLICATION)return 0;
    return failure_claim(SS_FAILURE_RUNNING,site,value,caller);
}
/* platform_panic and the unexpected-interrupt path: a software-detected
 * fault on a core that can still run. A fault with interrupts or priorities
 * masked, in a critical section or in an interrupt, cannot be handed to a
 * terminal task, and keeps its watchdog ending. */
int os_cpu_return_panic(const char *file,unsigned line,const void *caller)
{
    const uint32_t site=SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_BOARD<<12|(line&0xfffu);
    if(atomic_load_explicit(&failing,memory_order_acquire)||atomic_load_explicit(&closing,memory_order_acquire))return 0;
    /* Note the file and the caller before the masked-context exit below, so a panic with the mask raised, which
     * ends with the watchdog, still leaves the source file and return address in the breadcrumbs. */
    board_fail_note(site,(uint32_t)(uintptr_t)file,caller);
    if(xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED&&!core_can_schedule())return 0;
    return failure_claim(SS_FAILURE_FAULT,site,(uint32_t)(uintptr_t)file,caller);
}
/* An interrupt nothing owns, taken by a running core. The caller masked the
 * source. The interrupted task reaches its terminal after this handler. */
int os_cpu_return_unexpected_interrupt(unsigned interrupt,const void *caller)
{
    const uint32_t site=SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_BOARD<<12|(0x800u|(interrupt&0x7ffu));
    if(xTaskGetSchedulerState()==taskSCHEDULER_NOT_STARTED||
       atomic_load_explicit(&failing,memory_order_acquire)||atomic_load_explicit(&closing,memory_order_acquire))return 0;
    board_fail_note(site,interrupt,caller);
    return failure_claim(SS_FAILURE_FAULT,site,interrupt,caller);
}
void os_cpu_return_thermal_done(void)
{sunstone_crumb(SUNSTONE_CRUMB_THERMAL_DONE,0);atomic_store_explicit(&thermal_done,1,memory_order_release);}
/* Called at the top of the sole PMIC owner loop, before another service step. */
void os_cpu_return_pmic(void)
{
    if(!atomic_load_explicit(&pmic_stop,memory_order_acquire))return;
    bounded();
    if(!os_cpu_return_devices_ready())fail(0x741);
    struct a733_native_usb *p=&providers.shared;
    if(p->pmic.held||p->pmic.retained||p->pmic.failed||p->pmic.active!=A733_PMIC_REQUESTS||
       atomic_load_explicit(&p->locks.retained,memory_order_acquire))fail(0x739);
    for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)
        if(p->pmic.slots[i].state||p->pmic.slots[i].borrow.id)fail(0x73a);
    if(xSemaphoreGetMutexHolder(p->locks.mutex[0])||xSemaphoreGetMutexHolder(p->locks.mutex[1]))fail(0x73b);
    sunstone_crumb(SUNSTONE_CRUMB_PMIC_STOP,0);
    atomic_store_explicit(&pmic_done,1,memory_order_release);
    for(;;)vTaskSuspend(NULL);
}
void os_cpu_return_supervise(void)
{
    bounded();
    /* Live and before the deadline: restart WDT0. fail() never returns, and a
     * stalled supervisor stops the restarts, so WDT0 resets one interval later. */
    sunstone_watchdog_restart();
    /* A failure return that the services cannot finish in time ends at the
     * watchdog, as the failure did before this return existed. */
    if(os_cpu_return_failing()&&a733_smp_counter()/24-atomic_load_explicit(&failing_since_us,memory_order_acquire)>=
       OS_FAILURE_RETURN_US)fail(0x742);
    if(!os_cpu_return_requested()){
        uint64_t cutoff=os_cpu_return_cutoff();
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
        /* The recognizer starts this phase before its original cutoff, after
         * every inference worker stopped. Recognition itself gets no extension. */
        if(os_gpu_return_started())cutoff=completion_cutoff();
#endif
        if(a733_smp_counter()>=cutoff)fail_cutoff();
        return;
    }
    if(!os_cpu_return_devices_ready())fail(0x741);
    if(!atomic_load_explicit(&thermal_done,memory_order_acquire))return;
#ifdef SUNSTONE_A733_OS_DMA
    if(!os_dma_return_ready())return;
#endif
    /* A session that grants the radio may hold the Wi-Fi credential: it leaves RAM, at EL1, before the return proceeds.
     * The runner uploads a credential only for an application that declares the radio (tools/board.py). */
    if(os_granted(OS_GRANT_RADIO)){
        os_wifi_credential_poll();
        if(!os_wifi_credential_wiped())return;
    }
#ifdef SUNSTONE_A733_OS_RADIO_SCAN
    /* The module is shut down, with its readbacks, while the PMIC service can still turn BLDO5 off. */
    os_radio_scan_return_request();
    if(!os_radio_scan_return_ready())return;
#endif
    if(!atomic_exchange_explicit(&pmic_stop,1,memory_order_acq_rel))xTaskNotifyGive(board.handles[0]);
    if(!atomic_load_explicit(&pmic_done,memory_order_acquire))return;
    /* This profile's readiness task never calls USB startup. Constructors
     * alone cannot acquire USB ownership or invoke controller callbacks. */
    const struct a733_native_usb *p=&providers.shared;
    if(p->usb_step||atomic_load_explicit(&p->usb_prepared,memory_order_acquire)||
       p->usb.wrote||p->usb.writes_attempted||p->usb.token.id||p->usb_rail.id||p->usb_power.provider)fail(0x73d);
    for(unsigned i=0;i<4;++i)if(p->usb_request[i].id)fail(0x73e);
    atomic_store_explicit(&ready,1,memory_order_release);
    /* The supervisor is pinned to CPU0. Notify it last so its higher-priority
     * terminal cannot preempt publication to the other seven CPUs. */
    if(portGET_CORE_ID()!=0)fail(0x73f);
    sunstone_crumb(SUNSTONE_CRUMB_CLOSE,0);
    for(unsigned c=1;c<8;++c)xTaskNotifyGive(terminal_handle[c]);
    xTaskNotifyGive(terminal_handle[0]);
    for(;;)vTaskSuspend(NULL);
}
