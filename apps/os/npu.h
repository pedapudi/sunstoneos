#ifndef SUNSTONE_OS_NPU_H
#define SUNSTONE_OS_NPU_H
#include "npu_runtime.h"
#include "sunstone_app.h"
/* The image links no graph (applications register them at run time). */
#ifndef OS_NPU_CORE /* the host build of the queue's open path (checks/os-npu-service.py) has one core */
#define OS_NPU_CORE 1u
#endif
#include "npu-session.h"
/* The NPU's reservation is taken from the unified memory ledger when the application is admitted (os_npu_pool_prepare, called
 * by os_large_prepare): the largest extent the NPU can address inside one 4 GiB window is reserved, the runtime places its
 * tables and regions from the start of it, and the part beyond what it placed goes back to the ledger. The extent belongs to
 * the application (its tensor and ring windows are in it) and is published for the EL3 terminal (apps/os/device-ranges.h). The
 * EL1-private metadata comes from kernel-owned ledger chunks. 1 when prepared, or when this image has no session for an NPU
 * (os_npu_start then refuses); 0 when the declaration or the memory does not fit, with the reason in the failure record. */
struct umem;
int os_npu_pool_prepare(struct umem *, uint16_t owner);
/* Check the guard extent of a placement request (apps/os/npu.c); called when the NPU client ends. */
/* The image links no graph: the application registers each one (os_npu_register). The report still names a client model,
 * always zero. */
extern const unsigned os_npu_client_model;

/* One application task, bound before scheduling. These finite calls refuse
 * every other task. 1 is operation completion, 0 is a refused call and -1 is
 * retained failure. Completion says nothing about numerical correctness.
 * LOAD selects a registered entry. Tensor windows
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
/* Which conditions of os_npu_retirement_check() are unmet, as bits (OS_NPU_STATE_*). Zero in an image without the NPU
 * service; bit 14 is always set in one, so a nonzero word says the service is linked. Reads state only. */
#define OS_NPU_STATE_NOT_PUBLISHED (1u<<0)  /* the client's report is not published */
#define OS_NPU_STATE_NOT_STOPPED_REPORT (1u<<1) /* the report does not say stopped */
#define OS_NPU_STATE_REPORT_RETAINED (1u<<2) /* the report says a failure is retained */
#define OS_NPU_STATE_NOT_STOPPED (1u<<3)    /* os_npu_stopped() is false */
#define OS_NPU_STATE_CLOSED (1u<<4)         /* the SDK close completed */
#define OS_NPU_STATE_STOPPED_FLAG (1u<<5)   /* the bootstrap's local stop completed */
#define OS_NPU_STATE_ACTIVE (1u<<6)
#define OS_NPU_STATE_STARTED (1u<<7)
#define OS_NPU_STATE_OPENED (1u<<8)
#define OS_NPU_STATE_FAILED (1u<<9)
#define OS_NPU_STATE_RUNTIME_FAILED (1u<<10)
#define OS_NPU_STATE_IRQ_PENDING (1u<<11)
#define OS_NPU_STATE_IRQ_STOPPED (1u<<12)
#define OS_NPU_STATE_PARKED (1u<<13)
#define OS_NPU_STATE_LINKED (1u<<14)
unsigned os_npu_retirement_state(void);
void os_npu_client_task(void *);
int os_npu_task_privileged(void (*)(void *),void *);
struct a733_pin_handoff;
int os_npu_pins(struct a733_pin_handoff *);
int os_npu_start(void);
struct os_npu_session;
/* The session's NPU declaration checked against what this image links and holds. NPU_DECLARATION_OK admits it; the
 * memory it reserves is checked at start. A session without a declaration is refused. */
enum npu_declaration_result os_npu_declaration_admitted(const struct os_npu_session *);
struct npu_declaration os_npu_declaration_of(const struct os_npu_session *);
/* The in-flight depth the session declares: the deepest queue the application may open. */
unsigned os_npu_in_flight(void);
unsigned os_npu_links(void);
int os_npu_never_ran(void);
void os_npu_report_unstarted(uint64_t epoch,uint64_t generation);
/* The session grants the NPU and its declaration selects the EL0 ring (os_npu_session_queued). */
int os_npu_queued(void);
/* 1 when the session grants the NPU and the application declares that it observes its jobs (OS_NPU_OBSERVE). */
int os_npu_observed(void);
struct npu_runtime *os_npu_queue_runtime(void);
int os_npu_queue_begin(void);
void os_npu_queue_end(void);
int os_npu_queue_sdk_bind(void);
int os_npu_queue_prepare_edges(struct npu_runtime *);
int os_npu_queue_tables(struct npu_runtime *);
/* Bind the queue and carve its tables: what the admission-time preparation of the NPU does for the queue (apps/os/npu.c). */
int os_npu_queue_prepare(struct npu_runtime *);
int os_npu_queue_sdk_step(void);
int os_npu_queue_sdk_closed(void);
void os_npu_queue_sdk_fail(void);
int ss_npu_queue_svc(void);
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
extern struct os_npu_report os_npu_published_report;
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
