#ifndef SUNSTONE_OS_NPU_H
#define SUNSTONE_OS_NPU_H
#include "npu_runtime.h"
#include "sunstone_app.h"
/* The immutable linked catalogue (apps/os/npu-catalogue.py). */
#define OS_NPU_CORE 1u
/* The pool is sized from the application's declaration (cmake SUNSTONE_OS_NPU_POOL_BYTES, docs/designs/npu-resources.md,
 * "The pool"): 8 MiB at 0x47400000 as always, or a larger multiple of 2 MiB that ends at 0x47000000, the bottom of the
 * monitor interval, so that it stays inside the write-back RAM that EL0 reaches and below the application regions.
 * A pool that the build places in the slot 1 area (cmake SUNSTONE_OS_NPU_POOL_BASE, OS_NPU_POOL_BASE here) may be larger:
 * the ledger holds it for the kernel at admission, so no application region, heap or stack can take it and no
 * application space maps it (docs/designs/application-memory-layout.md, step 5). */
#ifndef OS_NPU_POOL_BYTES
#define OS_NPU_POOL_BYTES UINT64_C(0x800000)
#endif
#define OS_NPU_POOL_LEGACY_BYTES UINT64_C(0x800000)
#ifdef OS_NPU_POOL_BASE
#define OS_NPU_POOL_START ((uint64_t)OS_NPU_POOL_BASE)
_Static_assert((uint64_t)OS_NPU_POOL_BASE>=UINT64_C(0x48000000)&&(uint64_t)OS_NPU_POOL_BASE+OS_NPU_POOL_BYTES<=UINT64_C(0x79000000)&&
               !((uint64_t)OS_NPU_POOL_BASE&(UINT64_C(0x200000)-1))&&OS_NPU_POOL_BYTES<=UINT64_C(0x30000000)&&
               OS_NPU_POOL_BYTES>=UINT64_C(0xa00000)&&!(OS_NPU_POOL_BYTES&(UINT64_C(0x200000)-1)),
               "a ledger NPU pool is 10 MiB to 768 MiB of whole 2 MiB blocks inside the slot 1 area");
#else
#define OS_NPU_POOL_START (OS_NPU_POOL_BYTES==OS_NPU_POOL_LEGACY_BYTES?UINT64_C(0x47400000):UINT64_C(0x47000000)-OS_NPU_POOL_BYTES)
_Static_assert(OS_NPU_POOL_BYTES>=OS_NPU_POOL_LEGACY_BYTES&&OS_NPU_POOL_BYTES<=UINT64_C(0x3000000)&&
               !(OS_NPU_POOL_BYTES&(UINT64_C(0x200000)-1)),"the NPU pool is 8 to 48 MiB in blocks of 2 MiB");
#endif
#include "npu-session.h"
extern const struct npu_runtime_model os_npu_models[];
extern const unsigned os_npu_model_count,os_npu_client_entry;
extern const struct ss_npu_model os_npu_model_info[];
extern const uint8_t os_npu_catalogue_digest[32];
struct os_npu_vector { const uint8_t *data; size_t bytes; };
extern const struct os_npu_vector os_npu_inputs[],os_npu_references[];
extern const unsigned os_npu_client_model,os_npu_input_count,os_npu_output_count;

/* One application task, bound before scheduling. These finite calls refuse
 * every other task. 1 is operation completion, 0 is a refused call and -1 is
 * retained failure. Completion says nothing about numerical correctness.
 * LOAD selects a linked entry; os_npu_client_entry identifies the diagnostic
 * model, whose source-list index is os_npu_client_model. Tensor windows
 * remain owned by this caller until its next LOAD, SUBMIT or CLOSE; thermal STOP cannot
 * reclaim them. SUBMIT temporarily transfers both windows to the runtime.
 * After SUBMIT returns1, outputs may be read in place. No tensor copy occurs
 * in these wrappers. The caller must stop using all pointers before CLOSE.
 * After thermal STOP, only OPEN (for accepted startup) and CLOSE remain.
 * An abandoned caller retains all resources at the fixed drain deadline. */
int os_npu_open(void);
/* Add a graph after start: 1 and its entry for LOAD, or 0 (not admissible, beyond the declared bounds or capacity). The caller
 * keeps the bytes alive and unchanged; LOAD refuses a copy that differs from what this admitted. */
int os_npu_register(const void *model,size_t bytes,unsigned *entry);
int os_npu_load(unsigned entry);
int os_npu_tensor(enum nbg_section_id,unsigned index,struct npu_runtime_tensor *);
int os_npu_submit(uint64_t timeout_us);
/* os_npu_submit returns 2 when the job failed with an NPU MMU exception and the NPU was recovered: the session is
 * usable, the outputs are not valid, and this record describes the fault (zero fields where a build lacks them). */
struct os_npu_fault_info { uint32_t mmu_status, mmu_address, canary_crc, marks, window_ms, reserved; };
extern struct os_npu_fault_info os_npu_last_fault;
int os_npu_close(void);
int os_npu_stopping(void);
unsigned os_npu_stopped(void);
/* Caller progress must remain within the finite NPU work deadline. */
int os_npu_progress(void);
/* Published finite result with local stop complete and no retained failure. */
int os_npu_complete(void);
/* Task context only: preserve a late atomic failure before generic cleanup. */
int os_npu_retirement_check(void);
void os_npu_client_task(void *);
int os_npu_task_privileged(void (*)(void *),void *);
struct a733_pin_handoff;
int os_npu_pins(struct a733_pin_handoff *);
int os_npu_start(void);
struct os_npu_session;
/* The session's NPU declaration checked against what this image links and holds. NPU_DECLARATION_OK admits it; the
 * memory it reserves is checked at start. A session without a declaration is refused. */
