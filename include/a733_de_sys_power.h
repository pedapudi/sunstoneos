#ifndef SUNSTONE_A733_DE_SYS_POWER_H
#define SUNSTONE_A733_DE_SYS_POWER_H
#include <stdint.h>
#define A733_PCK_DOMAIN_BASE(domain) (UINT32_C(0x07060000)+UINT32_C(0x1000)*(domain))
#define A733_DE_SYS_PCK_BASE A733_PCK_DOMAIN_BASE(1)
/* Fixed, one-attempt ON operation for one display PCK domain: DE_SYS (1) or
 * VO1 (10). The pinned sun60iw2 pck600 table gives these domains the same
 * register offsets and delay row. The consumer retains all authority, backing
 * and tokens until whole-board power removal, including after READY.
 * authorize runs outside the register mutex: 1=authenticated, 0=metadata BUSY,
 * -1=refused. It must authenticate the designated task, whole-domain exclusion,
 * drained inherited traffic, clock/PCK leases and readonly supply observation
 * (DCDC2 for DE_SYS).
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
    uintptr_t base; /* Selected domain's register block. */
};
/* Initialize zeroed private storage once; no callback or hardware access.
 * a733_de_sys_power_init selects DE_SYS; the _domain form accepts 1 or 10. */
int a733_de_sys_power_init(struct a733_de_sys_power *,
    const struct a733_de_sys_power_ops *,void *,uint64_t deadline);
int a733_de_sys_power_init_domain(struct a733_de_sys_power *,
    const struct a733_de_sys_power_ops *,void *,uint64_t deadline,unsigned domain);
/* Task-only: 0 pending, 1 READY, -1 retained terminal failure. Initial command
 * and status must agree at OFF0 or ON8. OFF programs the pinned delay row and
 * requests only ON8. The request wait is at most10ms and is never renewed.
 * No poweroff, reset, drain inference, lease release or recovery is provided. */
int a733_de_sys_power_poll(struct a733_de_sys_power *);
int a733_de_sys_power_ready(const struct a733_de_sys_power *);
#endif
