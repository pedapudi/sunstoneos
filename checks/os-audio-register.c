/* Execute the real stream_register caller with the native OS audio backend.
 * A two-tick mutex delay is shorter than the declared 10 ms operation. */
#include "../apps/os/audio.c"
struct board_application board;
struct board_provider providers;
volatile unsigned os_check_result;
volatile uint64_t ullPortInterruptNestings[8],ullCriticalNestings[8];
static uint64_t ticks,limit,observed_deadline;
static unsigned locked,accesses;
int board_live(void *v){(void)v;return 1;}
void board_fail(void *v){(void)v;__builtin_trap();}
int os_cpu_return_requested(void){return limit!=0;}
uint64_t os_cpu_return_deadline_us(void){return limit;}
int os_cpu_return_failing(void){return limit!=0;}
int os_quiesce_requested(const struct os_quiesce *q){(void)q;return 0;}
enum os_quiesce_result os_quiesce_result(const struct os_quiesce *q){(void)q;return OS_QUIESCE_PENDING;}
int os_quiesce_fault(struct os_quiesce *q,uint32_t s,uint32_t v,const void *p)
{(void)q;(void)s;(void)v;(void)p;return 1;}
BaseType_t xTaskGenericNotify(TaskHandle_t t,UBaseType_t i,uint32_t v,eNotifyAction a,uint32_t *p)
{(void)t;(void)i;(void)v;(void)a;(void)p;return pdPASS;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (TaskHandle_t)(uintptr_t)2;}
BaseType_t xTaskGetSchedulerState(void){return taskSCHEDULER_RUNNING;}
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t q){(void)q;return NULL;}
int dma_service_retired(const struct dma_service *s){(void)s;return 0;}
uint64_t a733_smp_counter(void){return ticks;}
int a733_counter_clock_now(void *v,uint64_t *out){(void)v;*out=ticks/24;return 1;}
int a733_platform_lock_enter(struct a733_platform_locks *p,enum a733_platform_lock which,uint64_t d)
{
    (void)p;(void)which;observed_deadline=d;
    ticks+=2u*24000000u/configTICK_RATE_HZ;
    if(ticks/24>=d)return 0; /* A late acquired mutex is already released. */
    locked=1;return 1;
}
static void unlock(void *v){(void)v;if(!locked)__builtin_trap();locked=0;}
static int register_read(void *v,uintptr_t a,uint32_t *out)
{(void)v;(void)a;if(!locked)__builtin_trap();++accesses;*out=0xcafe;return 1;}
void os_check_run(unsigned mode,unsigned unused)
{
    (void)unused;__asm__ volatile("msr daifclr, #2");
    static struct os_audio_state state;
    audio=&state;audio->handle=(TaskHandle_t)(uintptr_t)1;
    audio->pool.service.worker=(TaskHandle_t)(uintptr_t)2;
    atomic_store(&audio->ready,1);providers.shared.locks.initialized=1;
    audio->parent.initialized=1;audio->parent.stage=7; /* P_READY after real parent construction. */
    audio->parent.cookie=audio;
    audio->parent.ops=(struct a733_audio_operations){.enter=enter,.leave=unlock,.now_us=now,.read=register_read};
    audio->binding=(struct a733_i2s_platform){.parent=&audio->parent,.initialized=1,
        .stage=6,.stream_mode=1,.stream_deadline=10000}; /* L_READY after local OPEN. */
    if(mode)limit=1500;
    struct a733_i2s_stream_operations ops=a733_i2s_platform_stream_operations();
    uint32_t value=0;
    int result=ops.io.read(&audio->binding,A733_I2S0_BASE,&value);
    if(!mode)os_check_result=result==1&&value==0xcafe&&accesses==1&&!locked&&
        !audio->binding.failed&&observed_deadline==10000?1:220;
    else os_check_result=result==0&&!accesses&&!locked&&audio->binding.failed&&observed_deadline==limit?1:221;
    __asm__ volatile("brk #0");
}
