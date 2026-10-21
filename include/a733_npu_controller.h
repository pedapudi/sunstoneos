#ifndef SUNSTONE_A733_NPU_CONTROLLER_H
#define SUNSTONE_A733_NPU_CONTROLLER_H
#include "a733_npu_mmu.h"

#define A733_NPU_FETCH_BYTES 524280u
#define A733_NPU_BOOTSTRAP_BYTES 208u

enum a733_npu_state {
    A733_NPU_COLD, A733_NPU_BOOTING, A733_NPU_READY, A733_NPU_RUNNING,
    A733_NPU_CANCEL_ASSERTED, A733_NPU_CANCEL_WAIT, A733_NPU_DONE,
    A733_NPU_CANCELED, A733_NPU_DRAINED, A733_NPU_CLOSED, A733_NPU_FAULT
};
enum a733_npu_fault {
    A733_NPU_NO_FAULT, A733_NPU_IO, A733_NPU_IDENTITY, A733_NPU_NOT_IDLE,
    A733_NPU_REGISTER, A733_NPU_IRQ, A733_NPU_TIMEOUT, A733_NPU_MEMORY
};
struct a733_npu_command {
    uint32_t device;
    size_t readable_bytes; /* Entire prefetch span must be mapped and readable. */
    void *token; /* Stable privileged resource bundle; never an application pointer. */
};
struct a733_npu_ops {
    /* All callbacks are bounded and serialized by the caller. MMIO failure
     * permanently faults this instance; no later register callback is issued. */
    int (*read)(void *, unsigned offset, uint32_t *);
    int (*write)(void *, unsigned offset, uint32_t);
    uint64_t (*microseconds)(void *); /* Monotonic modulo 2^64. */
    void (*delay)(void *, unsigned microseconds);
    void (*barrier)(void *);
    /* Validate command/table/payload reservations and mappings, clean device
     * reads, and clean+invalidate device writes. Bootstrap includes the MMU
     * image. The prepared image is immutable until close; all job buffers stay
     * pinned until reap. No cache line may overlap another CPU owner's data. */
    int (*publish)(void *, const struct a733_npu_mmu_image *,
                   const struct a733_npu_command *, int bootstrap);
    /* After event AND full idle (or confirmed cancel AND full idle), invalidate
     * device-written buffers and order visibility. Failure retains ownership. */
    int (*finish)(void *, const struct a733_npu_command *, int canceled);
};
struct a733_npu_controller {
    struct a733_npu_ops ops;
    void *cookie;
    struct a733_npu_command job;
    struct a733_npu_mmu_image mmu;
    uint64_t start, timeout, cancel_start, cancel_timeout;
    enum a733_npu_state state;
    enum a733_npu_fault fault;
    uint32_t irq_seen, last_idle, chip_model, chip_revision, chip_product, chip_date;
    unsigned bootstrap;
};

/* Privileged, single-owner controller core. Zero-initialize once. The platform
 * must establish power/clocks/reset ownership, mask the GIC interrupt, and arm
 * an independently qualified watchdog before any call. Polling owns +0x10;
 * no concurrent interrupt handler may read that register. Reinitializing or
 * clearing a faulted instance is forbidden until external recovery proves all
 * accesses have stopped. The reference target is product 0x1000003b/rev 0x9202.
 *
 * open borrows the MMU image, bootstrap allocation, and resource bundle until
 * successful close. submit borrows job buffers until reap. Fault retains every
 * borrowed resource. Calls are not reentrant. Return 0 indicates refusal or
 * fault; inspect state/fault. All timeouts are 1..INT64_MAX microseconds.
 *
 * Commands must be trusted, validated native streams ending in an event 0
 * from the pixel engine followed by END. This core is not a command validator.
 * Internal SRAM VA 0x400000..0x47ffff must be reserved from ordinary mappings. */
int a733_npu_open(struct a733_npu_controller *, const struct a733_npu_ops *, void *,
                  const struct a733_npu_mmu_image *, const struct a733_npu_command *, uint64_t);
int a733_npu_submit(struct a733_npu_controller *, const struct a733_npu_command *, uint64_t);
enum a733_npu_state a733_npu_poll(struct a733_npu_controller *);
/* Optional interrupt profile: only the serialized worker calls this. Sample
 * events after the owned interrupt is masked and its notification collected.
 * Without a sample, deadlines/cancellation and previously latched completion
 * advance, but no initial destructive event read occurs. */
enum a733_npu_state a733_npu_advance(struct a733_npu_controller *, int sample_events);
/* Deadline-only progress while interrupt masking is deferred; no MMIO/cache. */
enum a733_npu_state a733_npu_check_timeout(struct a733_npu_controller *);
int a733_npu_cancel(struct a733_npu_controller *, uint64_t stop_timeout);
int a733_npu_reap(struct a733_npu_controller *, void **token, int *canceled);
int a733_npu_close(struct a733_npu_controller *);
/* Encode flush + reference initialization, including zero AXI SRAM base/end
 * states, + event + END and one END padding pair. No MMIO. Writes exactly
 * A733_NPU_BOOTSTRAP_BYTES; capacity and alignment are checked first.
 * The caller supplies a larger mapped allocation for the reference prefetch. */
int a733_npu_bootstrap(void *, size_t);
#endif
