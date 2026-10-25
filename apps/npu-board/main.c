/* The NPU runtime in the supervised board image: hooks into the board's
 * provider composition (apps/board/main.c) and the startup, runtime, client
 * and bootstrap construction (docs/designs/npu-board.md). */
#include "npu-board.h"
#include "snapshot.h"
#include "platform.h"
#include "a733_memory_reservations.h"
#include <string.h>
struct npu_board_application npu_board;

static uint64_t deadline(void){return board.session.deadline_us-10000;}
static void fatal(void *unused){(void)unused;board_fail(NULL);}
/* Independent core-1 supervisor: the thermal task runs on core 0. Its
 * frozen publication cannot leave either admission or draining unbounded. */
static uint64_t supervise(struct npu_board_client *c,uint64_t *now)
{
    if(!board_live(NULL)||!a733_counter_clock_now(&providers.shared.counter,now)||*now>=deadline())return 0;
    const uint64_t word=npu_board_client_watch(c,*now);
    return (word&NPU_BOARD_STOP)&&*now>=word>>3?0:word;
}
static void bootstrap(void *unused)
{
    (void)unused;struct npu_runtime *rt=&npu_board.runtime;struct npu_board_client *c=&npu_board.client;
    unsigned begun=0;
    for(;;) {
        uint64_t now,word=supervise(c,&now);
        if(!word)break;
        if(!begun) {
            if(word&NPU_BOARD_STOP){atomic_store_explicit(&npu_board.stopped,1,memory_order_release);vTaskSuspend(NULL);}
            if(!npu_board_client_claim(c)){(void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(25));continue;}
            npu_board_client_complete(c);
            begun=1;
        }
        enum a733_npu_platform_result r=npu_runtime_startup_poll(rt);
        if(r==A733_NPU_PLATFORM_RETAINED)break;
        if(r==A733_NPU_PLATFORM_READY) {
            /* The worker on core 1 may run as soon as activation returns. */
            enum npu_service_result a=npu_service_activate(&rt->service);
            if(a==NPU_SERVICE_OK) {
                npu_board_client_start(c);
                /* CLOSE proves controller and interrupt quiescence. A
                 * thermal stop during startup must still OPEN then CLOSE:
                 * runtime stop requires every slot CLOSED. No model job is
                 * admitted by this bootstrap handshake. */
                for(;;) {
                    if(!atomic_load_explicit(&c->closed,memory_order_acquire))
                        (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(25));
                    if(!supervise(c,&now))break;
                    if(!atomic_load_explicit(&c->closed,memory_order_acquire))continue;
                    enum a733_npu_platform_result s=npu_runtime_stop_poll(rt);
                    if(s==A733_NPU_PLATFORM_READY){atomic_store_explicit(&npu_board.stopped,1,memory_order_release);vTaskSuspend(NULL);}
                    if(s==A733_NPU_PLATFORM_RETAINED)break;
                    vTaskDelay(1);
                }
                break;
            }
            if(a!=NPU_SERVICE_BUSY)break;
        }
        vTaskDelay(1);
    }
    fatal(NULL);vTaskSuspend(NULL);for(;;){}
}
static void client(void *unused)
{(void)unused;npu_board_client_run(&npu_board.client);fatal(NULL);vTaskSuspend(NULL);for(;;){}}
static unsigned creating; /* Only while board_application_start creates its two tasks. */
/* The bootstrap and the client call a733_counter_clock_now, which masks
 * DAIF, so both start at EL1t. Only their two creations in
 * board_application_start qualify; any other task with these entries starts
 * at EL0. */
int board_application_task_privileged(void (*entry)(void *),void *context)
{return (creating && !context && (entry==bootstrap || entry==client)) ||
    npu_board_thermal_privileged(entry,context) || npu_service_task_privileged(entry,context)==pdTRUE;}
