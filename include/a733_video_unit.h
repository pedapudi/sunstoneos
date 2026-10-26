/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_VIDEO_UNIT_H
#define SUNSTONE_A733_VIDEO_UNIT_H
#include "a733_clock_leases.h"
#include "a733_gpu_resources.h"

/* Resource core shared by the VE decoder and encoder bindings. One immutable
 * descriptor names the unit's fixed registers; the core supplies lease
 * validation, the register mutex, ordered accesses, parent/fabric admission,
 * the inherited powered baseline and compound identity. Codec job contracts,
 * register allowlists and start/prepare sequencing stay in the bindings.
 * The core acquires nothing; it borrows the binding's leases and retains them
 * and any held mutex after every uncertain access. */
struct a733_video_unit_desc {
    uintptr_t ve, pck;      /* Codec window and PCK power-domain base. */
    uint32_t module;        /* CCU module clock register offset. */
    uint32_t bus;           /* CCU 0xa8c reset-deassert and bus-gate bits. */
    uint32_t mbus[3];       /* Local gates at CCU 0x5c0, 0x5e0 and 0x5e4. */
    uint32_t autonomous[2]; /* CCU 0x1b10 mask and powered value; mask 0 skips. */
    unsigned irq;           /* GIC interrupt ID; masked, level, inactive. */
    unsigned ve0_mux;       /* Module bit 24 value that selects PLL_VE0. */
    a733_clock_mask_t exclusive;
    uint32_t identity;      /* VE offset of the 0xe0/0xe4/0xf0 identity block. */
    uint32_t feature[2];    /* Accepted identity-block +0xf0 values. */
    const uint64_t *versions;
    unsigned version_count;
};
/* Common lease and clock fields of the decoder and encoder configurations. */
struct a733_video_unit_config {
    struct a733_clock_leases *clocks;
    struct a733_clock_token clock;
    struct a733_gpu_resource_manager *power;
    struct a733_gpu_resource_lease pck;
    unsigned power_client;
    uint64_t owner, handoff;
    uint32_t module, module_hz, fabric[3], fabric_hz[3];
    /* Nonzero: the binding's cold start programs the selected PLL_VE to the
     * vendor profile (A733_VIDEO_VENDOR_MODULE at A733_VIDEO_VENDOR_HZ),
     * which needs that PLL as an exclusive scope. Zero: the PLL is a shared,
     * already running parent. */
    unsigned program_pll;
};
/* The vendor operating point of both units on every speed bin: 624 MHz at
 * 800 mV (sun60iw2p1-cpu-vf.dtsi ve-opp-table, lines 2142-2189). Module mux 1
 * at divider 1 selects the vendor parent: PLL_VE0 for the decoder, PLL_VE1
 * for the encoder (sun60iw2p1.dtsi ve and ve2 nodes). The PLL runs at
 * N=78, P0=3, M1=1: a 1,872 MHz oscillator divided by 3. */
#define A733_VIDEO_VENDOR_MODULE UINT32_C(0x01000000)
#define A733_VIDEO_VENDOR_HZ UINT32_C(624000000)
#define A733_VIDEO_PLL_FACTORS UINT32_C(0x00204d00)
/* enter returns 1 acquired, 0 effect-free busy, -1 uncertain; leave exactly 1.
 * read/write return exactly 1 after the access and its completion barrier;
 * write takes width 1 or 4. */
struct a733_video_unit_ops {
    int (*now_us)(void *,uint64_t *);
    int (*enter)(void *,uint64_t);
    int (*leave)(void *);
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,unsigned,uint32_t);
};
struct a733_video_unit {
    const struct a733_video_unit_desc *desc;
    struct a733_video_unit_config config;
    struct a733_video_unit_ops ops;
    void *cookie;
    uint64_t token,deadline,last_now;
    a733_clock_mask_t shared;
    uint32_t selected_mhz[3];
    unsigned initialized,failed,locked;
    /* PLL programming: the entry classification and completion. */
    uint32_t pll_inherited;
    unsigned pll_entered,pll_adopted,pll_done;
};
extern const struct a733_video_unit_desc a733_video_decoder_unit,a733_video_encoder_unit;

