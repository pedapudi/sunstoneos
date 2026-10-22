#ifndef SUNSTONE_NPU_RUNTIME_H
#define SUNSTONE_NPU_RUNTIME_H
#include "a733_npu_native.h"
#include "a733_npu_startup.h"
#include "a733_storage_digest.h"
#include "nbg_assembly.h"

/* Compiled models loaded into fixed slots at runtime and run concurrently on
 * the one NPU (docs/designs/npu-breadth.md). Up to eight client tasks each
 * own one slot. A slot holds one catalogue model at a time; its client loads
 * a model by catalogue index, writes inputs in place, submits, and reads
 * outputs in place. Jobs of different slots share the NPU with round-robin
 * starts. The catalogue is fixed at preparation: every entry must match the
 * privileged build-time digest table in drivers/npu/npu_session_models.inc. */
#define NPU_RUNTIME_SLOTS NPU_SERVICE_CLIENTS
#define NPU_RUNTIME_MODELS 16u
#define NPU_RUNTIME_DEVICE_BASE 0x10000000u
#define NPU_RUNTIME_COMMAND_BYTES 524288u
#define NPU_RUNTIME_GROUP_BYTES 0x200000u
/* Bulk work runs in steps sized to fit 80 us at the design's worst-case rates
 * at 494 MHz: copied, hashed and checked LOAD chunks with the portable
 * SHA-256; streaming CRC32C and END compares; cache maintenance, END filling
 * and pool zeroing with its clean and invalidate; assembled operations. */
#define NPU_RUNTIME_LOAD_CHUNK 1024u
#define NPU_RUNTIME_READ_CHUNK 32768u
#define NPU_RUNTIME_CACHE_CHUNK 16384u
#define NPU_RUNTIME_ASSEMBLY_STEP 8u

struct npu_runtime_model { const void *data; size_t bytes; };
/* One parent allocation of exactly npu_runtime_requirements bytes, 2 MiB
 * aligned in CPU and physical addresses, write-back cached and mapped at EL1.
 * The catalogue models stay outside it, live and unchanged for the runtime's
 * lifetime; LOAD authenticates each copy it makes. */
struct npu_runtime_config {
    struct npu_runtime_model models[NPU_RUNTIME_MODELS];
    unsigned count, slots;
    uintptr_t cpu;
    uint64_t physical;
    size_t bytes;
    uintptr_t register_cpu;
    uint64_t register_physical;
    void (*fatal)(void *);
    void *fatal_cookie;
};
/* Slot states, written only by the worker and read by clients with acquire
 * ordering. A client may write its inputs and read its outputs only while its
 * slot is READY and it has no request pending. */
enum npu_runtime_state {
    NPU_RUNTIME_EMPTY, NPU_RUNTIME_LOADING, NPU_RUNTIME_READY, NPU_RUNTIME_QUEUED,
    NPU_RUNTIME_PREPARED, NPU_RUNTIME_RUNNING, NPU_RUNTIME_FINISHED, NPU_RUNTIME_CHECKING,
    NPU_RUNTIME_CLOSED
};
/* One tensor of the loaded model: its decoded descriptor and its bytes in
 * the slot's read window (inputs) or write window (outputs). */
struct npu_runtime_tensor {
    void *data;
    size_t bytes;
    struct nbg_tensor tensor;
};
/* Worker-written counts for host models and board diagnostics. */
enum npu_runtime_step {
    NPU_RUNTIME_STEP_LOAD, NPU_RUNTIME_STEP_PREPARE, NPU_RUNTIME_STEP_POOL,
    NPU_RUNTIME_STEP_START, NPU_RUNTIME_STEP_CHECK, NPU_RUNTIME_STEP_KINDS
};
struct npu_runtime_counters {
    /* longest: the longest step of each kind in microseconds of the native
     * counter; the start step includes G and the trigger, not the LOAD step
     * that follows it. */
    uint64_t steps[NPU_RUNTIME_STEP_KINDS], longest[NPU_RUNTIME_STEP_KINDS], loads, refused_loads, jobs;
    uint64_t table_bytes, address_bytes, tail_bytes, readonly_bytes, reference_bytes, staging_check_bytes;
    uint64_t zeroed_bytes, copied_bytes, cache_lines, digest_bytes;
    /* NPU register accesses through the backend; on the board each one also
     * makes one AT translation in the native adapter's mapping check. */
    uint64_t register_reads, register_writes;
};

