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
/* Independent caller result: mismatch survives successful local shutdown. */
struct os_npu_result {
    atomic_uint finished;
    unsigned model,jobs,mismatches,refused,retained;
    uint8_t last_digest[32];
};
extern struct os_npu_result os_npu_result;
#endif
