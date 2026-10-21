#ifndef SUNSTONE_A733_REMOTE_WRITERS_H
#define SUNSTONE_A733_REMOTE_WRITERS_H
#include <stdint.h>
/* Bus masters other than the Arm CPUs that can write DRAM on the A733.
 * docs/remote-writers.md gives the manual registers behind each decision. */
enum a733_remote_writer {
    A733_REMOTE_E902, A733_REMOTE_OR100, A733_REMOTE_DMA0, A733_REMOTE_DMA1,
    A733_REMOTE_CE, A733_REMOTE_SMHC0, A733_REMOTE_SMHC1, A733_REMOTE_SMHC2,
    A733_REMOTE_SMHC3, A733_REMOTE_NAND0, A733_REMOTE_UFS, A733_REMOTE_SPIF,
    A733_REMOTE_USB0, A733_REMOTE_USB1, A733_REMOTE_USB2, A733_REMOTE_PCIE0,
    A733_REMOTE_GMAC0, A733_REMOTE_DE0, A733_REMOTE_DI, A733_REMOTE_G2D,
    A733_REMOTE_EINK, A733_REMOTE_VE, A733_REMOTE_NPU, A733_REMOTE_GPU0,
    A733_REMOTE_CSI, A733_REMOTE_ISP, A733_REMOTE_WRITERS
};
#define A733_REMOTE_WRITER_BIT(writer) (UINT32_C(1) << (writer))
#define A733_REMOTE_WRITERS_ALL (A733_REMOTE_WRITER_BIT(A733_REMOTE_WRITERS) - 1u)

/* read32 returns 1 after storing the 32-bit word at a physical MMIO address,
 * or any other value when the read could not be made. */
struct a733_remote_writers_ops {
    int (*read32)(void *context, uint64_t address, uint32_t *value);
    void *context;
};

/* Reads the current reset, clock and DMA-enable state at EL3 and returns the
 * writers whose exclusion is not proven; zero means every listed master is
 * excluded. It never writes a register or stops an engine. A failed read,
 * a NULL ops or callback, and OR100 (no documented control) count as
 * unproven. One sample is meaningful only while no unproven programmable
 * agent can restart another master, so callers refuse on any nonzero bit. */
uint32_t a733_remote_writers_excluded(const struct a733_remote_writers_ops *ops);
#endif
