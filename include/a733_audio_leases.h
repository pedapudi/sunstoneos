#ifndef SUNSTONE_A733_AUDIO_LEASES_H
#define SUNSTONE_A733_AUDIO_LEASES_H
#include "a733_clock_leases.h"
#include "a733_i2s_platform.h"

struct a733_audio_leases {
    struct a733_clock_leases *registry;
    struct a733_audio_operations backend, operations;
    void *context;
    uint64_t owner;
    struct a733_clock_token tokens[4];
    uint32_t initialized, failed;
};
/* Compose one trusted audio parent/local binding with the common registry.
 * Copy backend enter/leave/now/read/write and forward them with its original
 * context. Its resource callbacks are ignored/replaced. Use shared FreeRTOS
 * register locks and ordered native MMIO callbacks in production. The registry
 * and backend must use the same monotonic clock. Serialize this object, parent
 * and local binding. The registry itself accepts concurrent callers.
 * Storage and callback state remain disjoint and live for platform lifetime.
 * No MMIO or allocation. Pass &binding.operations and binding to parent init.
 * Existing GPU clock users must also acquire registry leases before coexistence;
 * this adapter alone does not retrofit their resource manager.
 */
int a733_audio_leases_init(struct a733_audio_leases *,struct a733_clock_leases *,
    uint64_t owner,const struct a733_audio_operations *backend,void *context);
#endif
