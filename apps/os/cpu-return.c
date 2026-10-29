/* One finite staged CPU recognition. Service callbacks run on their existing
 * owners. Only the final per-core terminal stops the scheduler context. */
#include "cpu-return.h"
#include "os.h"
#include "../board/application.h"
#include "cpu-return-runtime.h"
#include "a733_usb_platform.h"
#include "platform.h"
#include <string.h>
struct os_cpu_return_output {
    uint64_t magic,version,bytes,epoch,generation,text_bytes;
    uint8_t text[16384];
};
__attribute__((used,retain)) struct os_cpu_return_output os_cpu_return_output;
static atomic_uint closing,thermal_done,pmic_stop,pmic_done,ready;
static StaticTask_t terminal_state[8];
static StackType_t terminal_stack[8][1024];
static TaskHandle_t terminal_handle[8];
static _Noreturn void fail(unsigned detail)
{board_fail(NULL);platform_exit(detail);}
static void bounded(void)
{if(!board_live(NULL)||a733_smp_counter()/24>=board.session.deadline_us-10000)fail(0x736);}
uint64_t os_cpu_return_cutoff(void)
{return (board.session.deadline_us-OS_CPU_RETURN_RESERVE_US)*24;}
int os_cpu_return_requested(void){return atomic_load_explicit(&closing,memory_order_acquire);}
int os_cpu_return_ready(void){return atomic_load_explicit(&ready,memory_order_acquire);}
static void terminal(void *argument)
{
    unsigned core=(unsigned)(uintptr_t)argument;
    if(ulTaskNotifyTake(pdTRUE,portMAX_DELAY)!=1||portGET_CORE_ID()!=core||!os_cpu_return_ready())fail(0x737);
    a733_qualification_worker_done(core);
}
int os_cpu_return_task_privileged(void (*entry)(void *),void *argument)
{(void)argument;return entry==terminal;}
int os_cpu_return_start(void)
{
    if(board.session.workload!=2||board.session.deadline_us<=OS_CPU_RETURN_RESERVE_US+10000)return 0;
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
    if(!text||!bytes||bytes>sizeof(os_cpu_return_output.text)||os_cpu_return_requested()||
       a733_smp_counter()>=os_cpu_return_cutoff())fail(0x738);
    os_cpu_return_output.magic=UINT64_C(0x3152434f33333741);
    os_cpu_return_output.version=1;os_cpu_return_output.bytes=sizeof(os_cpu_return_output);
    os_cpu_return_output.epoch=board.session.epoch;os_cpu_return_output.generation=board.session.generation;
    os_cpu_return_output.text_bytes=bytes;memcpy(os_cpu_return_output.text,text,bytes);
    atomic_store_explicit(&closing,1,memory_order_release);
}
void os_cpu_return_thermal_done(void)
{atomic_store_explicit(&thermal_done,1,memory_order_release);}
/* Called at the top of the sole PMIC owner loop, before another service step. */
void os_cpu_return_pmic(void)
{
    if(!atomic_load_explicit(&pmic_stop,memory_order_acquire))return;
    bounded();
    struct a733_native_usb *p=&providers.shared;
    if(p->pmic.held||p->pmic.retained||p->pmic.failed||p->pmic.active!=A733_PMIC_REQUESTS||
       atomic_load_explicit(&p->locks.retained,memory_order_acquire))fail(0x739);
    for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)
        if(p->pmic.slots[i].state||p->pmic.slots[i].borrow.id)fail(0x73a);
    if(xSemaphoreGetMutexHolder(p->locks.mutex[0])||xSemaphoreGetMutexHolder(p->locks.mutex[1]))fail(0x73b);
    atomic_store_explicit(&pmic_done,1,memory_order_release);
    for(;;)vTaskSuspend(NULL);
}
void os_cpu_return_supervise(void)
{
    bounded();
    if(!os_cpu_return_requested()){
        if(a733_smp_counter()>=os_cpu_return_cutoff())fail(0x73c);
        return;
    }
    if(!atomic_load_explicit(&thermal_done,memory_order_acquire))return;
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
    for(unsigned c=1;c<8;++c)xTaskNotifyGive(terminal_handle[c]);
    xTaskNotifyGive(terminal_handle[0]);
    for(;;)vTaskSuspend(NULL);
}
