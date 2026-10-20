#ifndef SUNSTONE_ACCELERATORS_APPLICATION_H
#define SUNSTONE_ACCELERATORS_APPLICATION_H
#include "a733_npu_application.h"
#include "a733_gpu_backend.h"
#include "a733_gpu_compute_reserved.h"
#include "a733_gpu_platform_native.h"
#include "a733_npu_startup.h"

/* A borrowed view of the one permanent NPU application provider owner. No
 * component may copy, reinitialize, release or replace these instances. */
struct accelerator_providers {
    struct a733_counter_clock *counter;
    struct a733_clock_leases *clocks;
    struct a733_platform_locks *locks;
    struct a733_gpu_resource_manager *gpu;
    struct a733_gpu_resource_native *gpu_native;
    struct a733_pmic *pmic;
};
int accelerator_providers_get(struct accelerator_providers *);
int accelerator_shared_live(void);
void accelerator_shared_fail(unsigned);

/* Root authenticates and retains the entire combined session, including this
 * package, before application preparation. This accessor grants no admission. */
const struct pvr_compute_package *accelerator_gpu_package(void);
struct accelerator_gpu {
    struct a733_gpu_platform_native native;
    struct a733_gpu_platform power;
    struct a733_gpu_backend backend;
    struct a733_gpu_reserved firmware;
    struct a733_gpu_compute_reserved compute;
    struct pvr_gpu_service service;
    StaticTask_t task;
    StackType_t stack[4096] __attribute__((aligned(16)));
    TaskHandle_t client;
    atomic_uint failed,result,sequence,closed,mailbox,transport_ready;
    unsigned operation,expected,bytes;
    uint8_t input[PVR_COMPUTE_PACKAGE_INPUT];
    uint32_t output[PVR_COMPUTE_PACKAGE_INPUT/4];
};
extern struct accelerator_gpu accelerator_gpu;
int accelerator_gpu_prepare(void);
int accelerator_gpu_live(void);
void accelerator_gpu_fail(void);
void accelerator_gpu_irq(void);
BaseType_t accelerator_gpu_task_privileged(TaskFunction_t,void *);
TaskHandle_t accelerator_npu_client_create(void);
void accelerator_usb_start(void);
BaseType_t accelerator_usb_task_privileged(TaskFunction_t,void *);
#endif
