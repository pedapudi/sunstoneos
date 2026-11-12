/* Included after the production lock implementation and its modeled RTOS.
 * The actual native MMC adapter calls that implementation without a lock stub. */
#define SUNSTONE_A733_PLATFORM_LOCKS_H
#define A733_MMC_NATIVE_HOST_TEST
#include "a733_mmc_platform_native.h"
#include <string.h>
static unsigned loads,stores;
uint32_t a733_native_host_load(uintptr_t address){(void)address;++loads;return 0;}
void a733_native_host_store(uintptr_t address,uint32_t value){(void)address;(void)value;++stores;}
int a733_native_host_current(void *binding){(void)binding;return context_ok;}
int a733_storage_update_mmio(const struct a733_storage_update *u,uintptr_t a,uint32_t v)
{(void)u;(void)a;(void)v;return 0;}
int a733_storage_update_fifo(const struct a733_storage_update *u,uintptr_t a,unsigned c)
{(void)u;(void)a;(void)c;return 0;}
int a733_storage_update_owned(const struct a733_storage_update *u,const struct a733_mmc_platform *p)
{(void)u;(void)p;return 0;}
#include "storage-native-current.c"
static int admit_model(void *c,const struct a733_mmc_platform_config *p,const struct a733_clock_token *t)
{(void)c;(void)p;(void)t;return 1;}
static struct a733_mmc_platform_native binding;
static void native_reset(void)
{
    reset();memset(&binding,0,sizeof binding);loads=stores=0;
    assert(a733_mmc_platform_native_init(&binding,&locks,(void *)1,SUNXI_MMC_SD,admit_model,NULL));
}
int main(void)
{
    uint64_t stamp;uint32_t value;
    native_reset();failures=100;change_limit=1;
    assert(!binding.operations.enter(&binding,60000000));
    assert(takes==3&&clock_us==2100&&!binding.holding&&!holder[0]);
    clock_us=limit_us;
    assert(!binding.operations.now_us(&binding,&stamp));
    assert(!binding.operations.current(&binding));
    native_reset();failures=2;
    assert(binding.operations.enter(&binding,60000000)&&takes==3&&binding.holding);
    assert(binding.operations.now_us(&binding,&stamp)&&stamp==clock_us);
    binding.operations.leave(&binding);
    native_reset();change_limit=late=1;
    assert(!binding.operations.enter(&binding,60000000)&&gives==1&&!binding.holding);
    native_reset();assert(binding.operations.enter(&binding,60000000));
    assert(binding.operations.read(&binding,A733_MMC_BASE(0),&value)&&loads==1);
    limit_us=clock_us;
    assert(!binding.operations.read(&binding,A733_MMC_BASE(0),&value)&&loads==1);
    assert(!binding.operations.write(&binding,A733_MMC_BASE(0)+SUNXI_SMHC_IMASK,0)&&stores==0);
    assert(!binding.operations.now_us(&binding,&stamp));
    binding.operations.leave(&binding);
    puts("PASS native MMC held-lock return, ordinary contention, late acquisition and expired time/MMIO guards");
    return 0;
}
