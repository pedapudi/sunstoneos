#ifndef SUNSTONE_A733_DMA_NATIVE_H
#define SUNSTONE_A733_DMA_NATIVE_H
#include "dma_memory.h"

/* EL1 adapter. Keep this context and its controller outside all admitted DMA
 * windows. Calls require serialization and pinned stage-1 mappings; no stage-2
 * translation, IOMMU, or nonidentity physical-to-device bus translation is used.
 * Page-table attributes must describe actual RAM/device behavior. Register only
 * RAM reserved by the platform, never the general heap or firmware memory.
 * All CPUs sharing buffers need qualified coherent caches with <=64-byte CWG.
 */
struct a733_dma_native {
    struct dma_memory_policy memory;
    uintptr_t registers;
    void (*fatal)(void *);
    void *fatal_cookie;
    unsigned initialized;
};

/* Zero-initialize once. MMIO window is exactly 0x2000 bytes. Initialization
 * inspects CPU translation/cache state but does not read device registers.
 * fatal must not return; invalid callback use traps if it does return.
 * Physical use requires admitted recovery: qualified independent watchdog or
 * an explicitly supervised session retaining all state until manual power removal.
 * A supervisor deadline never establishes DMA drain or permits buffer reuse.
 */
int a733_dma_native_init(struct a733_dma_native *, uintptr_t register_cpu,
                         uint64_t register_physical, const struct dma_memory_window *, size_t,
                         const struct dma_fifo_port *, size_t, void (*fatal)(void *), void *);
extern const struct a733_dma_operations a733_dma_native_operations;

/* EL1 translation query; preserves PAR_EL1 and interrupt masks. */
int a733_dma_native_translate(void *, uintptr_t, int write, struct dma_translation *);
#endif
