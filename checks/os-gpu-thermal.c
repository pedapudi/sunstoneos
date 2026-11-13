/* Native OS GPU thermal ownership (apps/os/gpu.c) on the real FreeRTOS kernel under the host
 * shim (checks/host/), with modeled service replies and publications. Time and ticks are the
 * kernel's and the shim's; the wrong-client cases call from a second task. The modeled service
 * publishes progress from scheduled interrupts at exact virtual times. The owner's event wait
 * blocks in the kernel on its deadline notification index until a publication or its absolute
 * deadline, so the owner cannot wake without an event. Run as `os-gpu-thermal MODE`; exit
 * status 0 means the mode passed, otherwise it is the failure code. */
#include "a733_smp_placement.h" /* eight-core constants, checked before the host kernel names one core */
#include <stdio.h>
#include <stdlib.h>
#include "host_rtos.h"
#include "sunstone_notify.h"
#ifndef GPU_SOURCE
#define GPU_SOURCE "../apps/os/gpu.c"
#endif
#include GPU_SOURCE

struct board_application board;
struct board_provider providers;
static unsigned mode, throttled, sends, closes, opens, pending, polls, busy;
static uint64_t model_epoch=1;
static volatile unsigned model_phase=PVR_GPU_SERVICE_READY;
/* Service publications and the owner's event waits. A publication advances the sequence,
 * applies its progress and notifies an armed waiter. Each wait ends either by publication
 * (event_wakes) or at exactly its deadline (deadline_wakes). */
static volatile uint64_t model_sequence;
static volatile unsigned event_waiting, expired, wait_id;
static unsigned event_waits, event_wakes, deadline_wakes;
static uint64_t last_until;
static TaskHandle_t other_task;
static void check(int ok,unsigned code)
{if(!ok){fprintf(stderr,"FAIL os-gpu-thermal mode %u code %u\n",mode,code);fflush(stderr);exit((int)code);}}
#define time_us host_time_us()
/* The compute queue's client view is a device-side boundary (drivers/gpu/pvr_compute_queue.c
 * needs AArch64 barriers): the modeled queue has no client with a completion outstanding unless a job is owed. */
int pvr_compute_queue_clients_idle(struct pvr_compute_queue *q){return !q->owed;}
/* Referenced by the compute-queue paths that no thermal case reaches; the sanitizer's global
 * registration keeps the reference alive. */
int board_live(void *unused)
{(void)unused;return !atomic_load(&board.fatal)&&time_us<board.session.deadline_us&&time_us<atomic_load(&board.live_until);}
int os_thermal_throttled(void){return throttled;}
/* The supervised lifetime: the returning cutoff (apps/os/cpu-return.c) is outside this model and its branch is not taken. */
unsigned board_session_lifetime(void){return A733_LIFETIME_SUPERVISED;}
uint64_t os_cpu_return_cutoff(void){return UINT64_MAX;}
/* Link the actual registry, with no return requested in this supervised
 * thermal model. Its retained descriptor also references terminal callbacks;
 * reaching those callbacks is outside the model and fails the case. */
int os_cpu_return_requested(void){return 0;}
int os_cpu_return_failing(void){return 0;}
const struct ss_application ss_application={0};
const struct ss_grant *ss_grant(void){check(0,41);return NULL;}
unsigned pvr_gpu_service_quiesce(struct pvr_gpu_service *s,uint64_t deadline)
{(void)s;(void)deadline;check(0,42);return 3;}
int a733_gpu_backend_stopped(struct a733_gpu_backend *b,struct pvr_gpu_service *s)
{(void)b;(void)s;check(0,43);return 0;}
/* The owner waits only for publications and deadlines; any tick delay is polling. */
void __wrap_vTaskDelay(TickType_t ticks){(void)ticks;check(0,2);}
static void give_owner(void)
{
    BaseType_t woken=pdFALSE;
    vTaskNotifyGiveIndexedFromISR(os_gpu_client_task,SS_NOTIFY_DEADLINE,&woken);
    portYIELD_FROM_ISR(woken);
}
/* A service publication: STOPPING completes to STOPPED. In any other phase the publication
 * is an ownership event that leaves the owner's predicates unchanged. */
