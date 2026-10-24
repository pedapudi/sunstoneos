/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_power_domains_native.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include "a733_task_context.h"
#include "FreeRTOS.h"
#include "task.h"

/* Each window once: the PPU blocks are written, so their translation must be
 * outer-shareable Device-nGnRnE; the CCU and R_CCU pages are only read. */
static int mapped(struct a733_power_domains_native *n)
{
    static const struct a733_mapping_rule write_rule={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_OUTER}},0,1};
    static const struct a733_mapping_rule read_rule={A733_MAPPING_READ,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_ANY}},0,1};
    static const uintptr_t written[]={0x07061000,0x07064000,0x07065000},read[]={0x02002000,0x07010000};
    if(n->mapped)return n->mapped==1;
    n->mapped=2;
    for(unsigned i=0;i<3;++i)if(!a733_mapping_check(aarch64_device_translate,written[i],written[i],&write_rule))return 0;
    for(unsigned i=0;i<2;++i)if(!a733_mapping_check(aarch64_device_translate,read[i],read[i],&read_rule))return 0;
    n->mapped=1;return 1;
}
static int read32(void *v,uintptr_t a,uint32_t *out)
{
    struct a733_power_domains_native *n=v;
    if(!n || !n->initialized || !out || !a733_power_domains_native_address(a,0) || !mapped(n))return 0;
    *out=a733_mmio_read32_dsb_sy_both(a);return 1;
}
static int write32(void *v,uintptr_t a,uint32_t value)
{
    struct a733_power_domains_native *n=v;
    if(!n || !n->initialized || !a733_power_domains_native_address(a,1) ||
       !a733_task_context_owner_el1((TaskHandle_t)n->task) || !mapped(n))return 0;
    a733_mmio_write32_dsb_sy_both(a,value);return 1;
}
/* The service handles its own submissions within the same step, so a wake
 * from the service task itself is dropped. */
static void notify(void *v)
{
    struct a733_power_domains_native *n=v;
    if(n && n->initialized && xTaskGetCurrentTaskHandle()!=(TaskHandle_t)n->task)
        xTaskNotifyGive((TaskHandle_t)n->task);
}
int a733_power_domains_native_init(struct a733_power_domains_native *n,void *task)
{
    if(!n || n->initialized || !task || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    *n=(struct a733_power_domains_native){.operations={read32,write32,notify},.task=task,.initialized=1};
    return 1;
}
