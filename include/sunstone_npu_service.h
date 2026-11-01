#ifndef SUNSTONE_NPU_APP_SERVICE_H
#define SUNSTONE_NPU_APP_SERVICE_H
#include <stddef.h>
#include <stdint.h>
/* OS task only. Bind once before applications start; callbacks execute only
 * in the service task that calls step. A negative callback result permanently
 * retains the mailbox and its buffers. */
struct ss_npu_backend {
    int (*run)(unsigned,const void *,size_t,void *,size_t,uint64_t);
    int (*close)(void);
};
int ss_npu_service_bind(const struct ss_npu_backend *);
/* 0 idle, 1 request completed/refused, -1 retained failure. */
int ss_npu_service_step(void);
void ss_npu_service_fail(void);
#endif