static void publish(void *unused)
{
    (void)unused;
    if(model_phase==PVR_GPU_SERVICE_STOPPING)model_phase=PVR_GPU_SERVICE_STOPPED;
    ++model_sequence;
    if(event_waiting)give_owner();
}
static void publish_at(uint64_t us){host_irq_at(us,publish,NULL);}
/* The precise deadline of one wait. A deadline of an earlier wait is ignored. */
static void expire(void *id)
{
    if(!event_waiting||(uintptr_t)id!=wait_id)return;
    expired=1;give_owner();
}
uint64_t pvr_gpu_service_event_sequence(struct pvr_gpu_service *s)
{check(s==&gpu.service,50);return model_sequence;}
unsigned pvr_gpu_service_event_phase(struct pvr_gpu_service *s)
{check(s==&gpu.service,51);return model_phase;}
/* The service is busy throughout retire-close-busy, so CLOSE never becomes ready there. */
int pvr_gpu_service_close_ready(struct pvr_gpu_service *s)
{check(s==&gpu.service,52);return model_phase==PVR_GPU_SERVICE_READY&&!busy&&!gpu.component->owed;}
/* The arm, recheck and disarm order of drivers/gpu/pvr_gpu_service.c. A publication before
 * arming returns at once; otherwise the owner blocks in the kernel. */
