#ifndef SUNSTONE_OS_DISPATCH_H
#define SUNSTONE_OS_DISPATCH_H
/* Placement of mimus linear layers on the CPU or the GPU in the integrated
 * OS image (docs/recognition-dispatch.md). The GPU hook in mimus reads one
 * row of a generated table per call; the recognizer opens the GPU session
 * once, after every model hash matched, and ends the boot on a GPU fault. */
#include <stddef.h>
#include <stdint.h>
#include "pvr_mimus_job.h"

/* One row per tensor of the merged model, indexed by the tensor's ordinal:
 * base tensors in file order with overlay replacements in place, then the
 * overlay's new tensors (apps/os/placement.py). n_gpu output channels
 * [0, n_gpu) run on the GPU in jobs of at most job_rows rows, a multiple of
 * 32 at or below the GPU worker's own bound for the tensor; n_gpu = 0 keeps
 * the tensor on the CPU. */
struct os_placement { uint16_t n_gpu, job_rows; };
/* One generated entry per row with n_gpu > 0, in ordinal order, for the
 * runtime's tensor admission: the model file (region 0 base, 1 overlay), the
 * byte offset of weight row 0 in it, the weight row stride, K and N. */
struct os_placement_tensor { uint32_t offset; uint16_t ordinal, stride, k, n; uint8_t region; };

/* The table's length, and the bitmap generated with it: bit i % 8 of byte
 * i / 8 is set when ordinal i has n_gpu > 0. The recognizer offers the
 * engine's exact-sum hook only these tensors' calls (apps/os/gpu_hook.cc). */
extern const uint32_t os_placement_rows;
extern const uint8_t os_placement_gpu_ordinals[];

/* Completion states the hook reports (GPU kernel design). */
enum { OS_DISPATCH_RESET = 1, OS_DISPATCH_LOST = 2, OS_DISPATCH_TIMEOUT = 3 };
/* Caller outcomes, distinct from the worker's actual completion record. */
enum { OS_GPU_CALL_LOST = -1, OS_GPU_CALL_TIMEOUT = -2 };
/* Why the resident supervisor stopped renewal after a GPU loss: a value
 * outside the service numbers 1..OS_SERVICES (apps/os/services.c). The next
 * boot reads it from the retained fault slot and keeps the GPU closed. */
#define OS_STOP_DETAIL_GPU_LOSS UINT64_C(0x4750554c)
/* Written only by the recognizer task, which also runs the hook. */
struct os_dispatch_record {
    uint64_t placed;      /* calls that placed channels on the GPU */
    uint64_t refused;     /* jobs refused before device access, computed on the CPU */
    uint32_t opened;      /* 1 after OPEN succeeded */
    uint32_t open_failed; /* 1 after OPEN was refused before queue acceptance */
    uint32_t failure;     /* RESET, LOST or caller TIMEOUT, before board_fail */
    uint32_t incomplete;  /* 1 after a completed job fails output validation */
};
extern struct os_dispatch_record os_dispatch_record;
/* Active table rows; read through os_dispatch_place only. */
extern uint32_t os_dispatch_active;

/* Recognizer task, once, after every model hash matched. Asks the GPU worker
 * to OPEN the session; on success every table row becomes active. A refusal
 * before queue acceptance keeps tensors on the CPU; uncertainty after
 * acceptance enters the terminal failure path. Returns 1 when opened. */
int os_dispatch_open(void);
/* Hook, per layer: the tensor's row while the session is open and thermal
 * throttling is clear, else n_gpu = 0. An admitted layer may finish its
 * remaining GPU batches; NORMAL never reopens a disabled session. */
struct os_placement os_dispatch_place(uint32_t ordinal);
/* Hook, per placed call and per job refused before device access. */
void os_dispatch_placed(void);
void os_dispatch_refused(void);
/* Hook, after an OK record has a wrong sequence, missing tag or incorrect sum:
 * records the rejection and closes dispatch admission for the boot. This is
 * bookkeeping only; the caller must complete terminal retirement before
 * removing the hook or allowing CPU fallback. */
void os_dispatch_incomplete(void);
/* Recognizer waiting for a job record or a lifecycle transition. */
void os_dispatch_waiting(int waiting);
/* Any task: OS_DISPATCH_GPU_WAITING during OPEN, CLOSE or a job-record wait,
 * so a silent recognizer means a silent GPU;
 * OS_DISPATCH_GPU_FAULT once a GPU fault ends the boot; otherwise
 * OS_DISPATCH_GPU_IDLE. */
enum { OS_DISPATCH_GPU_IDLE, OS_DISPATCH_GPU_WAITING, OS_DISPATCH_GPU_FAULT };
unsigned os_dispatch_gpu_state(void);
/* Hook, on a RESET or LOST completion and before it computes any row on the
 * CPU: records the state and ends the boot through the recognizer's failure
 * path (board_fail, then suspension retaining all ownership). Does not return. */
__attribute__((noreturn)) void os_dispatch_failed(unsigned state);

