/* The host fixtures that model the synchronous NPU service do not link the EL0 ring (apps/os/npu-queue.c). The OS code they
 * include calls the ring only when the session declares more than one job in flight (os_npu_session_queued), which no such
 * fixture does: each stand-in below stops the check if it is reached. It uses no C library, because some fixtures build
 * with the image's own headers. Include it after the OS sources. */
#include "sunstone_npu_queue.h"
#define NPU_QUEUE_UNLINKED() __builtin_trap()
const struct ss_npu_queue_edges ss_npu_queue_edges={0};
int os_npu_queue_sdk_bind(void){NPU_QUEUE_UNLINKED();}
int os_npu_queue_tables(struct npu_runtime *rt){(void)rt;NPU_QUEUE_UNLINKED();}
int os_npu_queue_sdk_step(void){NPU_QUEUE_UNLINKED();}
int os_npu_queue_sdk_closed(void){NPU_QUEUE_UNLINKED();}
void os_npu_queue_sdk_fail(void){NPU_QUEUE_UNLINKED();}
