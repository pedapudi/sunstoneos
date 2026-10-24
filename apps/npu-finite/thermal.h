#ifndef SUNSTONE_NPU_FINITE_THERMAL_H
#define SUNSTONE_NPU_FINITE_THERMAL_H
#include "a733_npu_application.h"
#include "a733_thermal.h"
#include "a733_counter_clock.h"
#include "a733_platform_locks.h"
enum npu_finite_thermal_reason { NPU_THERMAL_NONE, NPU_THERMAL_HOT,
    NPU_THERMAL_CRITICAL, NPU_THERMAL_STALE, NPU_THERMAL_FAULT };
struct npu_finite_thermal {
    struct a733_thermal driver;
    struct a733_thermal_policy policy;
    struct a733_thermal_config config;
    struct a733_clock_token token;
    struct a733_counter_clock *clock;
    struct a733_clock_leases *registry;
    struct a733_platform_locks *locks;
    TaskHandle_t owner;
    /* One generation: deadline, first-stop reason and ALLOW/ACCEPTED/STOP. */
    atomic_uint_fast64_t gate;
    atomic_uint stopped;
    uint64_t deadline,initial_until,next_read,observed;
    unsigned active;
    atomic_uint fault;
};
extern unsigned npu_finite_thermal_irq_admitted;
int npu_finite_thermal_init(struct npu_finite_thermal *,const struct a733_npu_thermal_boot *,
    struct a733_counter_clock *,struct a733_clock_leases *,struct a733_platform_locks *,TaskHandle_t,uint64_t);
int npu_finite_thermal_step(struct npu_finite_thermal *);
int npu_finite_thermal_watch(struct npu_finite_thermal *);
int npu_finite_thermal_claim(struct npu_finite_thermal *);
void npu_finite_thermal_complete(struct npu_finite_thermal *);
void npu_finite_thermal_close(struct npu_finite_thermal *);
unsigned npu_finite_thermal_reason(const struct npu_finite_thermal *);
int npu_finite_thermal_stopping(const struct npu_finite_thermal *);
uint64_t npu_finite_thermal_deadline(const struct npu_finite_thermal *);
int npu_finite_thermal_quiet(const struct npu_finite_thermal *);
#endif
