#ifndef SUNSTONE_G2D_H
#define SUNSTONE_G2D_H
/* G2D 2D engine class: memory-to-memory 2D operations. An application declares the g2d device in its manifest.json
 * ("devices": {"required": ["g2d"]}); the session grants it with SS_DEVICE_G2D. One OS owner task does every register
 * access (apps/os/g2d.c, docs/g2d-os.md). Every call returns at once: 1 done, 0 not yet (repeat the identical call after
 * ss_g2d_wait), -1 refused or failed. One request is in flight. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Words read at open, before the owner writes anything (entry) and after the open (after). The index names the
 * register; G2D-local words are read only after the open, once the block is powered, clocked and out of reset. */
enum {
    SS_G2D_CCU_PLL_REF,        /* CCU 0x000 */
    SS_G2D_CCU_PERI0,          /* CCU 0x0a0, PLL_PERI0 control */
    SS_G2D_CCU_PERI0_GATES,    /* CCU 0x1908, PERI0 output gate configuration */
    SS_G2D_CCU_PERI0_STATUS,   /* CCU 0x1988, PERI0 output gate status */
    SS_G2D_CCU_G2D_CLK,        /* CCU 0x0a40, G2D module clock */
    SS_G2D_CCU_G2D_BGR,        /* CCU 0x0a44, G2D bus gate (bit 0) and reset (bit 16) */
    SS_G2D_CCU_DE_SYS_BGR,     /* CCU 0x0a74, DE_SYS reset (bit 16) */
    SS_G2D_CCU_AHB_GATES,      /* CCU 0x05c0, DE AHB master gate (bit 5) */
    SS_G2D_CCU_MBUS_GATES,     /* CCU 0x05e0, DESYS MBUS gate (bit 11) */
    SS_G2D_RCCU_PCK,           /* R_CCU 0x1ac, PCK600 bus clock (bit 0) */
    SS_G2D_PCK1_POLICY,        /* PCK600 domain 1 (DE_SYS) +0x0 */
    SS_G2D_PCK1_STATUS,        /* PCK600 domain 1 +0x8 */
    SS_G2D_PCK1_DELAY,         /* PCK600 domain 1 +0xc10 */
    SS_G2D_CCU_IOMMU0,         /* CCU 0x058c, IOMMU bank 0 gates and reset */
    SS_G2D_CCU_IOMMU1,         /* CCU 0x05b4, IOMMU bank 1 gates and reset */
    SS_G2D_RTC_OSC,            /* RTC 0x160, oscillator detection */
    SS_G2D_CCU_DE_CLK,         /* CCU 0x0a00, DE module clock (gate bit 31) */
    SS_G2D_CCU_DE_BGR,         /* CCU 0x0a04, DE bus gate (bit 0) */
    SS_G2D_CCU_DI_CLK,         /* CCU 0x0a20, DI module clock (gate bit 31) */
    SS_G2D_CCU_DI_BGR,         /* CCU 0x0a24, DI bus gate (bit 0) */
    SS_G2D_GICD_ENABLE,        /* GICD_ISENABLER2 (0x03400108): INTID 83 is bit 19 */
    SS_G2D_GICD_PENDING,       /* GICD_ISPENDR2 (0x03400208) */
    SS_G2D_GICD_ACTIVE,        /* GICD_ISACTIVER2 (0x03400308) */
    SS_G2D_PLATFORM_WORDS
};
enum {
    SS_G2D_TOP_SCLK_GATE,      /* G2D 0x00 */
    SS_G2D_TOP_HCLK_GATE,      /* G2D 0x04 */
    SS_G2D_TOP_AHB_RESET,      /* G2D 0x08 */
    SS_G2D_TOP_SCLK_DIV,       /* G2D 0x0c */
    SS_G2D_TOP_VERSION,        /* G2D 0x10: GSU, VSU, RTMX and ROT counts, IP version in bits 31:16 */
    SS_G2D_TOP_MCLK_GATE,      /* G2D 0x18 */
    SS_G2D_TOP_RCQ_IRQ,        /* G2D 0x20 */
    SS_G2D_TOP_RCQ_STATUS,     /* G2D 0x24 */
    SS_G2D_TOP_RCQ_CTRL,       /* G2D 0x28 */
    SS_G2D_MIXER_CTRL,         /* G2D 0x100 */
    SS_G2D_MIXER_IRQ,          /* G2D 0x104 */
    SS_G2D_LOCAL_WORDS
};
#define SS_G2D_INFO_MAGIC UINT32_C(0x49443247) /* "G2DI" */
/* Domain-1 status samples the record keeps; status_reads counts every read, including those past the last kept one. */
#define SS_G2D_STATUS_SAMPLES 8u
struct ss_g2d_info {
    uint32_t magic, bytes;
    uint32_t domain_entry;     /* PCK600 domain 1 at entry: 0 OFF, 8 ON (adopted with no write) */
    uint32_t module_hz;        /* the G2D module clock the open configured */
    uint32_t entry[SS_G2D_PLATFORM_WORDS], after[SS_G2D_PLATFORM_WORDS];
    uint32_t local[SS_G2D_LOCAL_WORDS];
    uint64_t requested_ticks;  /* system counter when the owner took the open request */
    uint64_t domain_on_ticks;  /* when the power-domain owner answered the row ON */
    uint64_t opened_ticks;     /* when the open finished */
    /* The power owner's clock (microseconds): the row request, the owner's PCK command (0 when it adopted the
     * domain with no command) and the row's ON answer. */
    uint64_t row_request_us, pck_command_us, row_on_us;
    /* Bit 8 of the domain-1 policy (bit 0 here) and status (bit 1) words at entry and after the open (bits 8 and 9).
     * Recorded only; nothing refuses on it. */
    uint32_t pck_bit8;
    /* Domain-1 status read by the G2D owner at the request and at each row wake until the ON answer, with the
     * system-counter ticks since the request. */
    uint32_t status_reads;
    uint32_t status_word[SS_G2D_STATUS_SAMPLES], status_ticks[SS_G2D_STATUS_SAMPLES];
};
/* After close: the module clock, bus gate and reset, DE_SYS reset and domain words, read back after the sequence. */
struct ss_g2d_closed {
    uint32_t g2d_clk, g2d_bgr, de_sys_bgr, pck1_status;
    /* DE and DI gates read before the DE_SYS reset: bit 0 CCU 0xa00 bit 31, bit 1 0xa04 bit 0, bit 2 0xa20 bit 31,
     * bit 3 0xa24 bit 0. Any set bit leaves the DE_SYS reset deasserted. */
    uint32_t de_di_open, reserved;
    uint64_t closed_ticks;
};
/* Power, clock and reset the engine (the vendor's g2d_open order) and read its identity. */
int ss_g2d_open(struct ss_g2d_info *info);
/* Gate and reset the engine (the vendor's g2d_release order). The DE_SYS reset is asserted only when the DE and DI
 * gates read closed. The power domain stays on. */
int ss_g2d_close(struct ss_g2d_closed *closed);
/* Fault record of a failed call: the owner's step number and the word that failed it. */
int ss_g2d_fault(uint32_t *step, uint32_t *value);
void ss_g2d_wait(uint32_t wait_us);
#ifdef __cplusplus
}
#endif
#endif
