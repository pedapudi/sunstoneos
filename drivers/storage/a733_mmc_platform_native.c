#include "a733_mmc_platform_native.h"
#include "a733_storage_update.h"
#include "a733_platform_locks.h"
#include "sunxi_smhc.h"
#include "task.h"
#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1
#error "MMC native binding requires scheduler-state, task and mutex-owner APIs"
#endif
static int current(void *cookie)
{
    struct a733_mmc_platform_native *n=cookie;
    uint64_t el,flags,nesting,critical;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, daif" : "=r"(el),"=r"(flags));
    if (!n || !n->initialized || el!=4 || (flags&0x80)) return 0;
    /* Read the port's task/interrupt state with the same short IRQ exclusion
     * used by the shared lock implementation. Never admit ISR or critical code. */
    __asm__ volatile("msr daifset, #2" ::: "memory");
#if configNUMBER_OF_CORES > 1
    extern volatile uint64_t ullPortInterruptNestings[],ullCriticalNestings[];
    uint64_t core;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(core));
    nesting=core<configNUMBER_OF_CORES ? ullPortInterruptNestings[core] : 1;
    critical=core<configNUMBER_OF_CORES ? ullCriticalNestings[core] : 1;
#else
    extern uint64_t ullPortInterruptNesting;
    extern volatile uint64_t ullCriticalNesting;
    nesting=ullPortInterruptNesting; critical=ullCriticalNesting;
#endif
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
    return !nesting && !critical && xTaskGetSchedulerState()==taskSCHEDULER_RUNNING &&
        xTaskGetCurrentTaskHandle()==(TaskHandle_t)n->owner_task;
}

static int now(void *cookie,uint64_t *v)
{
    struct a733_mmc_platform_native *n=cookie;
    return n->locks->now(n->locks->context,v);
}
static int admit(void *cookie,const struct a733_mmc_platform_config *c,const struct a733_clock_token *t)
{
    struct a733_mmc_platform_native *n=cookie;
    if(c->kind!=n->kind||c->emmc_host!=n->emmc_host||!current(n))return -1;
    int r=n->admit(n->admission_cookie,c,t);
    if(r==1)n->sd_profile=c->sd_profile;
    return r;
}
/* A successful lock entry is cached until leave or retain, so each access
 * needs no kernel critical section to query the mutex holder. */
