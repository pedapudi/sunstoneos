/* Case 32 of checks/os-npu-service.py: the calls that os_npu_start makes once it has begun to start the service. A session
 * that does not grant the NPU must return before any of them, so each stops the program. The ledger functions are linked
 * from kernel/umem/umem.c, so the case checks the admission-time preparation by its session read instead. */
#include <stdio.h>
#include <stdlib.h>
#define TRAP(name) void name(void) { fprintf(stderr, "ungranted NPU start reached " #name "\n"); abort(); }
/* The graph-region path of the admission-time preparation, which the ring selects: an ungranted session reads neither the
 * application's manifest nor its graph index. */
#include "application.h"
const struct ss_application ss_application;
TRAP(os_npu_index_check)
TRAP(os_npu_index_entry)
TRAP(a733_npu_startup_init)
TRAP(os_npu_client_task)
TRAP(os_npu_complete)
TRAP(os_npu_irq_bind)
TRAP(os_npu_sdk_bind)
