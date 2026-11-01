#ifndef SUNSTONE_GPU_QUEUE_H
#define SUNSTONE_GPU_QUEUE_H
/* The GPU compute queue: the application interface of the OS GPU compute
 * service (docs/designs/gpu-compute-queue.md).
 *
 * Status: prototype. Nothing implements these declarations yet.
 *
 * An application creates a context, loads compiled programs into it, leases
 * buffers into the context's GPU address space and submits jobs. A job names
 * a program, binds leased buffer ranges, gives a grid of workgroups and a
 * small argument block, and carries the application's sequence number. The
 * GPU service worker writes one completion record per accepted job. Buffers
 * are used in place: the queue copies the job description, never buffer
 * contents. The queue never computes a job on the CPU; REFUSED and FAULT
 * records leave that choice to the application.
 *
 * Each context belongs to one client task, bound to a GPU service client
 * slot before the scheduler starts. Only that task calls the functions below
 * for the context. Every call runs in task context; none may be called from
 * an interrupt handler. Plain C with no FreeRTOS types, so host checks and
 * application code share these definitions. */
#include <stddef.h>
#include <stdint.h>

/* Fixed capacities. The worker reserves every context slot's firmware
 * objects at OPEN, so the slot count is fixed for the image. */
#define GPU_QUEUE_CONTEXTS 4u
#define GPU_QUEUE_PROGRAMS 4u          /* loaded programs per context */
#define GPU_QUEUE_LEASES 8u            /* leases mapped per context */
#define GPU_QUEUE_BINDINGS 8u          /* bindings per job */
#define GPU_QUEUE_ARGUMENT_BYTES 192u  /* application argument bytes per job */
#define GPU_QUEUE_DEPTH 4u             /* accepted jobs per context, queued or on the GPU */
#define GPU_QUEUE_PROGRAM_BYTES 8128u  /* USC code, two 4 KiB program pages less one cache line */
#define GPU_QUEUE_MAX_JOB_US UINT64_C(1000000)

/* Results. A call returns one of these; a completion record carries OK,
 * REFUSED or FAULT in its result field.
 * - OK: accepted (submit), completed (record) or done (other calls).
 * - BUSY: the context already holds GPU_QUEUE_DEPTH jobs; nothing changed.
 * - PARKED: the OS has closed the device (thermal throttling or image
 *   return); nothing was queued. The context survives and accepts jobs
 *   again after the OS reopens the device.
 * - REFUSED: the request was malformed or not admissible; no device access
 *   took place for it. The context stays usable.
 * - FAULT: the GPU or the service failed. Every later call on every context
 *   returns FAULT; owed records arrive with FAULT.
 * - TIMEOUT: the caller's wait ended first. A wait for a record keeps the
 *   record owed; a lifecycle call keeps its request pending, and the next
 *   lifecycle call on the context collects it. */
enum gpu_queue_result {
    GPU_QUEUE_OK, GPU_QUEUE_BUSY, GPU_QUEUE_PARKED, GPU_QUEUE_REFUSED,
    GPU_QUEUE_FAULT, GPU_QUEUE_TIMEOUT
};
/* Why a request was refused or a context faulted. */
enum gpu_queue_detail {
    GPU_QUEUE_NO_DETAIL, GPU_QUEUE_PROGRAM, GPU_QUEUE_BINDING, GPU_QUEUE_RANGE,
    GPU_QUEUE_PERMISSION, GPU_QUEUE_SEQUENCE, GPU_QUEUE_GRID, GPU_QUEUE_ARGUMENTS,
    GPU_QUEUE_DEADLINE, GPU_QUEUE_CLOSED, GPU_QUEUE_CAPACITY,
    /* FAULT details. */
    GPU_QUEUE_HANG, GPU_QUEUE_RESET, GPU_QUEUE_TIMELINE, GPU_QUEUE_MAINTENANCE,
    GPU_QUEUE_SERVICE
};

/* Opaque handles. generation changes when a slot is reused, so a stale
 * handle is refused instead of reaching another context. */
struct gpu_queue_context { uint32_t slot, generation; };

/* A program from apps/gpu-program/compile.py, compile --profile 3, as the
 * descriptor that the compiler step generates. The queue copies the code
 * into the context's program pages at load; the descriptor and code need not
 * stay alive afterwards. Programs are trusted: the queue checks the
 * descriptor's shape, never the code's memory behaviour. Source admission
 * stays an offline review step (include/pvr_compute_package.h). */
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

/* Context creation. job_us bounds each job from its kick to its timeline
 * update; a job that exceeds it faults the service. */
struct gpu_queue_context_config {
    uint64_t owner;            /* the application's lease owner identity */
    uint64_t job_us;           /* 1 .. GPU_QUEUE_MAX_JOB_US */
    uint32_t priority;         /* 0; reserved for firmware context priority */
    uint32_t reserved;
};

/* CPU access is fixed by the memory region that backs a lease, because the
 * CPU translation tables do not change after boot. GPU access is set per
 * lease in the context's GPU address space.
 * - CPU_READ_WRITE with GPU_READ: the CPU writes, the GPU reads. The queue
 *   cleans each job's declared range before the kick.
 * - CPU_READ with GPU_READ_WRITE: the GPU writes, the CPU reads. No CPU
 *   store can reach the region, so the queue cleans and invalidates each
 *   job's declared range after the timeline update without losing data.
 * - CPU_READ with GPU_READ: data nobody writes after the lease, such as
 *   model files; no maintenance.
 * - CPU_NONE: GPU-private memory; no maintenance.
 * CPU_READ_WRITE with GPU_READ_WRITE is refused: a CPU store between the
 * clean and the invalidate would be written back over GPU output. */
