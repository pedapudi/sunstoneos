/* The OS image's IOMMU wiring (apps/os/iommu.h): the owner's register access, its table memory from the unified
 * ledger, the A733_CLOCK_IOMMU_SYS lease, one mutex that serializes every caller, and the device address space. */
#include "os.h"
#include "../board/application.h"
#include "iommu.h"
#include "a733_clock_leases.h"
#include "a733_counter_clock.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include "aarch64_device_memory.h"
#include "dma_address_width.h"
#include "sunstone_deadline.h"
#include "umem.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <string.h>

#define IOMMU_OWNER UINT64_C(0x494f4d4d00000001) /* "IOMM", lease owner of the IOMMU clock */
/* Bound of one call, inside which the clock lease and the mutex are taken: a call's own register work takes well under
 * a millisecond (two 10 us waits per span). */
#define IOMMU_CALL_US 50000u
/* The first device address handed out; the page at 0 stays unmapped. */
#define IOVA_BASE UINT64_C(0x10000000)

struct umem *os_large_umem(void);
static struct {
    struct a733_iommu_owner owner;
    struct a733_clock_token clock;
    uint64_t next_iova, limit;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
} iommu;

static uint64_t now_us(void) { return sunstone_deadline_now_us(); }
static int clock_now(void *c, uint64_t *t) { (void)c; *t = now_us(); return 1; }
static int device(uintptr_t a)
{
    static const struct a733_mapping_rule rule = {A733_MAPPING_WRITE, 1, {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    return a733_mapping_check(aarch64_device_translate, a, a, &rule);
}
static int reg_read(void *c, uint32_t off, uint32_t *v)
{
    (void)c;
    if(off >= 2 * A733_IOMMU_BANK_BYTES || (off & 3) || !device(A733_IOMMU_BASE + off)) return -1;
    *v = a733_mmio_read32_dsb_sy_both(A733_IOMMU_BASE + off);
    return 1;
}
static int reg_write(void *c, uint32_t off, uint32_t v)
{
    (void)c;
    if(off >= 2 * A733_IOMMU_BANK_BYTES || (off & 3) || !device(A733_IOMMU_BASE + off)) return -1;
    a733_mmio_write32_dsb_sy_both(A733_IOMMU_BASE + off, v);
    return 1;
}
/* Non-cacheable kernel pages from the ledger, below the IOMMU's 34-bit table address limit. */
static int table(void *c, size_t bytes, size_t align, uint32_t **cpu, uint64_t *pa)
{
    (void)c;
    struct umem *ledger = os_large_umem();
    const struct umem_request r = {.bytes = (bytes + 4095) & ~(uint64_t)4095, .align = align, .owner = UMEM_OWNER_KERNEL,
                                   .flags = UMEM_RAW, .policy = UMEM_NC, .phys_max = A733_IOMMU_PHYSICAL_LIMIT};
    struct umem_buf b;
    if(!ledger || umem_alloc(ledger, &r, &b) != UMEM_OK) return -1;
    *cpu = (uint32_t *)(uintptr_t)b.base; *pa = b.base;
    return 1;
}
static void release(void *c, uint32_t *cpu, uint64_t pa, size_t bytes)
{
    (void)c; (void)bytes;
    struct umem *ledger = os_large_umem();
    struct umem_buf b;
    if(ledger && (uintptr_t)cpu == pa && umem_lookup(ledger, pa, &b) == UMEM_OK) (void)umem_free(ledger, &b);
}
/* The tables are non-cacheable: a full-system barrier orders their stores before the next register write. */
static int publish(void *c) { (void)c; __asm__ volatile("dsb sy" ::: "memory"); return 1; }
/* Lease deadlines are in the counter clock that the clock leases compare against, as for the G2D service's own
 * leases. A take ends no later than 10 ms before the session's deadline; a release, which the quiesce path may make
 * after that point, has only the call's own bound. */
static uint64_t lease_now(void)
{
    uint64_t t = 0;
    return a733_counter_clock_now(&providers.shared.counter, &t) == 1 ? t : 0;
}
static int clock_lease(void *c, int take)
{
    (void)c;
    const uint64_t at = lease_now(), session = take ? board.session.deadline_us - 10000 : UINT64_MAX;
    if(!at || at >= session) return -1;
    const uint64_t limit = at + IOMMU_CALL_US < session ? at + IOMMU_CALL_US : session;
    for(;;) {
        const enum a733_clock_result r = take ?
            a733_clock_acquire(&providers.shared.clocks, IOMMU_OWNER, A733_CLOCK_FABRIC, A733_CLOCK_IOMMU_SYS, limit,
                               &iommu.clock) :
            a733_clock_release(&providers.shared.clocks, &iommu.clock, limit);
        if(r == A733_CLOCK_OK) { if(!take) memset(&iommu.clock, 0, sizeof(iommu.clock)); return 1; }
        if(r != A733_CLOCK_BUSY || lease_now() >= limit) return -1;
        (void)sunstone_deadline_wait_until(now_us() + 50);
    }
}
static const struct a733_iommu_owner_ops ops = {clock_now, reg_read, reg_write, table, release, publish, clock_lease};

/* The mutex is created at the first call; the start entries run before any service task, one at a time. */
static int enter(void)
{
    if(!iommu.mutex) {
        taskENTER_CRITICAL();
        if(!iommu.mutex) {
            iommu.mutex = xSemaphoreCreateMutexStatic(&iommu.mutex_storage);
            if(iommu.mutex && !iommu.owner.initialized && a733_iommu_owner_init(&iommu.owner, &ops, NULL) != 1)
                iommu.mutex = NULL;
            iommu.next_iova = IOVA_BASE; iommu.limit = A733_IOMMU_IOVA_LIMIT;
        }
        taskEXIT_CRITICAL();
        if(!iommu.mutex) return 0;
    }
    return xSemaphoreTake(iommu.mutex, pdMS_TO_TICKS(IOMMU_CALL_US / 1000)) == pdTRUE;
}
static void leave(void) { (void)xSemaphoreGive(iommu.mutex); }

int os_iommu_iova(uint64_t bytes, uint64_t align, uint64_t *iova)
{
    if(!iova || !bytes || (bytes & 4095) || align < 4096 || (align & (align - 1)) || !enter()) return -1;
    const uint64_t at = (iommu.next_iova + align - 1) & ~(align - 1);
    int r = -1;
    if(at >= iommu.next_iova && at < iommu.limit && bytes <= iommu.limit - at) { *iova = at; iommu.next_iova = at + bytes; r = 1; }
    leave();
    return r;
}
int os_iommu_attach(uint16_t masters, const struct a733_iommu_span *s, unsigned count)
{
    if(!enter()) return -1;
    const int r = a733_iommu_owner_attach(&iommu.owner, masters, s, count);
    leave();
    return r;
}
int os_iommu_map(const struct a733_iommu_span *s, unsigned count)
{
    if(!enter()) return -1;
    const int r = a733_iommu_owner_map(&iommu.owner, s, count);
    leave();
    return r;
}
int os_iommu_unmap(const struct a733_iommu_span *s, unsigned count)
{
    if(!enter()) return -1;
    const int r = a733_iommu_owner_unmap(&iommu.owner, s, count);
    leave();
    return r;
}
int os_iommu_detach(uint16_t masters)
{
    if(!enter()) return -1;
    const int r = a733_iommu_owner_detach(&iommu.owner, masters);
    if(r == 1 && !iommu.owner.attached) iommu.next_iova = IOVA_BASE;
    leave();
    return r;
}
int os_iommu_scan(unsigned master, struct a733_iommu_owner_fault *out)
{
    if(!enter()) return -1;
    const int r = a733_iommu_owner_scan(&iommu.owner, master);
    if(r >= 0 && out && master < A733_IOMMU_OWNER_MASTERS) *out = iommu.owner.faults[master];
    leave();
    return r;
}
const struct a733_iommu_owner *os_iommu_owner(void) { return &iommu.owner; }
