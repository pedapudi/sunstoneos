#ifndef SUNSTONE_A733_NPU_RESERVED_H
#define SUNSTONE_A733_NPU_RESERVED_H
#include "a733_memory_reservations.h"
#include "npu_convolution.h"

#define A733_NPU_RESERVED_TABLE_BYTES 20480u
#if defined(SUNSTONE_NPU_NN_POOL_GRAPH)
#define A733_NPU_RESERVED_BYTES 3235840u
#elif defined(SUNSTONE_NPU_LARGE_GRAPH)
#define A733_NPU_RESERVED_BYTES 2916352u
#else
#define A733_NPU_RESERVED_BYTES (2u * NPU_CONVOLUTION_COMMAND_BYTES + 4u * 4096u + A733_NPU_RESERVED_TABLE_BYTES)

#endif

/* Zero-initialize once. Private pre-scheduler session; keep it and the native
 * linker pool alive. Serialize this constructor with every pool allocation or
 * mutation. Session storage, fatal callback and its cookie retain platform lifetime.
 * One parent lease covers every buffer and all table storage. DEVICE means
 * the convolution service owns the complete session, including CPU input/output
 * access authorized by its existing client protocol. The allocator cannot
 * release it at individual job completion. Preparation failure marks the parent RETAINED; later convolution faults
 * leave it permanently reserved as DEVICE unless explicitly retained.
 * No copy, teardown, independent child release or reinitialization is supported.
 */
struct a733_npu_reserved {
    struct npu_convolution convolution;
    struct device_memory_pool *pool;
    struct device_memory_lease lease;
    struct device_memory_extent extent;
    unsigned admitted;
};
/* Actual native preparation, no peripheral register access. The existing
 * native linker pool must provide a contiguous 1,085,440-byte allocation.
 * Inputs and session metadata must be outside that complete pool. The selected catalogue model is
 * copied and authenticated by npu_convolution_prepare. register_cpu must map
 * the NPU's physical Device page at 0x03600000. Caller provides valid pointers,
 * independent recovery, exclusive remote/CPU ownership and pinned mappings.
 *
 * Success returns the embedded convolution for the existing handoff, startup
 * binding, startup polling and client APIs. This does not activate the service,
 * supply physical startup evidence or authorize device commands. Once admitted,
 * the parent allocation remains reserved through all jobs and all failures.
 * Argument/capacity refusal before admission leaves the session unchanged.
 */
struct npu_convolution *a733_npu_reserved_prepare(struct a733_npu_reserved *,
    uint64_t owner, const void *model, size_t model_bytes, uintptr_t register_cpu,
    void (*fatal)(void *), void *fatal_cookie);
#endif
