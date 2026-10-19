#ifndef SUNSTONE_NPU_CONVOLUTION_H
#define SUNSTONE_NPU_CONVOLUTION_H
#include "a733_npu_native.h"
#include "a733_npu_startup.h"
#include "nbg_assembly.h"

#define NPU_CONVOLUTION_MODEL_BYTES 1928u /* Baseline artifact only. */
#define NPU_CONVOLUTION_MODEL_CAPACITY 8192u
#define NPU_CONVOLUTION_IO_CAPACITY 512u
#define NPU_CONVOLUTION_INPUT_BYTES 192u
#define NPU_CONVOLUTION_OUTPUT_BYTES 72u
#define NPU_CONVOLUTION_COMMAND_BYTES 524288u
#define NPU_CONVOLUTION_BOOTSTRAP_BYTES 208u
enum npu_convolution_buffer {
    NPU_CONVOLUTION_NN, NPU_CONVOLUTION_COEFF, NPU_CONVOLUTION_COMMAND,
    NPU_CONVOLUTION_INPUT, NPU_CONVOLUTION_OUTPUT, NPU_CONVOLUTION_BOOTSTRAP,
    NPU_CONVOLUTION_BUFFERS
};
struct npu_convolution_config {
    const void *model;
    size_t model_bytes;
    struct a733_npu_buffer buffers[NPU_CONVOLUTION_BUFFERS];
    void *arena;
    size_t arena_bytes;
    uint64_t arena_physical;
    uintptr_t register_cpu;
    uint64_t register_physical;
    void (*fatal)(void *);
    void *fatal_cookie;
};
/* Private static storage, exposed only for reservation without a heap. All
 * allocations, context, service, source copy and startup records are trusted,
 * disjoint, pinned and inaccessible to other writers for this session. Shared
 * SMP mappings do not enforce that condition against hostile tasks. */
struct npu_convolution {
    unsigned char model[NPU_CONVOLUTION_MODEL_CAPACITY];
    size_t model_bytes, input_bytes, output_bytes;
    unsigned model_index;
    struct a733_npu_buffer buffers[NPU_CONVOLUTION_BUFFERS];
    unsigned char immutable_digest[4][32];
    struct a733_npu_native native;
    struct a733_npu_command commands[2];
    struct npu_service service;
    struct a733_npu_startup *startup;
    TaskHandle_t client;
    struct npu_service_reply observed;
    uint64_t pending, job;
    enum npu_service_operation pending_operation;
    atomic_uint failed, closed;
    unsigned prepared, bound, opened, ready, output_ready;
};

/* EL1, before scheduling, zero once. Copies then hashes the complete model;
 * accepts only the reviewed complete UINT8 compiled models in the private
 * catalogue. Select once for the context lifetime; no runtime upload or switch.
 * Six buffers have exact sizes 4K,4K,512K,4K,4K,512K and roles READ,READ,
 * COMMAND,READ,WRITE,BOOTSTRAP. CPU/PA/device ranges must be exclusive and
 * disjoint from context, source/config, tables and device registers. Native
 * initialization validates mappings; general pointer validity is a caller
 * prerequisite. No peripheral register access occurs.
 *
 * Any admitted initialization failure is terminal; reservations remain owned.
 * No teardown/reinitialization is provided. Model/weights/commands stay immutable;
 * publication checks their digests. Input/output are accessed only through the
 * client API below. Full mapped command tails contain END pairs.
 *
 * Successful preparation admits the selected trusted compiled artifact only;
 * hashes and checksums do not prove hardware access extents or confinement. Physical PD-mode
 * confinement, SRAM ownership/routing, independent recovery, remote exclusion,
 * power/clock/reset and secure attribution remain independent prerequisites. */
int npu_convolution_prepare(struct npu_convolution *,const struct npu_convolution_config *);

/* Offline composition: copy resource/evidence fields from the template and
 * replace backend, image, command table/count with the prepared private objects.
 * Requires a nonzero externally assigned command-admission evidence identity;
 * the number itself proves no hardware fact. Output must not alias owned storage.
 * Use the resulting record with a733_npu_startup_init/bind_task, then bind below. */
int npu_convolution_handoff(struct npu_convolution *,const struct a733_npu_handoff *,
    struct a733_npu_handoff *);
/* Pre-scheduler, exact authenticated startup record; one live client. This
 * constructs the deferred service with a fixed two-command catalogue. Only the
 * designated startup task may activate it through npu_service_activate. Keep
 * both tasks alive; task abandonment never releases resources. */
int npu_convolution_bind(struct npu_convolution *,struct a733_npu_startup *,
    TaskHandle_t client,UBaseType_t priority,UBaseType_t affinity);
/* Designated EL1 bootstrap task only. Use this wrapper for EVERY startup poll,
 * before service activation. Validate exact bound artifact/catalogue identities
 * before delegating any resource/power step. Mutation or retained startup stops
 * future delegation and retains every already acquired token and allocation.
 * No recovery/release is attempted. Cooperative privileged callers must not
 * bypass this wrapper through the embedded startup object. */
enum a733_npu_platform_result npu_convolution_startup_poll(struct npu_convolution *);

/* Single designated client task, finite queue waits. Only OPEN, STATUS, SUBMIT,
 * REAP and terminal CLOSE are exposed. SUBMIT copies exactly the selected input_bytes and always selects
 * command 1. Other operations require NULL/zero input. OPEN/SUBMIT require
 * finite positive microsecond deadlines; STATUS/REAP/CLOSE use zero timeout.
 *
 * CLOSE requires READY after every job has been reaped. Its copied reply must
 * report CLOSED with no session/job ownership. It permanently stops admission;
 * reopening the context is unsupported. Allocations and platform leases remain
 * owned. CLOSE alone does not restore local clocks/resets.
 *
 * Only copied replies establish READY/DONE; there are no reads of worker-owned
 * controller state. One pending request at a time. Failed collect preserves its
 * ticket and ownership; only collecting that same request may resume progress.
 * A service fault or unexpected reply permanently retains this instance.
 * A failed send before queuing may leave the private input updated while READY.
 * Return 1 means queued/collected, never completion of numerical execution.
 * Request/reply/output arguments must not overlap any reserved private storage.
 * Callers must not bypass these functions through the embedded service. */
int npu_convolution_send(struct npu_convolution *,enum npu_service_operation,
    const void *input,size_t bytes,uint64_t timeout_us,TickType_t wait,uint64_t *ticket);
int npu_convolution_collect(struct npu_convolution *,uint64_t ticket,
    struct npu_service_reply *,TickType_t wait);
/* Copy exactly output_bytes only after matching successful DONE with cache finish, before
 * explicit REAP. Refusal preserves output. Reap clears access to that result;
 * all allocations nevertheless remain reserved for the complete session. */
int npu_convolution_output(struct npu_convolution *,void *,size_t bytes);
/* Original EL1 bootstrap task only, after the client collected successful CLOSE.
 * Poll local module/bus gate and reset shutdown under the original authority.
 * READY denotes a stopped local engine, not released memory, restored PLLs,
 * powered-off domain, or ROM-return qualification. Faults retain all resources. */
enum a733_npu_platform_result npu_convolution_stop_poll(struct npu_convolution *);
#endif
