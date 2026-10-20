#ifndef SUNSTONE_A733_DE_SYS_POWER_H
#define SUNSTONE_A733_DE_SYS_POWER_H
#include <stdint.h>
#define A733_DE_SYS_PCK_BASE UINT32_C(0x07061000)
/* Fixed, one-attempt domain1 ON operation. The consumer retains all authority,
 * backing and tokens until whole-board power removal, including after READY.
 * authorize runs outside the register mutex: 1=authenticated, 0=metadata BUSY,
 * -1=refused. It must authenticate the designated task, whole-domain exclusion,
 * drained inherited traffic, clock/PCK leases and readonly DCDC2 observation.
 * enter uses the original absolute deadline, clamped to the immutable ON-wait
 * deadline during WAIT. read/write perform one ordered
 * Device access; callbacks must not take registry locks while held. A failed
 * access can have effects. Failure while held deliberately never calls leave.
 */
struct a733_de_sys_power_ops {
    int (*authorize)(void *);
    int (*now)(void *,uint64_t *);
    int (*enter)(void *,uint64_t);
    void (*leave)(void *);
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
};
struct a733_de_sys_power {
    const struct a733_de_sys_power *self;
    struct a733_de_sys_power_ops ops;
    void *context;
    uint64_t deadline,wait_deadline,last_now;
    unsigned state,held,write_attempted,inherited_on;
};
/* Initialize zeroed private storage once; no callback or hardware access. */
int a733_de_sys_power_init(struct a733_de_sys_power *,
    const struct a733_de_sys_power_ops *,void *,uint64_t deadline);
/* Task-only: 0 pending, 1 READY, -1 retained terminal failure. Initial command
 * and status must agree at OFF0 or ON8. OFF programs the pinned delay row and
 * requests only ON8. The request wait is at most10ms and is never renewed.
 * No poweroff, reset, drain inference, lease release or recovery is provided. */
int a733_de_sys_power_poll(struct a733_de_sys_power *);
int a733_de_sys_power_ready(const struct a733_de_sys_power *);
#endif
