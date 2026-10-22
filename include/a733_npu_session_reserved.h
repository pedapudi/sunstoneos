#ifndef SUNSTONE_A733_NPU_SESSION_RESERVED_H
#define SUNSTONE_A733_NPU_SESSION_RESERVED_H
#include "a733_memory_reservations.h"
#include "npu_session.h"

/* Zero-initialize once; keep it and the native pool alive for the platform
 * lifetime. One parent lease, sized by npu_session_requirements for the
 * selected models, holds the model copies, every NPU allocation and the page
 * tables. The default profile's pool is 2 MiB of image RAM; the extended
 * cached profile's pool is the boot-validated write-back DDR reservation
 * (0x48000000..0x78000000), so models whose plans need more than the image
 * RAM window run there. DEVICE ownership covers the whole session, including
 * CPU input and output access through the session client protocol. A
 * preparation failure after admission marks the lease RETAINED; later faults
 * keep it reserved. No release, copy or reinitialization is provided. */
struct a733_npu_session_reserved {
    struct npu_sessions sessions;
    struct device_memory_pool *pool;
    struct device_memory_lease lease;
    struct device_memory_extent extent;
    unsigned admitted;
};
/* EL1 before scheduling; serialize with every other pool user. Models and
 * metadata must lie outside the pool. register_cpu maps the NPU's Device page
 * at 0x03600000. Refusal before admission leaves session and pool unchanged.
 * Success returns the prepared sessions for handoff, binding and clients. */
struct npu_sessions *a733_npu_session_reserve(struct a733_npu_session_reserved *, uint64_t owner,
    const struct npu_session_model *, unsigned count, uintptr_t register_cpu,
    void (*fatal)(void *), void *fatal_cookie);
#endif
