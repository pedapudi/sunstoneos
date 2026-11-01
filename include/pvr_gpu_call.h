#ifndef SUNSTONE_PVR_GPU_CALL_H
#define SUNSTONE_PVR_GPU_CALL_H
#ifdef __cplusplus
extern "C" {
#endif

/* Bounded GPU work in the calling EL0 task's context. The fixed privileged
 * dispatcher validates the operation, admitted task and argument extents.
 * Its return value reaches the caller after restoring EL0. Refused gateway
 * entry, or an absent dispatcher, returns -1.
 *
 * This uses the existing trusted shared-address-space task stacks. It does
 * not isolate a hostile task or protect a task's stack from its peers.
 * The dispatcher must return normally and leave critical nesting unchanged.
 * The gateway itself allocates no memory and never blocks. */
int pvr_gpu_call(unsigned operation, void *argument);
int pvr_gpu_call_dispatch(unsigned operation, void *argument);

#ifdef __cplusplus
}
#endif
#endif
