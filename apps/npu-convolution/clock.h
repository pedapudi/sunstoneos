#ifndef SUNSTONE_NPU_APPLICATION_CLOCK_H
#define SUNSTONE_NPU_APPLICATION_CLOCK_H
#include "session.h"
struct npu_application_clock {
    struct a733_npu_pll pll;
    struct a733_npu_pll_config admitted;
    struct a733_clock_leases *registry;
    struct a733_platform_locks *locks;
    struct a733_counter_clock *counter;
    TaskHandle_t task;
    uint64_t whole_platform;
    unsigned initialized,held;
};
/* Private image storage, initialized once before scheduling. The authenticated
 * session and all providers remain immutable/alive until full power removal. */
int npu_application_clock_init(struct npu_application_clock *,const struct npu_session *,
    struct a733_clock_leases *,struct a733_platform_locks *,struct a733_counter_clock *,
    TaskHandle_t,uint64_t);
#endif
