#ifndef SUNSTONE_OS_QUIESCE_H
#define SUNSTONE_OS_QUIESCE_H
#include <stdatomic.h>
#include <stdint.h>
#include "sunstone_crumb.h"

enum os_quiesce_result { OS_QUIESCE_PENDING, OS_QUIESCE_DONE, OS_QUIESCE_FAULT };
enum os_quiesce_order {
    OS_QUIESCE_QUEUES, OS_QUIESCE_DEVICES, OS_QUIESCE_CLOCK,
    OS_QUIESCE_THERMAL, OS_QUIESCE_PMIC, OS_QUIESCE_TERMINALS
};
enum os_quiesce_owner {
    OS_QUIESCE_NPU=SUNSTONE_CRUMB_QUIESCE_NPU,
    OS_QUIESCE_GPU=SUNSTONE_CRUMB_QUIESCE_GPU,
    OS_QUIESCE_USB=SUNSTONE_CRUMB_QUIESCE_USB,
    OS_QUIESCE_RADIO=SUNSTONE_CRUMB_QUIESCE_RADIO,
    OS_QUIESCE_AUDIO=SUNSTONE_CRUMB_QUIESCE_AUDIO,
    OS_QUIESCE_HEADSET=SUNSTONE_CRUMB_QUIESCE_HEADSET,
    OS_QUIESCE_STORAGE=SUNSTONE_CRUMB_QUIESCE_STORAGE,
    OS_QUIESCE_DMA=SUNSTONE_CRUMB_QUIESCE_DMA,
    OS_QUIESCE_CPU_CLOCK=SUNSTONE_CRUMB_QUIESCE_CPU_CLOCK,
    OS_QUIESCE_THERMAL_OWNER=SUNSTONE_CRUMB_QUIESCE_THERMAL_OWNER,
    OS_QUIESCE_PMIC_OWNER=SUNSTONE_CRUMB_QUIESCE_PMIC_OWNER,
    OS_QUIESCE_TERMINAL_OWNER=SUNSTONE_CRUMB_QUIESCE_TERMINAL_OWNER,
    OS_QUIESCE_USB0=SUNSTONE_CRUMB_QUIESCE_USB0,
    OS_QUIESCE_I2C=SUNSTONE_CRUMB_QUIESCE_I2C,
    OS_QUIESCE_RTC=SUNSTONE_CRUMB_QUIESCE_RTC,
};
/* Each service owns one descriptor for the session. Registration precedes
 * task creation or device access. The supervisor alone invokes step, which
 * must never wait. Hardware work stays on the task authorized to perform it.
 * requested is published before step; result and fault_state publish owner
 * replies. After DONE, step only observes the stopped proof; a late fault
 * revokes it without restarting hardware work. FAULT is final. A failed owner
 * keeps its resources and its first fault record. */
struct os_quiesce {
    struct os_quiesce *next;
    enum os_quiesce_result (*step)(struct os_quiesce *);
    enum os_quiesce_owner owner;
    enum os_quiesce_order order;
    atomic_uint requested, result, fault_state;
    uint32_t site, value;
    const void *caller;
    unsigned registered, visited;
};
int os_quiesce_register(struct os_quiesce *);
int os_quiesce_requested(const struct os_quiesce *);
enum os_quiesce_result os_quiesce_result(const struct os_quiesce *);
void os_quiesce_complete(struct os_quiesce *);
/* Returns 1 during a failure window or a requested normal-return step,
 * capturing the fault instead of closing board admission. Callers preserve their normal failure path otherwise. */
int os_quiesce_fault(struct os_quiesce *,uint32_t site,uint32_t value,const void *caller);
int os_quiesce_drained_before(enum os_quiesce_order);
void os_quiesce_note_faults(void (*note)(uint32_t,uint32_t,const void *));
/* One supervisor pass. Faults are reported once and other owners still run.
 * DONE means every owner completed; FAULT means all steps ran but at least
 * one owner could not establish safe retirement. */
enum os_quiesce_result os_quiesce_step(void (*note)(uint32_t,uint32_t,const void *));
#endif
