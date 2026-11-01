#ifndef SUNSTONE_NPU_NATIVE_RUNTIME_H
#define SUNSTONE_NPU_NATIVE_RUNTIME_H
#include "a733_npu_application.h"
#include "npu_runtime.h"

/* The supervised single-core image's providers with the NPU runtime in place
 * of the fixed-model convolution object (native-runtime.c). The runtime has
 * one slot, owned by the client task; the slot loads any of the models given
 * here, each of which must match an entry of the image's admitted table.
 * Before scheduling: prepares the runtime in the linked parent region
 * (__npu_runtime_parent_start), constructs the providers, the bootstrap and
 * PMIC tasks and the startup sequencer, binds the slot and INTID 97, and
 * attaches the job helper (npu_job.h) to the runtime. Returns 1 on success. */
int a733_npu_application_runtime_init(TaskHandle_t client, const struct npu_runtime_model *, unsigned count);
/* The runtime, for the client task only, once the bootstrap has activated it. */
struct npu_runtime *a733_npu_application_runtime(void);
#endif