int pvr_gpu_service_wait_event(struct pvr_gpu_service *s,uint64_t sequence,uint64_t until)
{
    check(s==&gpu.service&&xTaskGetCurrentTaskHandle()==os_gpu_client_task&&!event_waiting,53);
    check(until>time_us,54);
    ++event_waits;last_until=until;
    taskENTER_CRITICAL();
    event_waiting=1;expired=0;++wait_id;
    int changed=sequence!=model_sequence;
    if(changed)event_waiting=0;
    taskEXIT_CRITICAL();
    if(changed)return SUNSTONE_DEADLINE_NOTIFIED;
    host_irq_at(until,expire,(void *)(uintptr_t)wait_id);
    (void)ulTaskNotifyTakeIndexed(SS_NOTIFY_DEADLINE,pdTRUE,portMAX_DELAY);
    taskENTER_CRITICAL();
    event_waiting=0;
    taskEXIT_CRITICAL();
    (void)ulTaskNotifyTakeIndexed(SS_NOTIFY_DEADLINE,pdTRUE,0);
    if(sequence!=model_sequence){++event_wakes;return SUNSTONE_DEADLINE_NOTIFIED;}
    /* No publication: the only other wake is the deadline itself, at its exact time. */
    check(expired&&time_us==until,55);
    ++deadline_wakes;return SUNSTONE_DEADLINE_EXPIRED;
}
int pvr_gpu_service_send_epoch(struct pvr_gpu_service *s,uint64_t epoch,
    const struct pvr_gpu_service_request *request,TickType_t wait,uint64_t *ticket)
{
    if(mode==12&&pending)return 0;
    check(s==&gpu.service&&epoch==model_epoch&&wait==0&&!pending,3);
    check(board_live(NULL)&&request->timeout_us<=OS_GPU_OPEN_US,4);
    ++sends;pending=request->operation+1;*ticket=sends;
    if(request->operation==PVR_GPU_SERVICE_CLOSE){
        check(model_phase==PVR_GPU_SERVICE_READY&&request->timeout_us,5);
        if(mode>=7)check(gpu.retired&&atomic_load(&gpu.waiting),26);
        ++closes;model_phase=PVR_GPU_SERVICE_STOPPING;
        /* The service publishes STOPPED 2 ms later. It never answers in the deadline modes,
         * never stops in close-shared-status-budget, and publishes once in retire-close-busy. */
        if(mode!=3&&mode!=9&&mode!=14&&(mode!=8||closes==1))publish_at(time_us+2000);
    }else if(request->operation==PVR_GPU_SERVICE_OPEN){
        check(model_phase==PVR_GPU_SERVICE_STOPPED&&request->timeout_us,6);
        ++opens;++model_epoch;model_phase=PVR_GPU_SERVICE_READY;
        if(mode==1)throttled=1;
    }else{
        /* One final STATUS record follows the published STOPPED phase; no STATUS polls. */
        check(request->operation==PVR_GPU_SERVICE_STATUS&&!request->timeout_us&&model_phase==PVR_GPU_SERVICE_STOPPED,7);
        ++polls;
    }
    return 1;
}
int pvr_gpu_service_collect(struct pvr_gpu_service *s,uint64_t ticket,
    struct pvr_gpu_service_reply *reply,TickType_t wait)
{
    check(s==&gpu.service&&ticket==sends&&wait==0&&pending,8);
    if(mode==3||mode==9)return 0;
    *reply=(struct pvr_gpu_service_reply){.ticket=ticket,.epoch=model_epoch,
        .result=PVR_GPU_SERVICE_OK,.phase=model_phase};
    if(mode==8){reply->result=PVR_GPU_SERVICE_BUSY;model_phase=PVR_GPU_SERVICE_READY;}
    if(mode==2||mode==10)atomic_store(&board.fatal,1);
    if(mode==6&&model_phase==PVR_GPU_SERVICE_READY)++reply->epoch;
    if(mode==13&&model_phase==PVR_GPU_SERVICE_STOPPED)++reply->epoch;
    pending=0;return 1;
}
static int result_value;
static void thermal_entry(void *unused){(void)unused;result_value=os_gpu_queue_thermal();}
static void retire_entry(void *unused){(void)unused;result_value=os_gpu_queue_retire();}
/* Wrong-client modes call from a task that is not the GPU client. */
static int caller(void (*entry)(void *))
{
    if(mode==4||mode==11)host_call(other_task,entry,NULL);else entry(NULL);
    return result_value;
}
static void scenario(void *unused)
{
    (void)unused;
    os_gpu_client_task=xTaskGetCurrentTaskHandle();
    gpu.started=gpu.opened=1;gpu.epoch=1;gpu.component=&gpu.session.component;
    board.session.deadline_us=60000000;atomic_store(&board.live_until,3000000);
    if(mode==0){
        for(unsigned i=0;i<2;++i){
            throttled=1;check(caller(thermal_entry)==0&&gpu.parked&&!gpu.opened&&!atomic_load(&gpu.waiting),11);
            throttled=0;check(caller(thermal_entry)==1&&!gpu.parked&&gpu.opened&&!atomic_load(&gpu.waiting),12);
        }
        check(opens==2&&closes==2&&gpu.epoch==3,13);
        /* Each close waits once for the published STOPPED phase, then reads one record. */
        check(polls==2&&event_waits==2&&event_wakes==2&&!deadline_wakes,18);
    }else if(mode==1||mode==2||mode==6){
        gpu.opened=0;gpu.parked=1;model_phase=PVR_GPU_SERVICE_STOPPED;
        int result=caller(thermal_entry);check(opens==1,14);
        if(mode==1)check(result==0&&closes==1&&gpu.parked&&!atomic_load(&gpu.waiting)&&
                         event_waits==1&&event_wakes==1&&polls==1,15);
        else{
            check(result==-1&&!closes&&gpu.failed&&atomic_load(&gpu.waiting)&&!event_waits,16);
            unsigned before=sends;atomic_store(&board.fatal,0);throttled=0;
            check(caller(thermal_entry)==-1&&sends==before,17);
        }
    }else if(mode==3){
        throttled=1;check(caller(thermal_entry)==-1&&sends==1&&closes==1&&gpu.failed,19);
        /* The missing reply holds one wait to the window's exact end. */
        check(time_us==1000100&&atomic_load(&gpu.waiting)&&last_until==1000100&&
              event_waits==1&&deadline_wakes==1&&!event_wakes,20);
    }else if(mode==4){check(caller(thermal_entry)==-1&&!sends,21);}
    else if(mode==5){
        atomic_store(&board.live_until,time_us);check(caller(thermal_entry)==-1&&!sends,22);
        atomic_store(&board.live_until,3000000);check(caller(thermal_entry)==-1&&!sends&&gpu.failed,25);
    }else if(mode>=7&&mode<=14){
        if(mode==12)pending=PVR_GPU_SERVICE_STATUS+1;
        if(mode==8)busy=1;
        int retired=caller(retire_entry);unsigned before=sends;
        if(mode==11)check(!retired&&!gpu.retired&&!gpu.failed&&!sends&&!atomic_load(&gpu.waiting),27);
        else if(mode==7){
            check(retired&&gpu.retired&&gpu.parked&&!gpu.opened&&!gpu.failed&&!atomic_load(&gpu.waiting),28);
            check(closes==1&&!opens&&!pending&&model_phase==PVR_GPU_SERVICE_STOPPED,29);
            check(polls==1&&event_waits==1&&event_wakes==1&&!deadline_wakes,32);
            throttled=1;check(caller(thermal_entry)==0,30);
            throttled=0;check(caller(thermal_entry)==0&&caller(retire_entry)==1&&sends==before,31);
        }else{
            check(!retired&&gpu.retired&&gpu.failed&&atomic_load(&gpu.waiting)&&!gpu.parked&&gpu.opened,33);
            /* A BUSY close reply is retried only after an ownership event; the one
             * publication gives one retry, and the second wait expires the window. */
            check((mode==8?closes==2:closes==(mode==12?0u:1u))&&!opens,34);
            if(mode==8)check(time_us==1000100&&last_until==1000100&&event_waits==2&&
                             event_wakes==1&&deadline_wakes==1,39);
            if(mode==9)check(time_us==1000100&&pending&&event_waits==1&&deadline_wakes==1,35);
            if(mode==12)check(pending==PVR_GPU_SERVICE_STATUS+1&&!sends&&!event_waits,36);
            if(mode==10||mode==13)check(event_waits==(mode==13),44);
            /* CLOSE and its phase wait share one absolute window: with no STOPPED
             * publication, one wait ends at the window's end and no STATUS is sent. */
            if(mode==14)check(xTaskGetTickCount()>=1000&&time_us==1000100&&last_until==1000100&&
                              closes==1&&!polls&&event_waits==1&&deadline_wakes==1&&!event_wakes,37);
            atomic_store(&board.fatal,0);atomic_store(&board.live_until,3000000);
            check(!caller(retire_entry)&&caller(thermal_entry)==-1&&sends==before,38);
        }
    }else if(mode==15){
        /* One unrelated publication arrives while the job remains owed. The owner rechecks
         * its clients after that wake, waits again, and fails at the window's end. */
        gpu.component->owed=1;throttled=1;publish_at(time_us+1000);
        check(caller(thermal_entry)==-1&&gpu.failed&&!sends&&gpu.component->owed,40);
        check(time_us==1000100&&event_waits==2&&event_wakes==1&&deadline_wakes==1,45);
    }else check(0,23);
    }
int main(int argc,char **argv)
{
    check(argc==2,1);
    mode=(unsigned)atoi(argv[1]);
    host_set_time_us(100);
    TaskHandle_t client=host_rtos_prepare(scenario,NULL,2,1000000);(void)client;
    other_task=host_server_create("other",2);
    host_rtos_start();
    return 0;
}

