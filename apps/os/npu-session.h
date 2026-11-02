#ifndef SUNSTONE_OS_NPU_SESSION_H
#define SUNSTONE_OS_NPU_SESSION_H
#include <stdint.h>
#define OS_NPU_SESSION_MAGIC UINT64_C(0x415337334f4e5031)
#define OS_NPU_SESSION_BYTES 288u
/* Version 2 adds the application's declaration of NPU resources, four full-width fields (docs/designs/npu-resources.md).
 * Version 1 (272 bytes, no declaration) is no longer admitted. */
#define OS_NPU_SESSION_VERSION 2u
/* Fixed optional profile; the supervisor grants this exact private interval.
 * Evidence hashes bind external facts, never manufacture those facts. */
struct os_npu_session {
    uint64_t magic,version,bytes,owner,command_admission,supervisor;
    /* jobs: the workload of the diagnostic client (how many jobs it runs). It is not a limit: a session never fails
     * because it has run a number of jobs (docs/designs/npu-resources.md). */
    uint32_t pck_client,client_model,jobs,reserved;
    uint64_t pool_start,pool_bytes;
    uint8_t catalogue[32],admission[32],clock[32],power[32],supervision[32],ddr[32];
    /* What the application declares: concurrent graphs (slots), jobs kept in flight, graphs kept resident and tensor
     * links. All must be declared; none has a width limit below the memory they need. */
    uint32_t slots,in_flight,graphs,links;
};
_Static_assert(sizeof(struct os_npu_session)==OS_NPU_SESSION_BYTES,"OS NPU session geometry");
#define OS_NPU_WORK_US UINT64_C(60000000)
#endif
