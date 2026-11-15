#ifndef SUNSTONE_G2D_H
#define SUNSTONE_G2D_H
/* G2D 2D engine class: memory-to-memory 2D operations. An application declares the g2d device in its manifest.json
 * ("devices": {"required": ["g2d"]}); the session grants it with SS_DEVICE_G2D. One OS owner task does every register
 * access (apps/os/g2d.c, docs/g2d-os.md). Every call returns at once: 1 done, 0 not yet (repeat the identical call after
 * ss_g2d_wait), -1 refused or failed. One request is in flight. */
#include <stddef.h>
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
    /* The power owner's supply verification (its clock, microseconds): the time verification may start (the provider
     * start plus A733_POWER_SUPPLY_SETTLE_US, or the provider start when the settle is skipped); the first wake of the
     * first time the open found DCDC2 verified, an upper bound on the verification time because the power owner keeps
     * no time of its own; and 1 when os_g2d_start marked the supply settled. */
    uint64_t verify_at_us, verified_seen_us;
    uint32_t settled_supply, reserved;
};
/* After close: the module clock, bus gate and reset, DE_SYS reset and domain words, read back after the sequence. */
struct ss_g2d_closed {
    uint32_t g2d_clk, g2d_bgr, de_sys_bgr, pck1_status;
    /* DE and DI gates read before the DE_SYS reset: bit 0 CCU 0xa00 bit 31, bit 1 0xa04 bit 0, bit 2 0xa20 bit 31,
     * bit 3 0xa24 bit 0. Any set bit leaves the DE_SYS reset deasserted. */
    uint32_t de_di_open, reserved;
    uint64_t closed_ticks;
};
/* A surface: width by height pixels of one of the eight 32-bit RGB orders (format 0 ARGB8888 to 7 BGRX8888, the
 * vendor's codes), rows stride bytes apart, starting offset bytes into one of the application's large regions
 * (manifest.json "large_regions"; ss_large_base gives its address). The output's region must be writable. */
struct ss_g2d_surface {
    uint32_t region, format;
    uint64_t offset;
    uint32_t width, height, stride, reserved;
};
enum { SS_G2D_FILL = 1 };
/* One operation. Fill writes color (ARGB, 32 bits) to the w by h rectangle at dx, dy of the output; source, sx and sy
 * are zero. The rest of the output is unchanged. */
struct ss_g2d_job {
    uint32_t op, color;
    struct ss_g2d_surface source, output;
    uint32_t sx, sy, dx, dy, w, h, reserved[2];
};
#define SS_G2D_DONE_MAGIC UINT32_C(0x44443247) /* "G2DD" */
/* The record of one operation. Times are system-counter ticks. */
struct ss_g2d_done {
    uint32_t magic, bytes;
    uint32_t rcq_status;       /* G2D 0x24 read by the interrupt handler before it acknowledged task end */
    uint32_t reset_word;       /* G2D 0x08 read by the handler before its mixer reset pulse */
    uint32_t status_after;     /* G2D 0x24 read by the owner after the handler */
    uint32_t irq_count;        /* G2D interrupts taken since the open */
    uint32_t iommu_scan;       /* 1: no IOMMU fault for master 10 after the operation */
    uint32_t output_iova;      /* device address of the output surface's first byte */
    uint32_t iommu_attached;   /* 1: this operation attached master 10 (the open's first operation) */
    /* At the first attach, before any write, also when it refused: ENABLE, INT_STATUS, L1 and L2 status of bank 0 then
     * bank 1, and the micro bypass of USB (master 0) and G2D (master 10). */
    uint32_t iommu_bank_words[8];
    uint32_t iommu_bypass[2];
    uint32_t iommu_fault[10];      /* master 10's fault record (a733_iommu_owner_fault order) */
    /* The IOMMU owner's first failure (step, register offset, value read back), its last refusal (1 clock lease, 2
     * entry state), and interrupts taken with no operation armed. */
    uint32_t iommu_fault_step, iommu_fault_word, iommu_fault_read, iommu_refused, stray_interrupts;
    uint64_t submit_ticks, launch_ticks, irq_ticks, done_ticks;
    uint64_t attach_us;            /* time the IOMMU attach took, 0 when no attach was made */
    uint64_t surface_physical, directory_physical;
};
_Static_assert(sizeof(struct ss_g2d_done) == 192 && offsetof(struct ss_g2d_done, submit_ticks) == 136,
               "apps/g2d-check/host.py decodes this layout");
/* Run one operation on the open engine and wait for its completion interrupt; done receives the record. */
int ss_g2d_run(const struct ss_g2d_job *job, struct ss_g2d_done *done);
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
