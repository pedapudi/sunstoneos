/* Host capture of the native first-failure calls; no RTC or device access. */
#ifndef SUNSTONE_GPU_STOP_CRUMBS_H
#define SUNSTONE_GPU_STOP_CRUMBS_H
#include <stdint.h>
#define SUNSTONE_CRUMB_H
#define SUNSTONE_CRUMB_DEFINED 1
#define SUNSTONE_CRUMB_FAIL_SVC 0x5c0e
#define SUNSTONE_CRUMB_HERE() ((const void *)(uintptr_t)__LINE__)
#define PVR_GPU_STOP_CRUMB_TEST 1
static inline void sunstone_crumb(uint32_t stage, uint32_t value)
{ (void)stage; (void)value; }
void sunstone_crumb_fail(uint32_t site, uint32_t value, const void *caller);
#endif