static int enter(void *cookie,uint64_t deadline)
{
    struct a733_mmc_platform_native *n=cookie;
    n->holding=current(n)&&a733_platform_lock_enter(n->locks,A733_PLATFORM_REGISTER_LOCK,deadline)==1;
    return (int)n->holding;
}
static void leave(void *cookie)
{
    struct a733_mmc_platform_native *n=cookie;
    n->holding=0;a733_platform_lock_leave(n->locks,A733_PLATFORM_REGISTER_LOCK);
}
static void retain(void *cookie)
{
    struct a733_mmc_platform_native *n=cookie;
    n->holding=0;configASSERT(a733_platform_lock_retain(n->locks,A733_PLATFORM_REGISTER_LOCK)==1);
}
static uintptr_t fifo_address(const struct a733_mmc_platform_native *n)
{return A733_MMC_BASE(a733_mmc_slot(n->kind,n->emmc_host))+SUNXI_SMHC_FIFO;}
static int allowed(struct a733_mmc_platform_native *n,uintptr_t a,unsigned writing)
{
    if(!current(n))return 0;
    int fifo=writing&&a==fifo_address(n)&&n->update;
    if(!fifo&&!a733_mmc_platform_native_address_selected(n->kind,n->emmc_host,a,writing))return 0;
    return n->holding&&!(atomic_load_explicit(&n->locks->retained,memory_order_acquire)&1u);
}
/* Device accesses to one peripheral stay ordered; barriers bracket the burst. */
static int load(struct a733_mmc_platform_native *n,uintptr_t a,uint32_t *v,unsigned count)
{
    if(!v||!count||count>SUNXI_MMC_FIFO_WORDS||!allowed(n,a,0))return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    for(unsigned i=0;i<count;++i)v[i]=*(volatile const uint32_t *)a;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
static int store(uintptr_t a,const uint32_t *v,unsigned count)
{
    __asm__ volatile("dsb sy" ::: "memory");
    for(unsigned i=0;i<count;++i)*(volatile uint32_t *)a=v[i];
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
static int read32(void *cookie,uintptr_t a,uint32_t *v)
{return load(cookie,a,v,1);}
static int read_fifo(void *cookie,uintptr_t a,uint32_t *v,unsigned count)
{
    struct a733_mmc_platform_native *n=cookie;
    return n&&a==fifo_address(n)&&load(n,a,v,count);
}
static int write_fifo(void *cookie,uintptr_t a,const uint32_t *v,unsigned count)
{
    struct a733_mmc_platform_native *n=cookie;
    if(!n||!v||!count||count>SUNXI_MMC_FIFO_WORDS||a!=fifo_address(n)||!allowed(n,a,1)||
       !a733_mmc_platform_native_fifo_allowed(n->kind,n->emmc_host,n->update,a,count))return 0;
    return store(a,v,count);
}
static int write32(void *cookie,uintptr_t a,uint32_t v)
{
    struct a733_mmc_platform_native *n=cookie;
    if(!allowed(n,a,1))return 0;
    unsigned slot=a733_mmc_slot(n->kind,n->emmc_host);
    uintptr_t base=A733_MMC_BASE(slot);
    uintptr_t module=A733_MMC_MODULE(slot);
    if(a==base+SUNXI_SMHC_FIFO) {
        if(!a733_mmc_platform_native_transfer_allowed(n->kind,n->emmc_host,n->update,a,v))return 0;
    } else if(a==0x03400198) {
        if(v!=A733_MMC_GIC_BIT(slot))return 0;
    } else if(a==module||a==A733_MMC_BUS(slot)) {
        uint32_t mask=a==module?UINT32_C(0x87001f1f):UINT32_C(0x10001);
        if(a==module){
            uint32_t setting=v&(mask&~UINT32_C(0x80000000));
            if(setting!=29&&(n->kind!=SUNXI_MMC_SD||
               !n->sd_profile||(setting&~31u)!=UINT32_C(0x01000000)||
               (n->sd_profile==SUNXI_MMC_SD_DEFAULT4?(setting&31u)<7:(setting&31u)!=3)))return 0;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        uint32_t old=*(volatile const uint32_t *)a;
        if((old^v)&~mask)return 0;
    } else if(a==base+SUNXI_SMHC_CMD) {
        if(v&SUNXI_SMHC_CMD_WRITE) {
            if(!a733_mmc_platform_native_transfer_allowed(n->kind,n->emmc_host,n->update,a,v))return 0;
        }
        if(!(v&SUNXI_SMHC_CMD_UPDATE_CLOCK))switch(v&SUNXI_SMHC_CMD_INDEX) {
        case 0:case 1:case 2:case 3:case 7:case 8:case 9:case 13:
        case 16:case 17:case 18:case 41:case 55:break;
        case 6:case 51:
            if(n->kind!=SUNXI_MMC_SD||!n->sd_profile)return 0;
            /* New SD commands have fixed read-only protocol forms. No eMMC
             * SWITCH or write-direction command is admitted by this path. */
            {uint32_t argument=*(volatile const uint32_t *)(base+SUNXI_SMHC_ARG);
             uint32_t flags=SUNXI_SMHC_CMD_START|SUNXI_SMHC_CMD_WAIT|SUNXI_SMHC_CMD_CRC|SUNXI_SMHC_CMD_RESP;
             if((v&SUNXI_SMHC_CMD_INDEX)==51){if(v!=(flags|SUNXI_SMHC_CMD_DATA|51u)||argument)return 0;}
             else if(v==(flags|6u)){if(argument!=2)return 0;}
             else if(v==(flags|SUNXI_SMHC_CMD_DATA|6u)){if(n->sd_profile!=SUNXI_MMC_SD_HIGH_SPEED4||(argument!=0x00fffff1&&argument!=0x80fffff1))return 0;}
             else return 0;}
            break;
        case 24:if(!a733_mmc_platform_native_transfer_allowed(n->kind,n->emmc_host,n->update,a,v))return 0;break;
        default:return 0;
        }
    }
    return store(a,&v,1);
}
static int barrier(void *cookie)
{
    if(!current(cookie))return 0;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);
}
static int outside(void *cookie,const void *v,size_t bytes)
{
    struct a733_mmc_platform_native *n=cookie;
    return n&&separate(n,sizeof(*n),v,bytes)&&separate(n->locks,sizeof(*n->locks),v,bytes);
}
int a733_mmc_platform_native_init_selected(struct a733_mmc_platform_native *n,
    struct a733_platform_locks *locks,void *task,enum sunxi_mmc_kind kind,enum a733_mmc_emmc_host host,
    int (*admission)(void *,const struct a733_mmc_platform_config *,const struct a733_clock_token *),void *cookie)
{
    if(!n||n->initialized||!locks||!locks->initialized||!task||!admission||
       (kind!=SUNXI_MMC_SD&&kind!=SUNXI_MMC_EMMC)||(unsigned)host>A733_MMC_EMMC_SDC3||
       (kind==SUNXI_MMC_SD&&host))return 0;
    n->emmc_host=host;n->kind=kind;n->locks=locks;n->owner_task=task;n->admit=admission;n->admission_cookie=cookie;
    n->operations=(struct a733_mmc_platform_ops){now,current,admit,enter,leave,retain,read32,write32,barrier,outside,
        read_fifo,write_fifo};
    n->initialized=1;return 1;
}

int a733_mmc_platform_native_init(struct a733_mmc_platform_native *n,
    struct a733_platform_locks *locks,void *task,enum sunxi_mmc_kind kind,
    int (*admission)(void *,const struct a733_mmc_platform_config *,const struct a733_clock_token *),void *cookie)
{return a733_mmc_platform_native_init_selected(n,locks,task,kind,A733_MMC_EMMC_SDC2,admission,cookie);}

int a733_mmc_platform_native_bind_update(struct a733_mmc_platform_native *n,struct a733_storage_update *u)
{
    if(!n||!n->initialized||n->update||!u||!current(n)||!a733_storage_update_owned(u,u->platform)||
       u->platform->update!=u||u->platform->cookie!=n||u->platform->ops.write!=write32||
       u->platform->ops.write_fifo!=write_fifo||
       u->platform->config.kind!=n->kind||u->platform->config.emmc_host!=n->emmc_host)return 0;
    n->update=u;return 1;
}
