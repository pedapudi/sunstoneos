#ifndef SUNSTONE_SD_SINGLE_CORE_COMPAT_H
#define SUNSTONE_SD_SINGLE_CORE_COMPAT_H
#include "boot.h"
/* Only fields used by the unchanged board application. One admitted boot CPU,
 * not an eight-core cache/barrier record. No cached/SMP startup is linked. */
struct a733_cached_runtime { uint64_t epoch,startup_deadline,diagnostic_deadline,redistributor; };
extern struct a733_cached_runtime a733_cached_runtime[1];
int a733_smp_device_preparation_admitted(void);
#endif
