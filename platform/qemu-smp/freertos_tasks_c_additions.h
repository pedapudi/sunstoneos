// Included inside tasks.c, where the task's reentrancy block is accessible.
#include <stddef.h>
// __getreent (runtime/newlib_smp.c) reaches the block through StaticTask_t.
_Static_assert(offsetof(TCB_t, xTLSBlock) == offsetof(StaticTask_t, xDummy17),
               "StaticTask_t TLS member offset");
// sunstone_task_self_local (include/sunstone_task_self.h) reads the running task's task-local pointers through
// StaticTask_t.
_Static_assert(offsetof(TCB_t, pvThreadLocalStoragePointers) == offsetof(StaticTask_t, pvDummy15),
               "StaticTask_t task-local pointer offset");
// Only SVC 118 calls this function, with IRQs masked throughout the lookup.
struct _reent *runtime_current_reent(void) {
    if (xSchedulerRunning == pdFALSE) return _GLOBAL_REENT;
    return &portCURRENT_TCB(portGET_CORE_ID_FROM_ISR())->xTLSBlock;
}
