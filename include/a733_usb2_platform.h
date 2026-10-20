/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_A733_USB2_PLATFORM_H
#define SUNSTONE_A733_USB2_PLATFORM_H
#include "a733_pmic.h"
#include "a733_typec.h"
#define A733_USB2_SUPPLIES 5u
/* Cold, direct USB2 host on TYPEC24. The immutable admission authenticates
 * V1.2 wiring, measured supply stability, independent recovery and exclusion
 * of every remote/secure/IRQ/DMA owner. PCK7/8 and SERDES100MHz are inherited
 * ON. Local USB2 reset/gates start off. No reset proves DMA drain.
 */
struct a733_usb2_platform_config {
    uint64_t owner,identity,topology,deadline,lifetime_deadline;
    unsigned pck_client,superspeed,companion_root;
    unsigned topology_mode; /* 0 legacy,1 direct,2 one self-powered root hub. */
    uint32_t fabric[3],fabric_hz[3]; /* AHB/NSI/MBUS exact held profiles. */
    uint32_t supply_uv[A733_USB2_SUPPLIES]; /* DCDC1,DC1SW2,DCDC9,ELDO6,CLDO5. */
    uint32_t revision; /* Actual admitted DWC_usb31 release, checked after power. */
};
struct a733_usb2_platform_ops {
    int (*now)(void *,uint64_t *);
    int (*current)(void *);
    int (*admit)(void *,const struct a733_usb2_platform_config *);
    int (*enter)(void *,uint64_t);
    void (*leave)(void *);
    void (*retain)(void *);
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
    int (*outside)(void *,const void *,size_t);
    int (*read16)(void *,uintptr_t,uint16_t *);
    int (*write16)(void *,uintptr_t,uint16_t);
};
enum a733_usb2_result { A733_USB2_PENDING,A733_USB2_READY,A733_USB2_BUSY,
    A733_USB2_INVALID,A733_USB2_RETAINED };
struct a733_usb2_platform {
    struct a733_usb2_platform *self;
    struct a733_usb2_platform_config config;
    struct a733_usb2_platform_ops ops;
    void *cookie;
    struct a733_clock_leases *clocks;
    struct a733_rail_leases *rails;
    struct a733_pmic *pmic;
    struct a733_gpu_resource_manager *pck;
    struct a733_typec *typec;
    struct a733_clock_token clock;
    struct a733_rail_token rail;
    struct a733_gpu_resource_lease pck_lease;
    struct a733_pmic_lifetime pmic_lifetime;
    struct a733_pmic_request request;
    struct a733_pmic_snapshot supplies[A733_USB2_SUPPLIES+1];
    struct a733_typec_token attachment;
    atomic_uint busy;
    uint64_t deadline,last_now;
    unsigned phase,supply_index,held,failed,clock_retained,rail_retained;
    unsigned ss_phase,ss_failed;uint64_t ss_wait;
};
/* Zero private platform-lifetime storage before concurrency; dependencies use
 * one clock, registry, rail ledger and register mutex. One designated task owns
 * every host call. Type-C and its power task must continue independently.
 * All outputs and caller buffers exclude the complete private object graph.
 */
int a733_usb2_platform_init(struct a733_usb2_platform *,const struct a733_usb2_platform_config *,
    const struct a733_usb2_platform_ops *,void *,struct a733_clock_leases *,
    struct a733_rail_leases *,struct a733_pmic *,struct a733_gpu_resource_manager *,struct a733_typec *);
enum a733_usb2_result a733_usb2_platform_poll(struct a733_usb2_platform *);
/* Concurrent Type-C publication may return0; issue no controller/ring access
 * until1. Pin first attachment; never adopt a replacement. No MMIO here.
 */
int a733_usb2_platform_live(struct a733_usb2_platform *,uint64_t generation,uint64_t deadline);
int a733_usb2_platform_outside(struct a733_usb2_platform *,const void *,size_t);
void a733_usb2_platform_lost(struct a733_usb2_platform *);
/* Exact native register/field allowlist shared with source checks. */
uint32_t a733_usb2_platform_mask(uintptr_t);
int a733_usb2_platform_address(uintptr_t,unsigned writing);
#endif