/* Pure scope derivation: fabric sources/rates, the selected VE PLL and PERI0.
 * A programmed PLL is left out of the shared scopes. */
int a733_video_unit_scopes(const struct a733_video_unit_desc *,const struct a733_video_unit_config *,
    a733_clock_mask_t *);
/* The exclusive scopes: the descriptor's, plus the selected PLL_VE when the
 * configuration programs it. */
a733_clock_mask_t a733_video_unit_exclusive(const struct a733_video_unit_desc *,
    const struct a733_video_unit_config *);
/* Requires the exclusive scopes, exact shared scopes and a stable OSC|PCK
 * lease. Returns 1 initialized, 0 refused without effects. */
int a733_video_unit_init(struct a733_video_unit *,const struct a733_video_unit_desc *,
    const struct a733_video_unit_config *,const struct a733_video_unit_ops *,void *);
/* Mark terminal failure; every later operation refuses. Returns -1. */
int a733_video_unit_fail(struct a733_video_unit *);
/* Monotonic time from now_us; refusal or regression fails the unit. */
int a733_video_unit_now(struct a733_video_unit *,uint64_t *);
int a733_video_unit_timely(struct a733_video_unit *);
/* Both leases healthy: 1. A registry busy beyond 2 ms or the deadline: 0.
 * Anything else fails the unit: -1. */
int a733_video_unit_held(struct a733_video_unit *);
int a733_video_unit_lock(struct a733_video_unit *);
int a733_video_unit_unlock(struct a733_video_unit *);
/* Accesses require the held mutex and fail the unit on any uncertainty. */
int a733_video_unit_read(struct a733_video_unit *,uintptr_t,uint32_t *);
int a733_video_unit_write(struct a733_video_unit *,uintptr_t,unsigned,uint32_t);
int a733_video_unit_check(struct a733_video_unit *,uintptr_t,uint32_t mask,uint32_t expected);
/* Read-modify-write of mask bits, then verify the readback. */
int a733_video_unit_update(struct a733_video_unit *,uintptr_t,uint32_t mask,uint32_t value);
/* Fabric words, DCXO/reference, selected VE PLL, PERI0 outputs and the shared
 * PCK clock gate, in that order. Returns 1 when all match the configuration. */
int a733_video_unit_clocks(struct a733_video_unit *);
/* The same checks for a cold start. A programmed PLL may instead be disabled:
 * enable, lock enable, LOCK and output gate clear, and no other bit set but
 * the factors, the LDO and reserved bit 24, which is kept. The lock-level
 * fields (bits 7:5) must be zero. A programmed PLL that already runs the
 * vendor profile with its output open is adopted. */
int a733_video_unit_clocks_entry(struct a733_video_unit *);
/* After a733_video_unit_clocks_entry, under the held mutex, with the module
 * gate closed: write the module selector and divider, then run the manual's
 * enable method 2 on a disabled PLL, in one call that spins at most about
 * 320 us on LOCK. Returns 1 done (at once when nothing is programmed) and 0
 * refused; uncertain accesses fail the unit. */
int a733_video_unit_pll_prepare(struct a733_video_unit *);
/* Clocks plus PCK ON, module/bus/local gates, autonomous power control and a
 * masked, inactive, level-triggered interrupt. No VE register is read. */
int a733_video_unit_baseline(struct a733_video_unit *);
/* GIC distributor idle, interrupt masked, not pending or active, level. */
int a733_video_unit_irq_quiet(struct a733_video_unit *);
/* Read the identity block; returns 1 for an accepted feature word and listed
 * compound version, which is stored through the pointer in either case. */
int a733_video_unit_identity(struct a733_video_unit *,uint64_t *);
/* Lowest native access for one allowlisted physical address: identity-mapped
 * Device-nGnRnE translation, DSB SY before and after, volatile access. A
 * nonzero owned mask refuses a write that changes any bit outside it. Callers
 * apply their allowlists first. Non-AArch64 builds refuse. */
int a733_video_unit_native_read(uintptr_t,uint32_t *);
int a733_video_unit_native_write(uintptr_t,unsigned,uint32_t owned,uint32_t);
#endif
