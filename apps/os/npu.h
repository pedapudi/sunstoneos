#ifndef SUNSTONE_OS_NPU_H
#define SUNSTONE_OS_NPU_H
#include "npu_runtime.h"
#define OS_NPU_MODELS 5u
#define OS_NPU_CORE 1u
#define OS_NPU_POOL_START UINT64_C(0x47400000)
#define OS_NPU_POOL_BYTES UINT64_C(0x800000)
#define OS_NPU_SESSION_MAGIC UINT64_C(0x415337334f4e5031)
#define OS_NPU_SESSION_BYTES 272u
/* Fixed optional profile; the supervisor grants this exact private interval.
 * Evidence hashes bind external facts, never manufacture those facts. */
struct os_npu_session {
    uint64_t magic,version,bytes,owner,command_admission,supervisor;
    uint32_t pck_client,client_model,jobs,reserved;
    uint64_t pool_start,pool_bytes;
    uint8_t catalogue[32],admission[32],clock[32],power[32],supervision[32],ddr[32];
};
_Static_assert(sizeof(struct os_npu_session)==OS_NPU_SESSION_BYTES,"OS NPU session geometry");
extern const struct npu_runtime_model os_npu_models[OS_NPU_MODELS];
extern const uint8_t os_npu_catalogue_digest[32];
struct os_npu_vector { const uint8_t *data; size_t bytes; };
extern const struct os_npu_vector os_npu_inputs[],os_npu_references[];
extern const unsigned os_npu_client_model,os_npu_input_count,os_npu_output_count;

/* One application task, bound before scheduling. These finite calls refuse
 * every other task. 1 is operation completion, 0 is a refused call and -1 is
 * retained failure. Completion says nothing about numerical correctness.
 * LOAD selects any linked catalogue entry. Tensor windows remain owned by
 * this caller until its next LOAD, SUBMIT or CLOSE; thermal STOP cannot
 * reclaim them. SUBMIT temporarily transfers both windows to the runtime.
 * After SUBMIT returns1, outputs may be read in place. No tensor copy occurs
 * in these wrappers. The caller must stop using all pointers before CLOSE.
 * After thermal STOP, only OPEN (for accepted startup) and CLOSE remain.
 * An abandoned caller retains all resources at the fixed drain deadline. */
int os_npu_open(void);
int os_npu_load(unsigned model);
int os_npu_tensor(enum nbg_section_id,unsigned index,struct npu_runtime_tensor *);
int os_npu_submit(uint64_t timeout_us);
int os_npu_close(void);
int os_npu_stopping(void);
unsigned os_npu_stopped(void);
void os_npu_client_task(void *);
int os_npu_task_privileged(void (*)(void *),void *);
struct a733_pin_handoff;
int os_npu_pins(struct a733_pin_handoff *);
int os_npu_start(void);
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
 * mismatches. retained=1 is a runtime, transport or hardware failure that
 * kept every NPU resource. refused=1 means a thermal stop or the lifetime
 * ended the work before the selected number of jobs. */
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
/* USB0 owner: copies the published report, or the pending header. */
void os_npu_report_copy(uint8_t out[OS_NPU_REPORT_BYTES]);
#endif
