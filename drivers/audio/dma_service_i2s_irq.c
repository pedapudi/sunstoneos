#include "dma_service_i2s_irq.h"
/* The handler masks the SPI and reads no DMA register; the worker's polls
 * acknowledge the events before it rearms (docs/dma-service.md). Both sides
 * change masked only inside the kernel critical section. */
static int rearm(void *cookie)
{
    struct dma_service_i2s_irq *irq=cookie;
    if(!irq->masked)return 1;
    int r=gicv3_spi_rearm(&irq->spi);
    if(r==1)irq->masked=0;
    return r;
}
int dma_service_i2s_irq_bind(struct dma_service_i2s_irq *irq,struct dma_service *service,uint64_t counter_hz)
{
    if(!irq||!service||irq->service||!counter_hz||counter_hz>UINT64_MAX/DMA_SERVICE_I2S_IRQ_FALLBACK)return 0;
    *irq=(struct dma_service_i2s_irq){service,{0x03400000,A733_DMA0_INTID},1};
    const struct dma_service_irq binding={rearm,irq,DMA_SERVICE_I2S_IRQ_FALLBACK,
        counter_hz*DMA_SERVICE_I2S_IRQ_FALLBACK/configTICK_RATE_HZ};
    if(dma_service_bind_irq(service,&binding))return 1;
    irq->service=NULL;return 0;
}
void dma_service_i2s_irq_handler(struct dma_service_i2s_irq *irq)
{
    BaseType_t woken=pdFALSE;
    UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
    int ok=1;
    if(!irq->masked){irq->masked=1;ok=gicv3_spi_mask(&irq->spi);}
    /* Only a bound service's rearm enables the SPI. */
    if(irq->service)dma_service_irq_notify_from_isr(irq->service,ok,&woken);
    taskEXIT_CRITICAL_FROM_ISR(saved);
    portYIELD_FROM_ISR(woken);
}
