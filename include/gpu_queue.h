#ifndef SUNSTONE_GPU_QUEUE_H
#define SUNSTONE_GPU_QUEUE_H
/* The GPU compute queue: the application interface of the OS GPU compute
 * service (docs/designs/gpu-compute-queue.md).
 *
 * An application creates one context with one program and up to four
 * leases of its own manifest regions, submits one job at a time and waits
 * for that job's completion record. A job binds leased ranges in place, gives
 * a grid of workgroups and a small argument block, and carries the
 * application's sequence number. The queue copies the job description, never
 * buffer contents. It never computes a job on the CPU; REFUSED and FAULT
 * records leave that choice to the application.
 *
 * The context belongs to one client task, bound to a GPU service client slot
 * before the scheduler starts. Only that task calls the functions below.
 * Every call runs in task context. Plain C with no FreeRTOS types, so host
 * checks and application code share these definitions. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
#define GPU_QUEUE_STATIC_ASSERT static_assert
extern "C" {
#else
#define GPU_QUEUE_STATIC_ASSERT _Static_assert
#endif

#define GPU_QUEUE_CONTEXTS 1u
#define GPU_QUEUE_DEPTH 1u
#define GPU_QUEUE_PROGRAMS 1u
#define GPU_QUEUE_LEASES 4u
#define GPU_QUEUE_BINDINGS 4u
#define GPU_QUEUE_ARGUMENT_BYTES 64u
#define GPU_QUEUE_PROGRAM_BYTES 8128u  /* USC code: two 4 KiB pages less one cache line */
#define GPU_QUEUE_MAX_JOB_US UINT64_C(1000000)
#define GPU_QUEUE_LEASE_ALIGN (UINT64_C(2) << 20)

/* Results. A call returns one of these; a completion record carries OK,
 * REFUSED or FAULT.
 * - OK: accepted (submit), completed (record) or done (other calls).
 * - BUSY: the context's one job is still owed a record; nothing changed.
 * - PARKED: the OS closed the device for thermal throttling or image return;
 *   nothing was queued. The context accepts jobs again after the OS reopens
 *   the device.
 * - REFUSED: malformed or not admissible; no device access took place for
 *   it. The context stays usable.
 * - FAULT: the GPU or the service failed. Every later call returns FAULT, and
 *   an owed record arrives with FAULT.
 * - TIMEOUT: the caller's wait ended first. A record stays owed; a pending
 *   create reply is collected by the next call. */
enum gpu_queue_result {
    GPU_QUEUE_OK, GPU_QUEUE_BUSY, GPU_QUEUE_PARKED, GPU_QUEUE_REFUSED,
    GPU_QUEUE_FAULT, GPU_QUEUE_TIMEOUT
};
enum gpu_queue_detail {
    GPU_QUEUE_NO_DETAIL, GPU_QUEUE_PROGRAM, GPU_QUEUE_LEASE, GPU_QUEUE_BINDING,
    GPU_QUEUE_RANGE, GPU_QUEUE_PERMISSION, GPU_QUEUE_SEQUENCE, GPU_QUEUE_GRID,
    GPU_QUEUE_ARGUMENTS, GPU_QUEUE_DEADLINE, GPU_QUEUE_CLOSED,
    /* FAULT details. */
    GPU_QUEUE_HANG, GPU_QUEUE_RESET, GPU_QUEUE_TIMELINE, GPU_QUEUE_MAINTENANCE,
    GPU_QUEUE_SERVICE
};

struct gpu_queue_context { uint32_t generation; };

/* A program from apps/gpu-program/compile.py, compile --profile 3, as the
 * descriptor that the compiler step generates. The queue copies the code into
 * the context's program pages at creation. Programs are trusted: the queue
 * checks the descriptor's shape, never the code's memory behaviour. Source
 * admission stays an offline review step (include/pvr_compute_package.h). */
#define GPU_QUEUE_ABI_GRID 1u
struct gpu_queue_program {
    uint32_t abi;              /* GPU_QUEUE_ABI_GRID */
    uint32_t code_bytes;       /* USC code without the compiler's 8-byte header */
    uint32_t temps;            /* compiler metadata word 0 */
    uint32_t coefficients;     /* 3, or 7 when the program reads the group counts */
    uint32_t local_size[3];    /* product 32 */
    uint32_t reserved;
    const uint32_t *code;
    uint8_t program_sha256[32], source_sha256[32], compiler_sha256[32];
};