/* Provided by the recognizer (apps/os/recognition-dispatch.patch). */
__attribute__((noreturn)) void os_dispatch_stop(void);
/* The recognizer task, which the GPU runtime binds as the service's client. */
struct tskTaskControlBlock;
extern struct tskTaskControlBlock *os_recognizer;
/* The GPU runtime (apps/os/gpu.c). start, before the scheduler: prepares the
 * firmware and session memory and creates the service worker on `core`, with
 * no device access; a refusal (pool, owners, tensor admission, a GPU loss in
 * the previous-boot report) keeps OPEN refused for the boot and returns 1.
 * open: the recognizer, initially and after healthy thermal park; the worker
 * powers the GPU, starts the firmware,
 * publishes the session and formats its client ring. 1 when the session is
 * ready, 0 refused before queue acceptance, negative caller outcome on
 * retained failure. Its waits record no service progress. */
int a733_gpu_mimus_start(unsigned core);
int a733_gpu_mimus_open(void);
/* Recognizer only, between complete matmul calls after all output reads.
 * THROTTLE closes the healthy session; NORMAL reopens its retained backing.
 * 1 ready, 0 parked, -1 retained failure. No automatic idle timer or reset. */
int a733_gpu_mimus_thermal(void);
/* Recognizer only after receiving a rejected job's record, before returning
 * any output from that job; previous output must already be consumed. Close
 * admission permanently and wait for acknowledged STOPPED, retaining backing.
 * 1 permits CPU fallback; 0 requires LOST/board failure, never silent fallback. */
int a733_gpu_mimus_retire(void);
/* One job of a placed call. begin writes the slot and wakes the worker
 * without waiting: 1 submitted (finish returns its record), 0 refused before
 * any device access (the caller computes the rows), negative caller outcome
 * on retained failure. finish returns 1 only for an actual timely record;
 * a negative caller outcome leaves ownership pending. */
int a733_gpu_mimus_begin(const struct pvr_mimus_request *);
int a733_gpu_mimus_finish(struct pvr_mimus_record *);
/* The tensor admitted at start: K, N, weight stride and the CPU address of
 * weight row 0; 0 when the tensor is not on the GPU. */
struct os_gpu_tensor { uint32_t k, n, stride; const int8_t *weights; };
int a733_gpu_mimus_tensor(uint32_t ordinal, struct os_gpu_tensor *);
/* INTID 95, on the recognition core. */
void a733_gpu_mimus_irq(void);
/* The recognizer, after every model hash matched: opens the session and
 * installs the engine's exact-sum hook when the engine declares one
 * (apps/os/gpu_hook.cc); otherwise every layer stays on the CPU. */
void os_gpu_open(void);

/* Start list entry gpu-linear (SUNSTONE_A733_OS_GPU_LINEAR). */
struct a733_pin_handoff;
int os_gpu_linear_task_privileged(void (*)(void *),void *);
int os_gpu_linear_pins(struct a733_pin_handoff *);
int os_gpu_linear_start(void);

/* Recognition arena: mimus's arena and the codes in the first 12 MiB, then
 * 4 MiB mapped read-only at EL0 (and therefore at EL1). The GPU's output
 * region, 64 KiB of tags followed by the sums, is the first 2 MiB of it. The
 * last 2 MiB is not leased; it stays read-only so that a CPU store past the
 * arena faults instead of reaching memory that no owner holds. */
#define OS_DISPATCH_ARENA UINT64_C(0x46000000)
#define OS_DISPATCH_CODE_BYTES UINT64_C(0x00c00000)
#define OS_DISPATCH_READ_ONLY UINT64_C(0x46c00000)
#define OS_DISPATCH_READ_ONLY_BYTES UINT64_C(0x00400000)
#define OS_DISPATCH_OUTPUT OS_DISPATCH_READ_ONLY
#define OS_DISPATCH_OUTPUT_BYTES UINT64_C(0x00200000)
/* One tag word per GPU lane, zeroed once per boot (apps/os/identity.c). */
#define OS_DISPATCH_TAG_BYTES 65536u
/* Base and overlay model files as the loader places them; the GPU maps each
 * as one 8 MiB region. */
#define OS_DISPATCH_MODELS UINT64_C(0x44000000)
#define OS_DISPATCH_MODEL_BYTES UINT64_C(0x00800000)

/* The read-only view's check at application start, at the stride of the
 * image's tables (a733_local_pool_stride: 2 MiB where
 * platform/a733-smp-cached/memory.c maps image RAM with level-2 blocks only,
 * 4 KiB elsewhere). With a 4 KiB stride every page is checked; with a block
 * stride the first and the last 4 KiB page of each block. address and bytes
 * are multiples of the stride. Each checked page translates for an EL0 read
 * as inner-shareable write-back memory at its own address, faults for an
 * EL0 write (el0, AT S1E0R / AT S1E0W) and faults for an EL1 write (el1,
 * AT S1E1W). Checking pages, instead of negating a writable-range rule,
 * refuses a range in which only some pages are read-only. */
struct aarch64_translation;
typedef int (*os_dispatch_translate_fn)(uintptr_t, int, struct aarch64_translation *);
int aarch64_device_translate(uintptr_t, int, struct aarch64_translation *);
int aarch64_device_translate_el0(uintptr_t, int, struct aarch64_translation *);
extern const uintptr_t a733_local_pool_stride;
int os_dispatch_read_only(os_dispatch_translate_fn el0, os_dispatch_translate_fn el1, uintptr_t address,
                          size_t bytes, uintptr_t stride);
#endif
