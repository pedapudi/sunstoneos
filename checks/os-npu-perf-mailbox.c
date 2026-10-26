/* Actual OS stopped-retune mailbox; scheduler and provider response modeled. */
#include "../apps/os/npu.c"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
struct board_application board;
struct board_provider providers;
static uint64_t model_now;
static unsigned model_live, notified, takes, actions, failures;
static int action_result;
static jmp_buf finished;
int board_live(void *v){assert(!v);return (int)model_live;}
void board_fail(void *v){assert(!v);++failures;model_live=0;}
int a733_counter_clock_now(void *v,uint64_t *out)
{assert(v==&providers.shared.counter);*out=model_now;return 1;}
BaseType_t xTaskGenericNotify(TaskHandle_t task,UBaseType_t index,uint32_t value,eNotifyAction action,uint32_t *previous)
{assert(task==npu.bootstrap && !index && !value && action==eIncrement && !previous);++notified;return pdPASS;}
uint32_t ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t wait)
{assert(!index && clear==pdTRUE && wait==portMAX_DELAY);if(takes++)longjmp(finished,1);return 1;}
void vTaskSuspend(TaskHandle_t task){assert(!task);longjmp(finished,1);}
int a733_npu_startup_retune_until(struct a733_npu_startup *s,unsigned mhz,uint64_t deadline)
{
    assert(s==&npu.startup && mhz==492 && deadline==perf_deadline && model_now<deadline);
    ++actions;
    /* Preempt the consumer while it acts. Pickup must not free the mailbox. */
    const unsigned request=atomic_load(&perf_request);
    assert(request && !os_npu_perf_retune(852,99,deadline+1) && perf_deadline==deadline &&
        atomic_load(&perf_request)==request);
    return action_result;
}
static void reset(void)
{
    npu=(struct os_npu_application){0};
    atomic_store(&perf_request,0);atomic_store(&perf_answer,0);atomic_store(&perf_ready,1);
    atomic_store(&npu.stopped,1);atomic_store(&npu.active,1);atomic_store(&npu.runtime.service.irq_stopped,1);
    npu.started=1;npu.bootstrap=(TaskHandle_t)&npu;npu.end=60000000;
    board.session.deadline_us=UINT64_MAX/24;model_now=70000000;
    model_live=1;notified=takes=actions=failures=0;action_result=1;
}
static void consume(void){if(!setjmp(finished))perf_serve();}
int main(void)
{
    reset();const uint64_t deadline=model_now+50000;
    assert(os_npu_perf_retune(492,1,deadline)==1 && notified==1 && perf_deadline==deadline);
    assert(!os_npu_perf_retune(852,2,deadline+1) && perf_deadline==deadline && notified==1);
    consume();assert(actions==1 && !failures && os_npu_perf_answer(1)==1 && !atomic_load(&perf_request));
    assert(npu.end==60000000 && os_npu_perf_retune(492,2,deadline+1)==1);
    takes=0;consume();assert(actions==2 && os_npu_perf_answer(1)==0 && os_npu_perf_answer(2)==1);
    reset();assert(!os_npu_perf_retune(492,1,model_now) && !notified && !atomic_load(&perf_request));
    assert(os_npu_perf_retune(492,3,model_now+50000));model_now+=50000;consume();
    assert(!actions && !failures && os_npu_perf_answer(3)==2 && !atomic_load(&perf_request));
    reset();assert(os_npu_perf_retune(492,4,model_now+50000));model_live=0;consume();
    assert(!actions && failures==1 && os_npu_perf_answer(4)==3 && atomic_load(&npu.failed));
    reset();action_result=-1;assert(os_npu_perf_retune(492,5,model_now+50000));consume();
    assert(actions==1 && failures==1 && os_npu_perf_answer(5)==3 && atomic_load(&npu.failed));
    puts("PASS actual OS NPU mailbox: late lifetime, queued and executing exclusion, matching answers, next command, expiry and retained failure");
    return 0;
}
