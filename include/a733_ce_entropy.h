#ifndef SUNSTONE_A733_CE_ENTROPY_H
#define SUNSTONE_A733_CE_ENTROPY_H
#include "a733_ce.h"
#include "device_memory_pool.h"
#define A733_CE_ENTROPY_MAX 256u
enum a733_ce_entropy_result { A733_CE_ENTROPY_PENDING,A733_CE_ENTROPY_BUSY,
    A733_CE_ENTROPY_BYTES,A733_CE_ENTROPY_CONSUMED,A733_CE_ENTROPY_RETAINED,A733_CE_ENTROPY_INVALID };
struct a733_ce_entropy {
    uintptr_t self;
    struct {
        struct device_memory_pool *pool;
        struct device_memory_lease lease;
        struct device_memory_extent extent;
        struct a733_clock_leases *registry;
        struct a733_clock_token token;
        struct a733_ce_config config;
        struct a733_ce_ops ops;
        void *cookie;
    } binding;
    struct a733_ce controller;
    atomic_uint busy;
    uint64_t next_generation,generation,deadline,last_now;
    unsigned initialized,active,consumed,need_request,fault,terminal;
    size_t wanted,filled;
    uint8_t staging[A733_CE_ENTROPY_MAX];
};
/* Zero service storage once. Bind one actual CPU-owned page of this owner's device pool. The config's
 * address fields are replaced by that page's real physical extent. The complete
 * page transitions to DEVICE and remains reserved through success and failure;
 * no CPU/release transition or secondary pool overlays exist.
 * Hold the pool's existing common service lock for this call and EVERY request,
 * poll and other allocator/consumer operation. All state is private, disjoint
 * from the pool; never copy/reinitialize it or call its CE controller directly.
 * Source-defined task completion permits same-reservation reuse, not global
 * drain or memory release. Authority/health/startup qualification requirements
 * of a733_ce_init remain applicable. Output remains UNQUALIFIED for production
 * security use until raw-source characterization and a source-backed startup
 * protocol for the current test-enable epoch exist; this service proves neither. */
int a733_ce_entropy_bind(struct a733_ce_entropy *,struct device_memory_pool *,
    struct device_memory_lease,const struct a733_ce_config *,const struct a733_ce_ops *,
    void *,struct a733_clock_leases *,const struct a733_clock_token *);
/* Serialized boot only: get the actual singleton linker pool, acquire one page
 * and bind native CE MMIO/cache operations. No peripheral MMIO here. Must run
 * after platform_init, before concurrent users, with real native authority. */
int a733_ce_entropy_native_init(struct a733_ce_entropy *,const struct a733_ce_config *,
    const struct a733_ce_ops *,void *,struct a733_clock_leases *,const struct a733_clock_token *);
/* One request at a time, 1..256 bytes. A generation is issued only on PENDING.
 * The complete caller output remains unchanged until BYTES; no partial result.
 * Every request has one absolute deadline, including all of its 32-byte blocks.
 * Generation exhaustion refuses future requests rather than recycling handles. */
enum a733_ce_entropy_result a733_ce_entropy_request(struct a733_ce_entropy *,size_t,
    uint64_t deadline_us,uint64_t *generation);
enum a733_ce_entropy_result a733_ce_entropy_poll(struct a733_ce_entropy *,uint64_t generation,
    uint8_t *output,size_t bytes);
/* Health/profile changes, stale binding, controller failures and deadlines retain
 * resources and zero private staging. Failed DMA memory remains quarantined and
 * is never zeroed while device access could continue. No automatic retry/reset. */
#endif
