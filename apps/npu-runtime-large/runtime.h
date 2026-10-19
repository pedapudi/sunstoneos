#ifndef SUNSTONE_NPU_RUNTIME_LARGE_H
#define SUNSTONE_NPU_RUNTIME_LARGE_H
#include "a733_npu_application.h"
#if !defined(SUNSTONE_NPU_LARGE_GRAPH) || !defined(SUNSTONE_NPU_RUNTIME_INPUT) || !defined(SUNSTONE_NPU_INTERRUPT)
#error "Large graph requires the retained IRQ/runtime profile"
#endif
#define NPU_RUNTIME_CLOSE 0x36u
#define NPU_RUNTIME_BEGIN 0x37u
#define NPU_RUNTIME_UPLOAD 0x38u
#define NPU_RUNTIME_COMMIT 0x39u
#define NPU_RUNTIME_READ 0x3au
#define NPU_RUNTIME_FINISH 0x3bu
#define NPU_RUNTIME_FRAME UINT64_C(0x415337334e504c32)
#define NPU_RUNTIME_CHUNK 512u
#define NR_ACK_FINISH 0x100u
enum npu_runtime_state { NR_IDLE,NR_UPLOAD,NR_QUEUED,NR_RUNNING,NR_DONE,NR_CLOSING,NR_CLOSED,NR_FAILED };
struct npu_runtime_slot {
 atomic_uint state,reason;
 uint64_t id,deadline,start,end,job;
 unsigned operation,bytes,jobs,irq_count;
 uint8_t input_digest[32],output_digest[32];
};
extern struct npu_runtime_slot npu_runtime;
extern uint8_t npu_runtime_input[524288],npu_runtime_output[262144];
#define NPU_RUNTIME_INPUT npu_runtime_input
#define NPU_RUNTIME_OUTPUT npu_runtime_output
int npu_runtime_live(void);
void npu_runtime_fail(unsigned);
int npu_runtime_header(const uint8_t[64]);
int npu_runtime_payload(const uint8_t *,unsigned);
int npu_runtime_frame(uint8_t[1024],unsigned *,uint64_t *,uint64_t *);
void npu_runtime_ack(unsigned,uint64_t,uint64_t);
#endif
