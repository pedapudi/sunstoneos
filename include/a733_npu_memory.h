#ifndef SUNSTONE_A733_NPU_MEMORY_H
#define SUNSTONE_A733_NPU_MEMORY_H
#include "a733_npu_controller.h"

#define A733_NPU_MEMORY_MAX_BUFFERS 64u

enum a733_npu_buffer_role {
    A733_NPU_BUFFER_READ, A733_NPU_BUFFER_WRITE,
    A733_NPU_BUFFER_COMMAND, A733_NPU_BUFFER_BOOTSTRAP
};
enum a733_npu_cache_op {
    A733_NPU_CACHE_CLEAN, A733_NPU_CACHE_CLEAN_INVALIDATE,
    A733_NPU_CACHE_INVALIDATE
};
enum a733_npu_memory_state {
    A733_NPU_MEMORY_EMPTY, A733_NPU_MEMORY_IDLE,
    A733_NPU_MEMORY_DEVICE, A733_NPU_MEMORY_FAULT
};
struct a733_npu_buffer {
    uintptr_t cpu;
    uint64_t physical;
    size_t bytes;
    uint32_t device;
    enum a733_npu_buffer_role role;
};
struct a733_npu_translation {
    uint64_t physical;
    unsigned attributes, shareability;
};
struct a733_npu_memory_ops {
    int (*translate)(void *, uintptr_t, int write, struct a733_npu_translation *);
    int (*cache)(void *, enum a733_npu_cache_op, uintptr_t, size_t);
    void (*barrier)(void *);
};
struct a733_npu_memory {
    struct a733_npu_buffer buffers[A733_NPU_MEMORY_MAX_BUFFERS];
    size_t count;
    uintptr_t arena;
    size_t capacity;
    struct a733_npu_mmu_image image;
    struct a733_npu_memory_ops ops;
    void *cookie;
    struct a733_npu_command active;
    enum a733_npu_memory_state state;
};

/* Privileged, serialized memory policy; no allocation or MMIO. Zero once.
 * Caller supplies dedicated pinned RAM with stable mappings, no other CPU or
 * device aliases, and 64-byte exclusive cache ownership. translate must check
 * current EL1 read/write permission and return PA, MAIR byte, and shareability.
 * cache must complete to the point of coherency with a supported stride and
 * barriers. All callbacks must be bounded; native callbacks remain separate.
 *
 * init copies and sorts at most 64 buffers, checks CPU mappings, and builds
 * private 4 KiB NPU page tables. Exactly one bootstrap allocation is required.
 * CPU/PA/device bases and lengths of buffers must be 4 KiB aligned. All CPU,
 * physical, and device ranges are disjoint; SRAM VA 0x400000..0x47ffff is
 * excluded. Arena requirements match a733_npu_mmu_build. Failure leaves both
 * this object and the arena unchanged. Input metadata is not retained.
 *
 * This object, tables, command streams, and allocation identities must remain
 * private and immutable from initialization through successful controller
 * close. Payload bytes may change only when controller ownership permits it.
 * All buffers are reserved for the entire session. Fault retains everything;
 * only external recovery proving quiescence permits disposal of a faulted
 * instance. This policy does not validate command semantics or SRAM remaps. */
int a733_npu_memory_init(struct a733_npu_memory *, void *arena, size_t capacity,
                         uint64_t physical, const struct a733_npu_buffer *, size_t,
                         const struct a733_npu_memory_ops *, void *cookie);
/* Look up a command by device base, returning a stable internal resource token.
 * The entire registered allocation is the readable prefetch span. */
int a733_npu_memory_command(const struct a733_npu_memory *, uint32_t device,
                            struct a733_npu_command *);
/* Controller callback adapters pass their memory object here. publish checks
 * the selected image, canonical table contents, all live CPU translations,
 * command identity, and role before any cache maintenance. finish is legal
 * only after the controller proves event/cancel completion AND bus idle.
 * IDLE denotes completed cache transfer, not permission to bypass reap/close.
 * Failure latches MEMORY_FAULT and ends further callbacks. */
int a733_npu_memory_publish(struct a733_npu_memory *, const struct a733_npu_mmu_image *,
                            const struct a733_npu_command *, int bootstrap);
int a733_npu_memory_finish(struct a733_npu_memory *, const struct a733_npu_command *);
#endif
