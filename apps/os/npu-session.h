#ifndef SUNSTONE_OS_NPU_SESSION_H
#define SUNSTONE_OS_NPU_SESSION_H
#include <stdint.h>
#define OS_NPU_SESSION_MAGIC UINT64_C(0x415337334f4e5031)
#define OS_NPU_SESSION_BYTES 264u
/* Version 2 added the application's declaration of NPU resources, four full-width fields (docs/designs/npu-resources.md);
 * version 3 adds the bounds of the graphs it loads at run time; version 4 drops the digest of the image's model catalogue:
 * a graph runs as compiled, admitted by its structure and isolated by the page tables and the NPU MMU, so the session
 * names no list of models. Earlier versions are no longer admitted. */
#define OS_NPU_SESSION_VERSION 4u
/* Fixed optional profile; the supervisor grants this exact private interval.
 * Evidence hashes bind external facts, never manufacture those facts. */
struct os_npu_session {
    uint64_t magic,version,bytes,owner,command_admission,supervisor;
    /* jobs: the workload of the diagnostic client (how many jobs it runs). It is not a limit: a session never fails
     * because it has run a number of jobs (docs/designs/npu-resources.md). */
    uint32_t pck_client,client_model,jobs,reserved;
    /* A placement request for the NPU's reservation: its base and size, both zero when the allocator chooses (the normal case).
     * Any image accepts a request; the ledger reserves exactly that extent and refuses it unless it is free DRAM above the image
     * that stays inside one 4 GiB window. Only an explicit request sets these words: tools/session.py --npu-pool and the runner's
     * --npu-pool option write them, and the session preparation zeroes them otherwise (apps/os/host.py set_npu_pool_request). */
    uint64_t pool_request[2];
    uint8_t admission[32],clock[32],power[32],supervision[32],ddr[32];
    /* What the application declares: concurrent graphs (slots), jobs kept in flight, graphs kept resident and tensor
     * links. All must be declared; none has a width limit below the memory they need. */
    uint32_t slots,in_flight,graphs,links;
    /* Graphs beyond those the image links are loaded at run time (npu_runtime_register): each is at most graph_bytes
     * long and has no tensor larger than tensor_bytes. The reservation is made for these bounds; both zero means the
     * application loads no graph at run time. */
    uint32_t graph_bytes,tensor_bytes;
};
_Static_assert(sizeof(struct os_npu_session)==OS_NPU_SESSION_BYTES,"OS NPU session geometry");
#define OS_NPU_WORK_US UINT64_C(60000000)
#endif
