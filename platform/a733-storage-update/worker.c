#define A733_STORAGE_NATIVE_IMPLEMENTATION
#include "a733_storage_update_native.h"
#include "task.h"

static void wait_activation(struct a733_storage_native *n)
{while(!atomic_load_explicit(&n->active,memory_order_acquire))vTaskDelay(1);}
static void stop(struct a733_storage_native *n)
{a733_storage_native_fail(n);for(;;)vTaskDelay(1);}
void a733_storage_native_pmic_task(void *p)
{
    struct a733_storage_native *n=p;wait_activation(n);
    for(;;) {
        if(!a733_storage_native_live(n))stop(n);
        enum a733_pmic_status r=a733_pmic_poll(&n->pmic);
        if(r==A733_PMIC_READY||r==A733_PMIC_COMPLETE)
            atomic_store_explicit(&n->pmic_ready,1,memory_order_release);
        else if(r!=A733_PMIC_PENDING&&r!=A733_PMIC_BUSY)stop(n);
        vTaskDelay(1);
    }
}
void a733_storage_native_worker_task(void *p)
{
    struct a733_storage_native *n=p;wait_activation(n);
    while(!atomic_load_explicit(&n->pmic_ready,memory_order_acquire)) {
        if(!a733_storage_native_live(n))stop(n);vTaskDelay(1);
    }
    for(;;) {
        if(!a733_storage_native_live(n))stop(n);
        enum a733_mmc_platform_result r=a733_mmc_platform_poll(&n->mmc);
        if(r==A733_MMC_READY)break;
        if(r!=A733_MMC_PENDING&&r!=A733_MMC_BUSY)stop(n);vTaskDelay(1);
    }
    if(!a733_storage_update_init(&n->service,&n->mmc,&n->boot.grant,n->boot.mmc.deadline_us)||
       !a733_mmc_platform_native_bind_update(&n->mmc_native,&n->service)||
       !a733_maintenance_storage_observe(n->maintenance,&n->service))stop(n);
    atomic_store_explicit(&n->service_ready,1,memory_order_release);
    for(;;) {
        if(!a733_storage_native_live(n)) {
            a733_storage_update_lost(&n->service);
            a733_storage_channel_worker(&n->channel,&n->service);stop(n);
        }
        a733_storage_channel_worker(&n->channel,&n->service);
        if(!a733_maintenance_storage_observe(n->maintenance,&n->service)||
           atomic_load_explicit(&n->channel.lost,memory_order_acquire))stop(n);
        vTaskDelay(1);
    }
}
