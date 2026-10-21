#include "a733_pcie_native.h"
#ifdef SUNSTONE_PCIE_EPF_TEST
#include "a733_pcie_epf_test.h"
#endif
#ifdef SUNSTONE_PCIE_FUNCTION
#include "a733_pcie_function.h"
#endif
#include "a733_platform_locks.h"
#include "task.h"
#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1
#error "PCIe native binding requires scheduler-state, task and mutex-owner APIs"
#endif
static int current(void *cookie)
{
    struct a733_pcie_native *n=cookie;
    uint64_t el,flags,nesting,critical;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, daif" : "=r"(el),"=r"(flags));
    if(!n||!n->initialized||el!=4||(flags&0x80))return 0;
    __asm__ volatile("msr daifset, #2" ::: "memory");
#if configNUMBER_OF_CORES > 1
    extern volatile uint64_t ullPortInterruptNestings[],ullCriticalNestings[];
    uint64_t core;__asm__ volatile("mrs %0, tpidr_el1" : "=r"(core));
    nesting=core<configNUMBER_OF_CORES?ullPortInterruptNestings[core]:1;
    critical=core<configNUMBER_OF_CORES?ullCriticalNestings[core]:1;
#else
    extern uint64_t ullPortInterruptNesting;
    extern volatile uint64_t ullCriticalNesting;
    nesting=ullPortInterruptNesting;critical=ullCriticalNesting;
#endif
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
    return !nesting&&!critical&&xTaskGetSchedulerState()==taskSCHEDULER_RUNNING&&
        xTaskGetCurrentTaskHandle()==(TaskHandle_t)n->owner_task;
}
static int now(void *cookie,uint64_t *v)
{struct a733_pcie_native *n=cookie;return n->locks->now(n->locks->context,v);}
static int admit(void *cookie,const struct a733_pcie_config *c)
{
    struct a733_pcie_native *n=cookie;
    return current(n)&&c==&n->port->config?n->admit(n->admission_cookie,c):-1;
}
static int enter(void *cookie,uint64_t deadline)
{
    struct a733_pcie_native *n=cookie;
    return current(n)&&a733_platform_lock_enter(n->locks,A733_PLATFORM_REGISTER_LOCK,deadline);
}
static void leave(void *cookie)
{struct a733_pcie_native *n=cookie;a733_platform_lock_leave(n->locks,A733_PLATFORM_REGISTER_LOCK);}
static void retain(void *cookie)
{
    struct a733_pcie_native *n=cookie;
    configASSERT(a733_platform_lock_retain(n->locks,A733_PLATFORM_REGISTER_LOCK)==1);
}
int a733_pcie_native_address(uintptr_t a,unsigned width,unsigned writing)
{
    if((width!=1&&width!=2&&width!=4)||(a&(width-1)))return 0;
    if(a733_pcie_phy_address(a,width,writing))return 1;
    if(a>=0x20000000&&a<=0x20001000-width)return !writing;
    if(a>=0x06000000&&a<=0x06001000-width) {
        if(!writing)return 1;
        if(width==2&&(a==0x06000004||a==0x0600000a))return 1;
        if(width==2&&a>=0x06000070&&a<=0x060000fc)return 1;
        if(width!=4)return 0;
        if(a==0x06000010||a==0x06000014||a==0x06000018||a==0x06000710||
            a==0x0600080c||a==0x060008bc)return 1;
        if(a>=0x06000828&&a<=0x06000880)
            return (a-0x06000828)%12==0||(a-0x0600082c)%12==0;
        /* Actual capability-relative writes receive an additional exact check. */
        return a>=0x06000040&&a<=0x06000114;
    }
    if(width!=4)return 0;
    if(a>=0x06300000&&a<0x06302000) {
        unsigned off=(unsigned)(a-0x06300000);
        if(!writing)return off%0x200==4||off%0x200==0x104;
        return off<=24;
    }
    if(a==0x06400c00)return 1;
    if(a==0x06400e0c)return !writing;
    if(a>=0x02002000&&a<0x02004000) {
        switch(a-0x02002000) {
        case 0x574:case 0x1380:case 0x1384:case 0x138c:return 1;
        case 0:case 0xa0:case 0xa8:case 0xac:case 0x500:case 0x580:case 0x588:
        case 0x5c0:case 0x5e0:case 0x13c0:case 0x13c4:case 0x1908:case 0x1988:case 0x1b28:return !writing;
        default:return 0;
        }
    }
    if(a==0x0709016c||a==0x06c00004||a==0x06c000f0||a==0x06c06c44)return 1;
    if(a==0x03400194)return writing;
    switch(a) {
    case 0x07090160:case 0x070101ac:case 0x07067000:case 0x07067008:
    case 0x03400000:case 0x03400114:case 0x03400214:case 0x03400314:return !writing;
    default:return 0;
    }
}
static int allowed(struct a733_pcie_native *n,uintptr_t a,unsigned width,unsigned writing)
{
    if(!n)return 0;
    int address=a733_pcie_native_address(a,width,writing);
#ifdef SUNSTONE_PCIE_EPF_TEST
    address=address||a733_pcie_epf_native_address(n->port,a,width,writing);
#endif
#ifdef SUNSTONE_PCIE_FUNCTION
    address=address||a733_pcie_function_native_address(n->port,a,width,writing);
#endif
    return current(n)&&address&&
        !(atomic_load_explicit(&n->locks->retained,memory_order_acquire)&1)&&
        xSemaphoreGetMutexHolder(n->locks->mutex[A733_PLATFORM_REGISTER_LOCK])==(TaskHandle_t)n->owner_task;
}
static int read(void *cookie,uintptr_t a,unsigned width,uint32_t *v)
{
    struct a733_pcie_native *n=cookie;if(!v||!allowed(n,a,width,0))return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    *v=width==1?*(volatile const uint8_t *)a:width==2?*(volatile const uint16_t *)a:*(volatile const uint32_t *)a;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
static int write(void *cookie,uintptr_t a,unsigned width,uint32_t v)
{
    struct a733_pcie_native *n=cookie;if(!allowed(n,a,width,1))return 0;
    if((width==1&&v>255)||(width==2&&v>65535))return 0;
    if(a==0x03400194&&v!=0x0300ff00)return 0;
    if(a==0x06000004&&v!=0x102){
#if defined(SUNSTONE_PCIE_EPF_TEST) || defined(SUNSTONE_PCIE_FUNCTION)
        int client=0;
#ifdef SUNSTONE_PCIE_EPF_TEST
        client=client||a733_pcie_epf_native_write(n->port,a,width,v);
#endif
#ifdef SUNSTONE_PCIE_FUNCTION
        client=client||a733_pcie_function_native_write(n->port,a,width,v);
#endif
        if(!client)return 0;
#else
        return 0;
#endif
    }
#ifdef SUNSTONE_PCIE_EPF_TEST
    if((n->port->test||(!n->port->function&&!a733_pcie_native_address(a,width,1)))&&
       !a733_pcie_epf_native_write(n->port,a,width,v))return 0;
#endif
#ifdef SUNSTONE_PCIE_FUNCTION
    /* A claimed function client's exact policy covers every later write. */
    if((n->port->function||(!n->port->test&&!a733_pcie_native_address(a,width,1)))&&
       !a733_pcie_function_native_write(n->port,a,width,v))return 0;
#endif
    /* Outbound iATU writes must match the owned region's recorded window. */
    if(a>=0x06300000&&a<0x06302000&&!a733_pcie_atu_write_allowed(n->port,a,v))return 0;
    if(a==0x0600000a&&v!=0x0604)return 0;
    if(a>=0x06000040&&a<=0x06000114&&
        (a!=0x06000000u+n->port->root_cap+0xcu&&a!=0x06000000u+n->port->root_cap+0x30u))return 0;
    uint32_t mask=0;
    switch(a) {
    case 0x02002574:mask=0x10002;break;
    case 0x02003380:mask=0x8100001f;if((v&mask)!=0x80000000)return 0;break;
    case 0x02003384:mask=0x8700001f;if((v&mask)!=0x82000000)return 0;break;
    case 0x0200338c:mask=0x30000;break;
    case 0x0709016c:mask=1u<<5;break;
    case 0x06c00004:mask=0x70000;break;
    case 0x06c000f0:mask=1u<<29;break;
    case 0x06c06c44:if(v!=1)return 0;break;
    default:break;
    }
    if(mask) {
        __asm__ volatile("dsb sy" ::: "memory");
        uint32_t old=*(volatile const uint32_t *)a;
        if((old^v)&~mask)return 0;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if(width==1)*(volatile uint8_t *)a=(uint8_t)v;
    else if(width==2)*(volatile uint16_t *)a=(uint16_t)v;
    else *(volatile uint32_t *)a=v;
    __asm__ volatile("dsb sy" ::: "memory");return 1;
}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);
}
static int outside(void *cookie,const void *v,size_t bytes)
{
    struct a733_pcie_native *n=cookie;
    return n&&separate(n,sizeof(*n),v,bytes)&&separate(n->locks,sizeof(*n->locks),v,bytes);
}
int a733_pcie_native_init(struct a733_pcie_native *n,struct a733_pcie *port,
    struct a733_platform_locks *locks,void *task,
    int (*admission)(void *,const struct a733_pcie_config *),void *cookie)
{
    if(!n||n->initialized||!port||port->self||!locks||!locks->initialized||!task||!admission||
        !separate(n,sizeof(*n),port,sizeof(*port))||!separate(n,sizeof(*n),locks,sizeof(*locks))||
        !separate(port,sizeof(*port),locks,sizeof(*locks)))return 0;
    n->port=port;n->locks=locks;n->owner_task=task;n->admit=admission;n->admission_cookie=cookie;
    n->operations=(struct a733_pcie_ops){now,current,admit,enter,leave,retain,read,write,outside};
    n->initialized=1;return 1;
}
