#ifndef SUNSTONE_A733_DMA_REGS_H
#define SUNSTONE_A733_DMA_REGS_H

/* DMA0 register offsets from the pinned A733 User Manual, section 13.1.7.
 * Global registers are relative to the controller base. */
enum {
    A733_DMA_REG_AUTO_GATE = 0x28, A733_DMA_REG_BUSY = 0x30,
    A733_DMA_REG_CPU_ROUTE = 0x34, A733_DMA_REG_MCU_ROUTE = 0x38,
    A733_DMA_REG_CHANNELS = 0x100, A733_DMA_REG_CHANNEL_STRIDE = 0x40
};
/* Channel registers are relative to A733_DMA_REG_CHANNELS plus channel times
 * A733_DMA_REG_CHANNEL_STRIDE. */
enum {
    A733_DMA_REG_ENABLE = 0, A733_DMA_REG_PAUSE = 4, A733_DMA_REG_DESCRIPTOR = 8,
    A733_DMA_REG_DESTINATION = 0x14, A733_DMA_REG_REMAINING = 0x18,
    A733_DMA_REG_MODE = 0x28, A733_DMA_REG_PACKAGES = 0x30,
    A733_DMA_REG_IRQ_ENABLE = 0x34, A733_DMA_REG_PENDING = 0x38,
    A733_DMA_REG_DESCRIPTOR_HIGH = 0x3c
};
/* Channel registers that startup admission requires to read zero. DESCRIPTOR
 * and PACKAGES are excluded because they keep values from earlier transfers.
 * The native startup read allowlist admits exactly these channel registers. */
#define A733_DMA_QUIET_REGISTERS A733_DMA_REG_ENABLE, A733_DMA_REG_PAUSE, \
    A733_DMA_REG_MODE, A733_DMA_REG_IRQ_ENABLE, A733_DMA_REG_PENDING, \
    A733_DMA_REG_DESCRIPTOR_HIGH

#endif