/* Private storage, public only so the platform can reserve it without a heap.
 * Application code must not modify these fields. */
struct npu_runtime_region { uintptr_t cpu; uint64_t physical; size_t bytes; uint32_t device; };
struct npu_runtime_span { uintptr_t cpu; size_t bytes; };
struct npu_runtime_slot {
    _Alignas(64) atomic_uint state;
    unsigned model, phase, span;
    size_t offset;
    struct npu_runtime_region command, constant, read, write;
    struct nbg_assembly_buffer targets[NBG_LAYOUT_MAX_BUFFERS];
    struct nbg_assembly_layout layout;
    struct npu_runtime_tensor tensors[2 * NBG_ASSEMBLY_MAX_TENSORS];
    uint32_t stream_bytes, end_mark, address_crc, readonly_crc, crc;
    uint64_t timeout_us, finished;
    /* Client-written fields stay on their own cache lines. */
    _Alignas(64) TaskHandle_t client;
    uint64_t pending;
    enum npu_service_operation pending_operation;
    unsigned opened, closed;
};
struct npu_runtime {
    struct npu_runtime_slot slot[NPU_RUNTIME_SLOTS];
    struct npu_runtime_model catalogue[NPU_RUNTIME_MODELS];
    struct nbg_assembly_layout plans[NPU_RUNTIME_MODELS];
    unsigned entry[NPU_RUNTIME_MODELS];
    unsigned count, slots;
    size_t constant_bytes, read_bytes, write_bytes, staging_bytes;
    uintptr_t parent_cpu, staging;
    size_t parent_bytes;
    struct a733_npu_buffer bootstrap;
    uint32_t table_crc, bootstrap_crc;
    struct a733_npu_native native;
    struct a733_npu_command commands[NPU_RUNTIME_SLOTS + 1];
    struct npu_service service;
    struct a733_npu_startup *startup;
    /* Worker-private LOAD state: one load runs at a time. */
    struct storage_digest digest;
    uint32_t staging_crc;
    struct nbg_model view;
    struct nbg_assembly_cursor cursor;
    unsigned loading, checking, last_started;
    uint64_t finishes;
    struct npu_runtime_counters counters;
    atomic_uint failed;
    unsigned prepared, bound;
};

/* Size of the parent allocation for slots slots and these catalogue models,
 * or 0 when a model is absent from the build-time catalogue, refused by the
 * decoder or assembler, or needs a command allocation larger than
 * NPU_RUNTIME_COMMAND_BYTES. Every slot gets the largest read-only region,
 * read window and write window of any model, so any slot can load any model.
 * Reads the models; no other effect. */
size_t npu_runtime_requirements(const struct npu_runtime_model *, unsigned count, unsigned slots);

/* EL1, before scheduling, zero storage once. Authenticates and plans every
 * catalogue model, places the three groups (read-only regions with the NPU
 * table arena, bootstrap stream and load staging; read windows; write
 * windows), each starting and ending on a 2 MiB boundary, builds the NPU page
 * tables with 4 KiB entries and one unmapped page after every allocation,
 * fills every command allocation with END pairs and records the table and
 * bootstrap checksums. Refuses a DCZID_EL0 that prohibits DC ZVA or reports
 * a block other than 64 bytes. Slots start EMPTY. Any failure after
 * admission is terminal and retains the parent allocation. */
