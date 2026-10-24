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
static void fatal(void *unused){(void)unused;atomic_store_explicit(&npu_board.failed,1,memory_order_release);board_fail(NULL);}
static void bootstrap(void *unused)
{
    (void)unused;struct npu_runtime *rt=&npu_board.runtime;
    for(;;) {
        if(!board_live(NULL) || a733_smp_counter()/24>=deadline())break;
        enum a733_npu_platform_result r=npu_runtime_startup_poll(rt);
        if(r==A733_NPU_PLATFORM_RETAINED)break;
        if(r==A733_NPU_PLATFORM_READY) {
            /* The worker on core 1 may run as soon as activation returns. */
            enum npu_service_result a=npu_service_activate(&rt->service);
            if(a==NPU_SERVICE_OK) {
                atomic_store_explicit(&npu_board.ready,1,memory_order_release);
                if(!npu_board_client_post(&npu_board.client,1,NPU_BOARD_OPEN,0))break;
                /* The session ends with whole-board power removal; the stop
                 * table runs only after every slot's CLOSE. */
                for(;;) {
                    (void)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100));
                    if(!board_live(NULL))break;
                    if(npu_board.client.state==A733_NPU_CLOSED) {
                        enum a733_npu_platform_result s=npu_runtime_stop_poll(rt);
                        if(s==A733_NPU_PLATFORM_READY){atomic_store_explicit(&npu_board.stopped,1,memory_order_release);vTaskSuspend(NULL);}
                        if(s==A733_NPU_PLATFORM_RETAINED)break;
                    }
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
int board_application_task_privileged(void (*entry)(void *),void *context)
{return (!context && entry==bootstrap) || npu_service_task_privileged(entry,context)==pdTRUE;}
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
    a->bootstrap=xTaskCreateStaticAffinitySet(bootstrap,"npu-boot",NPU_BOARD_STACK,NULL,4,a->stacks[0],&a->tasks[0],core);
    a->client_task=xTaskCreateStaticAffinitySet(client,"npu-client",NPU_BOARD_STACK,NULL,3,a->stacks[1],&a->tasks[1],core);
    if(!a->bootstrap||!a->client_task||!a733_npu_startup_bind_task(&a->startup,a->bootstrap))return 0;
    const TaskHandle_t clients[NPU_BOARD_SLOTS]={a->client_task};
    if(!npu_runtime_bind(&a->runtime,&a->startup,clients,4,core)||!npu_board_irq_bind(&a->runtime))return 0;
    a->client=(struct npu_board_client){.rt=&a->runtime,.slot=0,.job_timeout_us=1000000,
        .wait=pdMS_TO_TICKS(2000),.task=a->client_task};
    /* USB0 stays on core 0 with the power service, apart from the NPU work. */
    if(!board.handles[1])return 0;
    vTaskCoreAffinitySet(board.handles[1],1);
    return 1;
}