enum npu_declaration_result os_npu_declaration_admitted(const struct os_npu_session *);
/* The in-flight depth the session declares: the deepest queue the application may open. */
unsigned os_npu_in_flight(void);
unsigned os_npu_links(void);
int os_npu_never_ran(void);
void os_npu_report_unstarted(uint64_t epoch,uint64_t generation);
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
struct npu_runtime *os_npu_queue_runtime(void);
int os_npu_queue_begin(void);
void os_npu_queue_end(void);
int os_npu_queue_sdk_bind(void);
int os_npu_queue_prepare_edges(struct npu_runtime *);
int os_npu_queue_sdk_step(void);
int os_npu_queue_sdk_closed(void);
void os_npu_queue_sdk_fail(void);
int ss_npu_queue_svc(void);
#endif
void os_npu_irq(void);
int os_npu_irq_bind(struct npu_runtime *);
/* Existing OS thermal owner only. Freshness is the minimum of all five
 * channel timestamps+1s. STOP is terminal; its first deadline is immutable. */
void os_npu_thermal(int allow,int stop,uint64_t until,uint64_t now);
/* SUBMIT output fill: the word of NPU_CONVOLUTION_OUTPUT_POISON
 * (include/npu_convolution.h), little-endian and repeated from each output
 * tensor's first byte. The client writes it before SUBMIT; the runtime cleans
 * and invalidates the output lines before the job, so a byte that the job does
 * not write still holds its pattern byte afterwards. */
#define OS_NPU_OUTPUT_PATTERN UINT32_C(0xc35a3ca5)
/* USB0 operation 6 returns this report alone, without the 64-byte frame
 * header. The client fills it once when it finishes and then publishes
 * finished=1 with release ordering; nothing writes it afterwards. Before
 * that, operation 6 returns the header and session identities with
 * finished=0 and every other field zero. A completed job counts in jobs;
 * a job whose outputs differ from the linked reference also counts in
 * mismatches. For an SDK application, jobs counts successful output copies
 * and output_bytes is the last copy's extent. The application checks numerical
 * results; mismatches, pattern_bytes, pattern and last_digest remain zero.
 * Model always identifies the session's diagnostic selection, rather than
 * generic SDK request history. retained=1 is a runtime, transport or hardware failure, or the
 * end of the lifetime, that kept every NPU resource; the board has failed and
 * USB0 sends no further frame, so that report is readable only from RAM.
 * refused=1 means a thermal stop, including one for stale thermal data,
 * ended the work before the selected number of jobs. */
/* finished is 1 for a report that the client filled and 2 for the report of a
 * failure return before the service ran (os_npu_never_ran): nothing was
 * powered or submitted, so stopped=1, retained=0 and every other field except
 * the header and session identities is zero. The failure return publishes it
 * with the failure record's epoch and generation. A host accepts finished=2
 * only together with a failure return (apps/os/cpu-return-host.py). */
#define OS_NPU_REPORT_NOT_STARTED 2u
#define OS_NPU_REPORT_OP 6u
#define OS_NPU_REPORT_MAGIC UINT32_C(0x5255504e) /* "NPUR" */
#define OS_NPU_REPORT_BYTES 104u
struct os_npu_report {
    uint32_t magic,version,bytes;
    atomic_uint finished;
    uint64_t epoch,generation;
    uint32_t model,jobs_selected,jobs,mismatches,refused,retained,stopped;
    uint32_t output_bytes,pattern_bytes,pattern; /* of the last completed job */
    uint8_t last_digest[32]; /* SHA-256 of the last completed job's outputs */
};
_Static_assert(sizeof(struct os_npu_report)==OS_NPU_REPORT_BYTES,"OS NPU report geometry");
/* First local NPU failure, retained without further device operations. Ready
 * is 0 empty, 1 being copied, 2 published. The RTC first-failure latch remains
 * authoritative when an earlier device/provider failure already exists. */
struct os_npu_failure {
    atomic_uint ready;
    uint32_t site,operation,model,reply_valid;
    uint64_t ticket,expected,observed;
    uintptr_t caller;
    struct npu_service_reply reply;
    unsigned slot_state,runtime_failed,irq_pending,irq_stopped,watch_valid;
    uint64_t now,end,gate;
};
extern struct os_npu_failure os_npu_first_failure;
/* USB0 owner: copies the published report, or the pending header. */
void os_npu_report_copy(uint8_t out[OS_NPU_REPORT_BYTES]);
#ifdef SUNSTONE_A733_OS_PERF
/* Performance modes (docs/designs/performance-modes.md). The NPU's rate
 * changes only after its session has stopped, on the bootstrap task.
 * ready: 1 once the startup reached READY and the NPU has not failed.
 * stopped: ready and the local stop complete. retune: posts request number
 * sequence (1 to 65535) for 492 or 852 MHz and wakes the bootstrap; 1 posted.
 * answer: for that number, 0 while pending, 1 done, 2 refused with nothing
 * written, 3 failed (the NPU then fails the board). */
int os_npu_perf_ready(void);
int os_npu_perf_stopped(void);
/* Thermal task only; one outstanding request until the bootstrap answers. */
int os_npu_perf_retune(unsigned mhz,unsigned sequence,uint64_t deadline_us);
unsigned os_npu_perf_answer(unsigned sequence);
#endif
#endif
