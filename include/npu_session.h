#ifndef SUNSTONE_NPU_SESSION_H
#define SUNSTONE_NPU_SESSION_H
#include "a733_npu_native.h"
#include "a733_npu_startup.h"
#include "nbg_assembly.h"

/* Several trusted compiled models sharing the one NPU through the shared
 * service mode. Each model has its own client task, allocations, command
 * stream and session group; the controller runs their jobs one at a time in
 * round-robin order. Models come from the privileged build-time catalogue
 * in drivers/npu/npu_session_models.inc; a caller hash cannot add one. */
#define NPU_SESSION_MODELS 4u
#define NPU_SESSION_DEVICE_BASE 0x10000000u
#define NPU_SESSION_BOOTSTRAP_BYTES 524288u

struct npu_session_model { const void *data; size_t bytes; };
/* One pinned parent allocation of exactly npu_session_requirements bytes.
 * It holds the private model copies, every NPU allocation and the NPU page
 * tables, so its size follows the selected models instead of a fixed window. */
struct npu_session_config {
    struct npu_session_model models[NPU_SESSION_MODELS];
    unsigned count;
    uintptr_t cpu;
    uint64_t physical;
    size_t bytes;
    uintptr_t register_cpu;
    uint64_t register_physical;
    void (*fatal)(void *);
    void *fatal_cookie;
};
/* Private per-model state; application code must not modify it. Buffers
 * follow the general assembly plan order; an empty working pool is zero. */
struct npu_session {
    const unsigned char *model;
    size_t model_bytes;
    unsigned entry;
    struct nbg_assembly_layout layout;
    struct a733_npu_buffer buffers[NBG_LAYOUT_MAX_BUFFERS];
    unsigned char constant_digest[2][32], stream_digest[32];
    size_t stream_bytes;
    TaskHandle_t client;
    struct npu_service_reply observed;
    uint64_t pending, job;
    enum npu_service_operation pending_operation;
    unsigned opened, ready, output_ready;
    atomic_uint closed;
};
/* Private static storage, public only for reservation without a heap. */
struct npu_sessions {
    struct npu_session session[NPU_SESSION_MODELS];
    unsigned count;
    uintptr_t parent_cpu;
    size_t parent_bytes;
    struct a733_npu_buffer bootstrap;
    unsigned char bootstrap_digest[32];
    struct a733_npu_native native;
    struct a733_npu_command commands[NPU_SESSION_MODELS + 1];
    struct npu_service service;
    struct a733_npu_startup *startup;
    atomic_uint failed;
    unsigned prepared, bound;
};

/* Size of the parent allocation for these models, in bytes, or 0 when any
 * model is absent from the catalogue or refused by the decoder, assembler or
 * the 64-buffer memory policy. Reads the caller's bytes; no other effect. */
size_t npu_session_requirements(const struct npu_session_model *, unsigned count);

/* EL1, before scheduling, zero once. Copies every model into the parent
 * allocation, authenticates each copy against the catalogue, then decodes,
 * plans and assembles it from the copy. Allocations are placed in catalogue
 * order after one bootstrap stream: descriptors, coefficients, working pool,
 * command stream, inputs, then outputs, each 4 KiB aligned. Device addresses
 * start at NPU_SESSION_DEVICE_BASE with one unmapped page after every
 * allocation, so an overrun faults in the NPU MMU instead of reaching a
 * neighbor. The NPU page tables follow the payload and grow by 16 KiB for
 * every 16 MiB of device addresses. Each model's buffers form session group
 * model+1 in the memory policy. Descriptors and coefficients are CONSTANT.
 *
 * Integrity follows the single-model registration: the bootstrap publication,
 * activation, startup and stop check every model copy, every descriptor and
 * coefficient digest and every command stream; a job publication checks the
 * submitting model's descriptors, coefficients and command stream. Any
 * failure after admission is terminal and retains the parent allocation.
 * Hashes do not prove device access extents; physical confinement, SRAM
 * ownership, power, clocks and recovery remain independent prerequisites. */
int npu_session_prepare(struct npu_sessions *, const struct npu_session_config *);
/* As npu_convolution_handoff: copy the template and install the private
 * backend, MMU image and command table (bootstrap, then one per model). */
int npu_session_handoff(struct npu_sessions *, const struct a733_npu_handoff *,
                        struct a733_npu_handoff *);
/* Pre-scheduler. clients[i] is the only task that may use model i; tasks are
 * distinct and none is the startup task. Constructs the deferred service in
 * shared mode with one command per model. */
int npu_session_bind(struct npu_sessions *, struct a733_npu_startup *,
                     const TaskHandle_t clients[], UBaseType_t priority, UBaseType_t affinity);
/* Optional, after bind and before scheduling: interrupt completion through
 * npu_service_bind_irq on the embedded service. */
int npu_session_bind_irq(struct npu_sessions *, const struct npu_service_irq_ops *, void *cookie,
                         UBaseType_t route);
/* Designated EL1 startup task only, as npu_convolution_startup_poll. */
enum a733_npu_platform_result npu_session_startup_poll(struct npu_sessions *);

/* Model metadata for clients: decoded tensor descriptors from the private
 * copy. section is NBG_INPUT or NBG_OUTPUT. Returns 1 on success. */
int npu_session_tensor(const struct npu_sessions *, unsigned model, enum nbg_section_id section,
                       unsigned index, struct nbg_tensor *);

/* The model's client task only; same protocol as npu_convolution_send and
 * collect, with the member views of the shared service. SUBMIT copies one
 * span per input tensor, each exactly that tensor's logical size, and clears
 * the model's working pool first while the model has no job. Its reply
 * arrives once the job is queued; WAIT holds until the job is DONE. Other
 * operations take no inputs. CANCEL succeeds only for a job that has not
 * started. CLOSE ends this model's session permanently; the last model's
 * CLOSE closes the controller. A service fault or unexpected reply retains
 * every model. */
int npu_session_send(struct npu_sessions *, unsigned model, enum npu_service_operation,
                     const struct nbg_span *inputs, unsigned count, uint64_t timeout_us,
                     TickType_t wait, uint64_t *ticket);
int npu_session_collect(struct npu_sessions *, unsigned model, uint64_t ticket,
                        struct npu_service_reply *, TickType_t wait);
/* Copy one output tensor, exactly its logical size, after the model's DONE
 * and before its REAP. */
int npu_session_output(struct npu_sessions *, unsigned model, unsigned index, void *, size_t);
/* Original startup task, after every model collected a successful CLOSE. */
enum a733_npu_platform_result npu_session_stop_poll(struct npu_sessions *);
#endif
