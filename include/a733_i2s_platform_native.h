#ifndef SUNSTONE_A733_I2S_PLATFORM_NATIVE_H
#define SUNSTONE_A733_I2S_PLATFORM_NATIVE_H
#include <stdint.h>
/* Ordered AArch64 MMIO adapter; cookie is unused. Caller supplies stable Device
 * mappings, actual IRQ/producer ownership, the common register mutex and
 * authenticated clock leases. Recovery requires either a qualified independent
 * watchdog or an explicit supervised session retaining all state until manual
 * power removal. A deadline or reconnection never proves DMA drain or reuse. No clock/pin/DMA ownership is inferred from mapped addresses.
 * Reference/consumer/SDM/auto-gate registers are read-only in this adapter. */
int a733_audio_native_read(void *, uintptr_t, uint32_t *);
int a733_audio_native_write(void *, uintptr_t, uint32_t);
#endif
