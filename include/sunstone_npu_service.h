#ifndef SUNSTONE_NPU_APP_SERVICE_H
#define SUNSTONE_NPU_APP_SERVICE_H
#include "sunstone_app.h"
#include <stddef.h>
#include <stdint.h>
/* OS task only. Bind once before applications start; callbacks execute only
 * in the service task that calls step. A negative callback result permanently
 * retains the mailbox and its buffers. */
struct ss_npu_backend {
    int (*run)(unsigned,const void *,size_t,void *,size_t,uint64_t);
    int (*close)(void);
    /* Immutable for the session: the models the backend links, which a test backend may name. A backend that links none
     * (the OS image) gives NULL and zero; every model is then registered at run time and reported through info. The
     * backend checks the linked table against its decoded models before bind; bind validates names and public capacities. */
    const struct ss_npu_model *models;
    unsigned count;
    /* Optional: the description of model index (linked or registered) from the backend's own decoded plan; 1, or 0 for an
     * unknown index. The name of a registered model is empty. */
    int (*info)(unsigned index,struct ss_npu_model *);
};
int ss_npu_service_bind(const struct ss_npu_backend *);
/* Optional, before bind. run returns 2 when the job failed with an NPU MMU exception and the NPU was recovered; the
 * callback then fills the description that ss_npu_last_fault gives the application. */
/* Optional, before bind: the callback adds a graph that the application hands over at run time; 1 with its model index, 0 when
 * the OS refuses it, negative for a retained failure. */
int ss_npu_service_bind_register(int (*)(const void *, size_t, unsigned *));
/* Optional, with the callback above: the reason of its last refusal, an enum ss_npu_register_refusal value. */
int ss_npu_service_bind_register_refusal(int (*)(void));
int ss_npu_service_bind_fault(void (*)(struct ss_npu_fault_info *));
/* Optional owner wakeup, registered once before bind and application startup.
 * Called in the publishing task after a request or failure becomes visible.
 * It must be safe at both application and service task privilege levels.
 * The notification must remain pending until consumed, including when the
 * owner has not entered its wait yet. It must not call the mailbox API. */
int ss_npu_service_bind_event(void (*notify)(void *),void *context);
/* 0 idle, 1 request completed/refused, -1 retained failure. */
int ss_npu_service_step(void);
void ss_npu_service_fail(void);
#endif