/* One lease adopts the first bytes of one region of the application's
 * manifest (struct ss_region, include/sunstone_app.h). bytes is a multiple of
 * 2 MiB at most the region's size rounded up to 2 MiB; zero takes the whole
 * rounded region. The region's base is 2 MiB aligned. CPU and GPU access come
 * from the region's access bits:
 * - CPU write and GPU read: the queue cleans each job's declared range
 *   before the kick;
 * - CPU read and GPU write: the CPU mapping is read-only, so no CPU store
 *   reaches the region. The queue cleans and invalidates the whole lease
 *   once at creation, which removes lines from boot-time writes and earlier
 *   reads, and each job's declared range after the timeline;
 * - CPU read and GPU read: an INPUT region, published at boot; no
 *   maintenance.
 * A region with both CPU write and GPU write, or without CPU read, is
 * refused. */
struct gpu_queue_lease_request { uint32_t region, reserved; uint64_t bytes; };
struct gpu_queue_lease_info {
    uint64_t address;          /* CPU, physical and GPU address */
    uint64_t bytes;
    uint32_t access;           /* the region's SS_ACCESS_* bits */
    uint32_t reserved;
};

/* Creation copies the program, maps the leases with 256 KiB GPU pages, builds
 * the GPU address space once and opens the device. The map never changes
 * afterwards. job_us bounds each job from its kick to its timeline update;
 * a job that exceeds it faults the service. */
struct gpu_queue_context_config {
    uint64_t job_us;           /* 1 .. GPU_QUEUE_MAX_JOB_US */
    const struct gpu_queue_program *program;
    uint32_t lease_count, reserved;
    struct gpu_queue_lease_request lease[GPU_QUEUE_LEASES];
};

/* One buffer range of a job. The program receives the GPU address
 * lease.address + address. The queue maintains the cache lines of
 * [sync_offset, sync_offset + sync_bytes) in the lease for this job, as the
 * lease's access requires; sync_bytes 0 skips maintenance. The range limits
 * maintenance only: the GPU MMU confines the program to the context's
 * leases, not to the ranges of its bindings. */
struct gpu_queue_binding {
    uint16_t lease;
    uint16_t reserved;
    uint32_t address;
    uint32_t sync_offset, sync_bytes;
};
GPU_QUEUE_STATIC_ASSERT(sizeof(struct gpu_queue_binding) == 16, "binding record");

/* One job. sequence rises strictly through the boot, across device close and
 * reopen; the worker writes it to the context's timeline after the work and
 * the record carries it back. groups is the grid in workgroups of the
 * program's local size. not_after_us, when nonzero, is the latest monotonic
 * time at which the worker may kick the job; a later job is refused with
 * GPU_QUEUE_DEADLINE.
 *
 * The program's two pointer arguments:
 * - unified registers 0-1: the argument block, which holds the GPU addresses
 *   of bindings 1 .. bindings-1 as 64-bit words, then argument_bytes bytes;
 * - unified registers 2-3: the GPU address of binding 0. */
struct gpu_queue_job {
    uint32_t sequence;
    uint16_t bindings, argument_bytes;
    uint32_t groups[3];
    uint32_t reserved;
    uint64_t not_after_us;
    struct gpu_queue_binding binding[GPU_QUEUE_BINDINGS];
    unsigned char arguments[GPU_QUEUE_ARGUMENT_BYTES];
};
GPU_QUEUE_STATIC_ASSERT(sizeof(struct gpu_queue_job) == 160, "job record");

/* kick_to_done_us is the worker's clock from the kick to the observed
 * timeline update. */
struct gpu_queue_record {
    uint32_t sequence;
    uint16_t result, detail;   /* enum gpu_queue_result, enum gpu_queue_detail */
    uint32_t kick_to_done_us, reserved;
};
GPU_QUEUE_STATIC_ASSERT(sizeof(struct gpu_queue_record) == 16, "completion record");

/* Once per boot. Waits at most wait_ticks for the device to open and the
 * context to become ready; info receives one entry per lease. */
int gpu_queue_context_create(const struct gpu_queue_context_config *, uint32_t wait_ticks,
                             struct gpu_queue_context *, struct gpu_queue_lease_info *info);
/* Nonblocking. Copies the job into the context's slot and wakes the worker.
 * OK means a record follows. Checks that need no worker state run here; the
 * worker repeats them on its own copy and reports a later refusal as a
 * REFUSED record. */
int gpu_queue_submit(struct gpu_queue_context, const struct gpu_queue_job *);
/* Waits at most wait_ticks for the record; 0 polls once. The wait blocks on
 * the context's record queue, which the worker fills after the completion
 * interrupt. */
int gpu_queue_wait(struct gpu_queue_context, struct gpu_queue_record *, uint32_t wait_ticks);
/* Refuses new jobs and waits for an owed record, then closes the device.
 * Every page and lease stays retained, as with today's GPU service; the
 * context cannot be created again in this boot. */
int gpu_queue_close(struct gpu_queue_context, uint32_t wait_ticks);
#ifdef __cplusplus
}
#endif
#endif
