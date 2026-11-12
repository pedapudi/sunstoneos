/* The USB parent and retained JPEG leases share one ledger-owned NC extent.
 * Construct it once before the scheduler; a failed constructor retains it. */
#include "a733_memory_uncached.h"
#include "a733_usb_memory.h"
#include "owner-memory.h"
#include "usb-memory.h"
#include "FreeRTOS.h"
#include "task.h"

#if !defined(SUNSTONE_USB_LEDGER_MEMORY) || !SUNSTONE_USB_LEDGER_MEMORY
#error "The OS USB memory constructor requires the ledger profile"
#endif

extern const unsigned ss_usb_devices;
/* The bounded owner interface is supplied by the OS ledger. */
extern void *os_owner_memory_below(uint64_t, uint64_t, unsigned, uint64_t);
static struct device_memory_pool pool;
static struct device_memory_pool *published;
static uintptr_t parent_base;
static unsigned attempted;

int a733_memory_uncached_prepare(void) { return 1; }

uintptr_t a733_usb_memory_parent_base(void)
{
    return __atomic_load_n(&published, __ATOMIC_ACQUIRE) ? parent_base : 0;
}

int a733_usb_memory_pool_valid(const struct device_memory_pool *candidate)
{
    return candidate&&candidate==__atomic_load_n(&published,__ATOMIC_ACQUIRE)&&candidate->initialized&&
        candidate->memory_type==DEVICE_MEMORY_NON_CACHEABLE&&candidate->memory_class==DEVICE_MEMORY_CLASS_NONE&&
        candidate->backing.cpu==parent_base&&candidate->backing.physical==parent_base&&
        candidate->backing.bytes==A733_USB_MEMORY_ARENA_FOR(SUNSTONE_USB_HOST_DEVICES);
}

struct device_memory_pool *a733_memory_uncached_init(void)
{
    struct device_memory_pool *ready=__atomic_load_n(&published,__ATOMIC_ACQUIRE);
    if(ready)return ready;
    if(attempted||(ss_usb_devices&&ss_usb_devices!=SUNSTONE_USB_HOST_DEVICES)||
       xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return NULL;
#if defined(__aarch64__) && !defined(__linux__)
    uint64_t el;__asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    if(el!=4)return NULL;
#endif
    attempted=1;
    const uint64_t bytes=A733_USB_MEMORY_ARENA_FOR(SUNSTONE_USB_HOST_DEVICES);
    const uint64_t limit=UINT64_C(1)<<32;
    void *memory=os_owner_memory_below(bytes,UMEM_CHUNK,OS_MEMORY_DEVICE,limit);
    uintptr_t base=(uintptr_t)memory;
    if(!base||(base&(UMEM_CHUNK-1))||bytes>limit||base>limit-bytes||
       !os_usb_memory_alias(base,A733_USB_MEMORY_PARENT_BYTES)||
       !device_memory_pool_init_typed(&pool,base,base,(size_t)bytes,DEVICE_MEMORY_CLASS_NONE,
                                      DEVICE_MEMORY_NON_CACHEABLE))return NULL;
    parent_base=base;
    __atomic_store_n(&published,&pool,__ATOMIC_RELEASE);
    return &pool;
}
