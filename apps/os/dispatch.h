#ifndef SUNSTONE_OS_DISPATCH_H
#define SUNSTONE_OS_DISPATCH_H
/* Placement of mimus linear layers on the CPU or the GPU in the integrated
 * OS image (docs/recognition-dispatch.md). The GPU hook in mimus reads one
 * row of a generated table per call; the recognizer opens the GPU session
 * once, after every model hash matched, and ends the boot on a GPU fault. */
#include <stddef.h>
#include <stdint.h>

/* One row per tensor of the merged model, indexed by the tensor's ordinal:
 * base tensors in file order with overlay replacements in place, then the
 * overlay's new tensors (apps/os/placement.py). n_gpu output channels
 * [0, n_gpu) run on the GPU in jobs of at most job_rows rows; n_gpu = 0 keeps
 * the tensor on the CPU. */
struct os_placement { uint16_t n_gpu, job_rows; };

/* Completion states the hook reports (GPU kernel design). */
enum { OS_DISPATCH_RESET = 1, OS_DISPATCH_LOST = 2 };
/* Written only by the recognizer task, which also runs the hook. */
struct os_dispatch_record {
    uint64_t placed;      /* calls that placed channels on the GPU */
    uint64_t refused;     /* jobs refused before device access, computed on the CPU */
    uint32_t opened;      /* 1 after OPEN succeeded */
    uint32_t open_failed; /* 1 after OPEN was refused or failed */
    uint32_t failure;     /* OS_DISPATCH_RESET or OS_DISPATCH_LOST, recorded before board_fail */
    uint32_t reserved;
};
extern struct os_dispatch_record os_dispatch_record;
/* Active table rows; read through os_dispatch_place only. */
extern uint32_t os_dispatch_active;

/* Recognizer task, once, after every model hash matched. Asks the GPU worker
 * to OPEN the session; on success every table row becomes active, otherwise
 * every tensor stays on the CPU for the boot. Returns 1 when opened. */
int os_dispatch_open(void);
/* Hook, per call: the tensor's row while the session is open, else a row
 * with n_gpu = 0. Two loads (the active count and the row) and one compare. */
struct os_placement os_dispatch_place(uint32_t ordinal);
/* Hook, per placed call and per job refused before device access. */
void os_dispatch_placed(void);
void os_dispatch_refused(void);
/* Hook, on a RESET or LOST completion and before it computes any row on the
 * CPU: records the state and ends the boot through the recognizer's failure
 * path (board_fail, then the watchdog reset). Does not return. */
__attribute__((noreturn)) void os_dispatch_failed(unsigned state);

/* Provided by the recognizer (apps/os/recognition-dispatch.patch). */
__attribute__((noreturn)) void os_dispatch_stop(void);
/* Provided by the GPU runtime: OPEN leases the model regions, the 12 MiB code
 * span and the two sum slots, and cleans the model regions and slots once.
 * Blocks until the worker replies; 1 on success. */
int a733_gpu_mimus_open(void);
/* GPU runtime: the EL1 worker task, created by a733_gpu_mimus_start before
 * the scheduler on the recognition core, and the INTID 95 handler. */
void a733_gpu_mimus_worker(void *);
int a733_gpu_mimus_start(unsigned core);
void a733_gpu_mimus_irq(void);

/* Start list entry gpu-linear (SUNSTONE_A733_OS_GPU_LINEAR). */
struct a733_pin_handoff;
int os_gpu_linear_task_privileged(void (*)(void *),void *);
int os_gpu_linear_pins(struct a733_pin_handoff *);
int os_gpu_linear_start(void);

/* Recognition arena: codes in the first 12 MiB, then the GPU's two 2 MiB sum
 * slots, mapped read-only at EL0 (and therefore at EL1). */
#define OS_DISPATCH_ARENA UINT64_C(0x46000000)
#define OS_DISPATCH_CODE_BYTES UINT64_C(0x00c00000)
#define OS_DISPATCH_SLOTS UINT64_C(0x46c00000)
#define OS_DISPATCH_SLOT_BYTES UINT64_C(0x00400000)

/* Each 4 KiB page of [address, address + bytes) must translate for an EL0
 * read and fault for an EL0 write. Checking every page, instead of negating a
 * writable-range rule, refuses a range in which only some pages are
 * read-only. translate is AT S1E0R / AT S1E0W through PAR_EL1, with the
 * signature of aarch64_device_translate. */
struct aarch64_translation;
typedef int (*os_dispatch_translate_fn)(uintptr_t, int, struct aarch64_translation *);
int os_dispatch_el0_read_only(os_dispatch_translate_fn translate, uintptr_t address, size_t bytes);
#endif
