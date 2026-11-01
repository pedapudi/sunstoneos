/* The same bounded CPU workload through the existing returning OS lifetime.
 * Native DMA is absent until the OS admits and drains a real DMA0 service. */
#include "sunstone_app.h"
#include "FreeRTOS.h"
#include "task.h"
#include "workload.h"
#ifdef SUNSTONE_LATENCY_DMA
#include "sunstone_dma.h"
#define latency_init latency_dma_init
#define latency_main latency_dma_main
enum { DMA_OPEN, DMA_FILL, DMA_SUBMIT, DMA_WAIT, DMA_REAP, DMA_CLOSE, DMA_RETIRE, DMA_DONE };
static struct {
    struct ss_dma_buffers buffers;
    unsigned active,stage,stopping,jobs;
} copy;
static void dma_progress(void)
{
    if(!copy.active||copy.stage==DMA_DONE)return;
    int r=0;
    switch(copy.stage){
    case DMA_OPEN:r=ss_dma_open(&copy.buffers);if(r==1)copy.stage=DMA_FILL;break;
    case DMA_FILL:{
        uint8_t *source=copy.buffers.source;
        for(unsigned i=0;i<copy.buffers.bytes;++i)source[i]=(uint8_t)(i^copy.jobs);
        copy.stage=DMA_SUBMIT;break;}
    case DMA_SUBMIT:r=ss_dma_submit();if(r==1)copy.stage=DMA_WAIT;break;
    case DMA_WAIT:
        r=ss_dma_complete();
        if(r==1){
            const uint8_t *data=copy.buffers.destination;
            for(unsigned i=0;i<copy.buffers.bytes;++i)
                if(data[i]!=(uint8_t)(i^copy.jobs))ss_fail(0xc00|(copy.jobs&255));
            ++copy.jobs;copy.stage=DMA_REAP;
        }
        break;
    case DMA_REAP:r=ss_dma_reap();if(r==1)copy.stage=copy.stopping?DMA_CLOSE:DMA_FILL;break;
    case DMA_CLOSE:r=ss_dma_close();if(r==1)copy.stage=DMA_RETIRE;break;
    case DMA_RETIRE:r=ss_dma_retire();if(r==1)copy.stage=DMA_DONE;break;
    default:ss_fail(0xb10);
    }
    if(r<0)ss_fail(0xb80|(copy.stage<<4)|(copy.jobs&15));
}
void latency_dma_native_start(void)
{copy.active=1;copy.stage=DMA_OPEN;}
unsigned latency_dma_native_stop(void)
{
    copy.stopping=1;
    TickType_t start=xTaskGetTickCount();
    while(copy.stage!=DMA_DONE){
        if((TickType_t)(xTaskGetTickCount()-start)>=100)ss_fail(0xb20|copy.stage);
        latency_workload_progress();vTaskDelay(1);
    }
    if(copy.jobs<2)ss_fail(0xb30);
    return copy.jobs;
}
#endif

static struct latency_result result;
void latency_workload_progress(void)
{
#ifdef SUNSTONE_LATENCY_DMA
    dma_progress();
#endif
    if (ss_live() < 0) ss_fail(0xb01);
    if (ss_throttled()) ss_fail(0xb05);
    ss_progress();
}
void latency_init(void)
{
    const struct ss_grant *grant = ss_grant();
    if (!grant || grant->main_core != 6 || grant->worker_cores != 0xbf) ss_fail(0xb02);
    uint64_t frequency;
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(frequency));
    ss_latency_init(frequency);
    latency_workload_bind(grant->regions[0].base);
    latency_workload_init();
}
static int at_top(void)
{
    char clock[256];
    const char prefix[] = "CLOCK cpu_l_mhz=1794 cpu_b_mhz=2002 cpu_l_mv=1000 cpu_b_mv=1050 ";
    if (ss_clock_info(clock, sizeof(clock)) < (int)(sizeof(prefix) - 1)) return 0;
    for (unsigned i = 0; i < sizeof(prefix) - 1; ++i) if (clock[i] != prefix[i]) return 0;
    return 1;
}
void latency_main(const struct ss_grant *grant)
{
    (void)grant;
    /* The thermal service publishes reduced load until its first fresh sensor
     * set. Wait for that publication before measuring at the top CPU points. */
    while (!ss_clock_ready() || ss_throttled()) {
        if (ss_live() < 0) ss_fail(0xb01);
        ss_progress(); vTaskDelay(1);
    }
    if (!at_top()) ss_fail(0xb03);
    ss_activity_begin();
    latency_workload_run(&result);
    if (!at_top()) ss_fail(0xb03);
    ss_activity_end(1);
    ss_finish(&result, sizeof(result));
}
/* The returning profile has no UART output authority. */
void platform_puts(const char *text)
{ (void)text; ss_fail(0xb04); }