int board_application_pins(struct a733_pin_handoff *pins)
{
    const struct npu_board_session *source=npu_board_admitted_session();
    return pins&&source&&!memcmp(&source->board,&board.admitted_session,sizeof(source->board));
}
/* Before the scheduler, after the board created its tasks and the power
 * owner: the record, the runtime's parent in the extended write-back pool,
 * the startup with its domain-4 row, the bound service, the interrupt and the
 * client. The worker, the bootstrap and the client run on core 1, the core
 * that receives INTID 97; USB0 and the power service stay on core 0. */
int board_application_start(void)
{
    struct npu_board_application *a=&npu_board;
    const struct npu_board_session *source=npu_board_admitted_session();
    if(!source||memcmp(&source->board,&board.admitted_session,sizeof(source->board)))return 0;
    a->session=*source;
    if(!npu_board_session_valid(&a->session)||memcmp(a->session.board.image,a733_smp_build_id,32))return 0;
    struct device_memory_pool *pool=a733_memory_reservations_init();
    const size_t bytes=npu_runtime_requirements(npu_board_models,NPU_BOARD_MODELS,NPU_BOARD_SLOTS);
    if(!pool||!bytes||!device_memory_acquire(pool,a->session.owner,bytes,&a->lease,&a->extent) ||
       a->extent.bytes!=bytes)return 0;
    a->pool=pool;
    struct npu_runtime_config config={.count=NPU_BOARD_MODELS,.slots=NPU_BOARD_SLOTS,.cpu=a->extent.cpu,
        .physical=a->extent.physical,.bytes=a->extent.bytes,.register_cpu=0x03600000,
        .register_physical=0x03600000,.fatal=fatal};
    memcpy(config.models,npu_board_models,sizeof(npu_board_models));
    if(!npu_runtime_prepare(&a->runtime,&config))return 0;
    const struct a733_board_session *s=&a->session.board;
    const struct a733_npu_startup_config startup={{{s->fabric[0],s->fabric[1],s->fabric[2]},
        {s->fabric_hz[0],s->fabric_hz[1],s->fabric_hz[2]}},a->session.owner,deadline(),a->session.pck_client};
    const struct a733_npu_startup_providers p={&providers.shared.counter,&providers.shared.clocks,
        &providers.shared.locks,&providers.shared.gpu,&providers.shared.rails,&providers.domains};
    if(!a733_npu_startup_init(&a->startup,&startup,&p,NULL,NULL))return 0;
    const UBaseType_t core=UINT32_C(1)<<NPU_BOARD_CORE;
    creating=1;
    a->bootstrap=xTaskCreateStaticAffinitySet(bootstrap,"npu-boot",NPU_BOARD_STACK,NULL,4,a->stacks[0],&a->tasks[0],core);
    a->client_task=xTaskCreateStaticAffinitySet(client,"npu-client",NPU_BOARD_STACK,NULL,3,a->stacks[1],&a->tasks[1],core);
    creating=0;
    if(!a->bootstrap||!a->client_task||!a733_npu_startup_bind_task(&a->startup,a->bootstrap))return 0;
    const TaskHandle_t clients[NPU_BOARD_SLOTS]={a->client_task};
    if(!npu_runtime_bind(&a->runtime,&a->startup,clients,4,core)||!npu_board_irq_bind(&a->runtime))return 0;
    a->client=(struct npu_board_client){.rt=&a->runtime,.slot=0,.job_timeout_us=1000000,
        .wait=pdMS_TO_TICKS(2000),.task=a->client_task,.bootstrap=a->bootstrap,
        .now=a733_counter_clock_now,.clock=&providers.shared.counter,.session_end=deadline()};
    uint64_t now;
    if(deadline()>(UINT64_MAX>>3)-5000000||!a733_counter_clock_now(&providers.shared.counter,&now)||now>=deadline())return 0;
    atomic_init(&a->client.admission,(now+1000000)<<3);
    if(!npu_board_thermal_start())return 0;
    /* USB0 stays on core 0 with the power service, apart from the NPU work. */
    if(!board.handles[1])return 0;
    vTaskCoreAffinitySet(board.handles[1],1);
    return 1;
}
