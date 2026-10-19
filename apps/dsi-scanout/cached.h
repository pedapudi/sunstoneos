#ifndef SUNSTONE_DSI_SINGLE_CORE_RUNTIME_H
#define SUNSTONE_DSI_SINGLE_CORE_RUNTIME_H
#include "../../platform/a733-smp-cached/boot.h"
struct a733_cached_runtime {uint64_t epoch,startup_deadline,diagnostic_deadline,redistributor;};
extern struct a733_cached_runtime a733_cached_runtime[1];
int a733_smp_device_preparation_admitted(void);
#endif
