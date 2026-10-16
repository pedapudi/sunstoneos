// Included inside tasks.c, where the task's reentrancy block is accessible.
// Only SVC 118 calls this function, with IRQs masked throughout the lookup.
struct _reent *runtime_current_reent(void) {
    if (xSchedulerRunning == pdFALSE) return _GLOBAL_REENT;
    return &pxCurrentTCBs[portGET_CORE_ID_FROM_ISR()]->xTLSBlock;
}