int npu_runtime_prepare(struct npu_runtime *, const struct npu_runtime_config *);
/* As npu_session_handoff: copy the template and install the private backend,
 * MMU image and command table (bootstrap, then one command per slot). */
int npu_runtime_handoff(struct npu_runtime *, const struct a733_npu_handoff *,
                        struct a733_npu_handoff *);
/* Pre-scheduler. clients[i] is the only task that may use slot i; tasks are
 * distinct and none is the startup task. Constructs the deferred service in
 * runtime mode with one command per slot. */
int npu_runtime_bind(struct npu_runtime *, struct a733_npu_startup *, const TaskHandle_t clients[],
                     UBaseType_t priority, UBaseType_t affinity);
/* Required, after bind and before scheduling: interrupt completion through
 * npu_service_bind_irq on the embedded service. Activation is refused
 * without it. idle_hint 1 keeps the idle-register hint until board evidence
 * qualifies INTID 97 delivery. */
int npu_runtime_bind_irq(struct npu_runtime *, const struct npu_service_irq_ops *, void *cookie,
                         UBaseType_t route, unsigned idle_hint);
/* Designated EL1 startup task only, as npu_session_startup_poll and
 * npu_session_stop_poll. The stop poll requires every slot's successful CLOSE. */
enum a733_npu_platform_result npu_runtime_startup_poll(struct npu_runtime *);
enum a733_npu_platform_result npu_runtime_stop_poll(struct npu_runtime *);

/* Slot client only. OPEN joins the shared controller (timeout required);
 * LOAD replaces the slot's model with catalogue entry model and replies once
 * the slot is READY or the load is refused, which leaves it EMPTY; SUBMIT
 * (timeout required, the job deadline) runs the loaded model on the inputs
 * already written in place and replies once the job is DONE and its check
 * passed, or FAULT; CLOSE leaves the controller and ends the slot. Between
 * SUBMIT and its reply the read and write windows belong to the device. One
 * request may be pending; waits must be finite. A FAULT reply, a mismatch in
 * an interim integrity check or a controller fault retains every slot. */
int npu_runtime_send(struct npu_runtime *, unsigned slot, enum npu_service_operation, unsigned model,
                     uint64_t timeout_us, TickType_t wait, uint64_t *ticket);
int npu_runtime_collect(struct npu_runtime *, unsigned slot, uint64_t ticket,
                        struct npu_service_reply *, TickType_t wait);
/* Slot client only, while the slot is READY and no request is pending:
 * section is NBG_INPUT or NBG_OUTPUT. Returns 1 on success. */
int npu_runtime_tensor(const struct npu_runtime *, unsigned slot, enum nbg_section_id, unsigned index,
                       struct npu_runtime_tensor *);

/* Interim integrity checks, in drivers/npu/a733_npu_runtime_interim.c until
 * the fixed read-only mappings exist. Worker or preparation only. */
uint32_t npu_runtime_crc32c(uint32_t crc, const void *, size_t);
/* T: every NPU table word against the checksum recorded at preparation. */
int npu_runtime_tables_intact(struct npu_runtime *);
/* A, before each trigger: the slot's address-bearing bytes (NN descriptors
 * and command stream) and every END pair after the stream up to the fetch end. */
int npu_runtime_stream_intact(struct npu_runtime *, struct npu_runtime_slot *);
/* LOAD: one step of the CRC references for A and H, over the bytes the load
 * wrote; the caller starts the slot's cursor with crc UINT32_MAX. Returns 1
 * once both are recorded. */
int npu_runtime_reference_step(struct npu_runtime *, struct npu_runtime_slot *);
/* H, after each job: one step over the slot's read-only bytes with the same
 * cursor rule. Returns 0 while bytes remain, then 1 on a match, -1 otherwise. */
int npu_runtime_check_step(struct npu_runtime *, struct npu_runtime_slot *);
#endif