enum gpu_queue_cpu_access { GPU_QUEUE_CPU_NONE, GPU_QUEUE_CPU_READ, GPU_QUEUE_CPU_READ_WRITE };
enum gpu_queue_gpu_access { GPU_QUEUE_GPU_READ = 1, GPU_QUEUE_GPU_READ_WRITE = 3 };
struct gpu_queue_lease_request {
    /* Zero leases bytes from the device-memory pool of the region class that
     * cpu selects. Nonzero adopts a whole region that the application's
     * manifest declares, such as a model file the host loaded, without
     * copying it. */
    uint32_t region;
    uint32_t cpu, gpu;         /* enum gpu_queue_cpu_access, enum gpu_queue_gpu_access */
    uint32_t reserved;
    uint64_t bytes;            /* pool leases only; rounded up to the GPU page size */
};
struct gpu_queue_lease_info {
    uintptr_t cpu;             /* CPU address; NULL for CPU_NONE */
    uint64_t gpu;              /* GPU virtual address; equal to the physical address */
    uint64_t bytes;
    uint32_t page_shift;       /* 21, 16 or 12 */
    uint32_t lease;            /* index used in bindings */
};

/* One buffer range of a job. The program receives the GPU address
 * lease.gpu + address. The queue maintains the cache lines of
 * [sync_offset, sync_offset + sync_bytes) in the lease for this job, as the
 * lease's access classes require; sync_bytes 0 skips maintenance, for data
 * that has not changed since the lease was published. The range limits
 * maintenance only: the GPU MMU confines a program to the leases mapped in
 * its context, not to the ranges of its bindings. */
struct gpu_queue_binding {
    uint16_t lease;
    uint8_t access;            /* enum gpu_queue_gpu_access for this job; within the lease's */
    uint8_t reserved;
    uint32_t address;
    uint32_t sync_offset, sync_bytes;
};
_Static_assert(sizeof(struct gpu_queue_binding) == 16, "binding record");

/* One job. sequence rises strictly within the context and across device
 * close and reopen; the worker writes it to the context's timeline after the
 * work, and the record carries it back. groups is the grid in workgroups of
 * the program's local size. not_after_us, when nonzero, is the latest
 * monotonic time at which the worker may kick the job; a later job is
 * refused with GPU_QUEUE_DEADLINE.
 *
 * The program's two pointer arguments are set as follows:
 * - unified registers 0-1: the job's argument block, which holds the GPU
 *   addresses of bindings 1 .. bindings-1 as 64-bit words, followed by the
 *   argument_bytes of arguments;
 * - unified registers 2-3: the GPU address of binding 0. */
struct gpu_queue_job {
    uint32_t sequence;
    uint16_t program, bindings, argument_bytes, reserved;
    uint32_t groups[3];
    uint64_t not_after_us;
    struct gpu_queue_binding binding[GPU_QUEUE_BINDINGS];
    unsigned char arguments[GPU_QUEUE_ARGUMENT_BYTES];
};
_Static_assert(sizeof(struct gpu_queue_job) == 352, "job record");

/* One record per accepted job, in submission order. kick_to_done_us is the
 * worker's clock from the kick to the observed timeline update. */
struct gpu_queue_record {
    uint32_t sequence;
    uint16_t result, detail;   /* enum gpu_queue_result, enum gpu_queue_detail */
    uint32_t kick_to_done_us, reserved;
};
_Static_assert(sizeof(struct gpu_queue_record) == 16, "completion record");

/* Lifecycle calls go through the worker's request queue and wait at most
 * wait_ticks for its reply. Submit and wait do not use that queue. */
int gpu_queue_context_create(const struct gpu_queue_context_config *, uint32_t wait_ticks,
                             struct gpu_queue_context *);
int gpu_queue_program_load(struct gpu_queue_context, const struct gpu_queue_program *,
                           uint32_t wait_ticks, uint16_t *program);
/* Leases and maps the buffer into the context. With jobs in flight the call
 * returns BUSY: the worker changes a context's GPU tables only while the
 * context is idle. */
int gpu_queue_lease(struct gpu_queue_context, const struct gpu_queue_lease_request *,
                    uint32_t wait_ticks, struct gpu_queue_lease_info *);
/* Unmaps the lease and returns it to its pool after the GPU MMU caches were
 * flushed. BUSY while the context has jobs in flight. */
int gpu_queue_release(struct gpu_queue_context, uint32_t lease, uint32_t wait_ticks);
/* Nonblocking. Copies the job into the context's submission ring and wakes
 * the worker. OK means a record follows. Admission checks that need no
 * worker state run here and return REFUSED at once; the worker repeats them
 * on its own copy, and a later refusal arrives as a REFUSED record. */
int gpu_queue_submit(struct gpu_queue_context, const struct gpu_queue_job *);
/* Waits at most wait_ticks for the next record; 0 polls once. The wait
 * blocks on the context's record queue, which the worker fills from the
 * completion interrupt. */
int gpu_queue_wait(struct gpu_queue_context, struct gpu_queue_record *, uint32_t wait_ticks);
/* Refuses new jobs, waits for the records of accepted ones, removes the
 * firmware context, unmaps and returns every lease. */
int gpu_queue_close(struct gpu_queue_context, uint32_t wait_ticks);
#endif
