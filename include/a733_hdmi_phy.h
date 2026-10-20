/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_HDMI_PHY_H
#define SUNSTONE_A733_HDMI_PHY_H
#include "a733_clock_leases.h"
#include <stddef.h>
#include <stdint.h>

#define A733_HDMI_BASE UINT64_C(0x05520000)
#define A733_HDMI_PHY_BASE UINT64_C(0x05600000)
#define A733_HDMI_TOP_BASE UINT64_C(0x05510000)
#define A733_HDMI_TV_BASE UINT64_C(0x05730000)
#define A733_HDMI_VO1_SCOPE (UINT64_C(1)<<63)
#define A733_HDMI_INITIAL_WORDS 8u
enum a733_hdmi_mode { A733_HDMI_720P60=4, A733_HDMI_1080P60=16 };
enum a733_hdmi_range { A733_HDMI_LIMITED=0,A733_HDMI_FULL=1 };
struct a733_hdmi_timing {
    uint32_t pixel_hz;
    uint16_t width,height,htotal,vtotal,hfront,hsync,hback,vfront,vsync,vback;
    uint8_t vic;
};
/* Full initial TOP PHY words: 0,4,20,24,28,2c,30,34. No reset image is supplied.
 * id/generation must resolve to externally qualified analog/reset provenance,
 * including charge pump, reserved/test controls and all inactive consumers. */
struct a733_hdmi_initial {
    uint64_t id,generation;
    uint32_t words[A733_HDMI_INITIAL_WORDS];
    uint32_t resistor_control,resistor_manual;
};
struct a733_hdmi_reservation {
    struct a733_clock_token clock;
    uint64_t generation,rail_owner,rail_generation,system_owner,system_generation;
    uint64_t inactive_epoch,recovery_epoch;
    /* Exactly one recovery authority: independent recovery, or supervised
     * TV0 pattern or explicitly bound framebuffer with permanent retention
     * until manual power removal. framebuffer_epoch distinguishes DMA ownership. */
    uint64_t supervision_epoch;
    uint64_t framebuffer_epoch; /* Distinct retained DMA authority; zero for pattern. */
    /* Acquired parent rates, not nominal mode clocks. TOP PLL uses raw DCXO;
     * SFR uses the normalized SYS24M parent. These are runtime authority facts,
     * not serialized session or initial analog-profile fields. */
    uint32_t phy_reference_hz,sfr_hz;
};
enum a733_hdmi_source { A733_HDMI_PATTERN=0,A733_HDMI_FRAMEBUFFER=1 };
struct a733_hdmi_request {
    struct a733_hdmi_initial initial;
    enum a733_hdmi_mode mode;
    enum a733_hdmi_range range;
    enum a733_hdmi_source source;
    /* Authenticated expected raw TOP PLL reference. Occupies former tail
     * padding; new images refuse old sessions whose padding was zero. */
    uint32_t phy_reference_hz;
};
_Static_assert(sizeof(struct a733_hdmi_request)==72&&
    offsetof(struct a733_hdmi_request,phy_reference_hz)==68,"HDMI request wire layout");
struct a733_hdmi_ops {
    int (*now_us)(void *,uint64_t *);
    /* Persistent output, including partial acquisition on 0/-1. May return 0
     * while pending. All other callbacks return exactly1 on success.
     * Resolve actual common clock token status, owner/generation, exclusive
     * VO1 (both TV paths, HDMI/eDP and shared TOP/PLL/domain/reset writers),
     * stable OSC/REF/FABRIC, supported raw24/26MHz TOP PLL reference,
     * normalized24MHz SFR and actual PMIC rail reservations.
     * Own the complete system-control resistor words 03000160/164 through a
     * common authority used by EVERY writer. HDMI-local ownership is not
     * sufficient. Hold all reservations through stop/fault; no implicit release.
     * Authenticate the entire initial profile, cold/reset handoff, inactive
     * producers and drained prior transactions. Authenticate independent recovery
     * or a fixed supervised720p60 limited-range reservation. Framebuffer input
     * additionally requires actual retained DE/memory authority and an explicit
     * framebuffer epoch; pattern authority alone cannot enable DMA.
     * Establish
     * Device mappings, VO1 domain10 ON, required bus clocks/reset release and
     * fixed rails before returning1. Do not write local HDMI/TV/PHY registers.
     * Native binding is a separate required component; booleans or fabricated
     * profile IDs cannot implement this contract. */
    int (*acquire)(void *,const struct a733_hdmi_request *,uint64_t,
                   struct a733_hdmi_reservation *);
    int (*validate)(void *,const struct a733_hdmi_request *,
                    const struct a733_hdmi_reservation *,uint64_t);
    int (*read8)(void *,uintptr_t,uint8_t *);
    int (*write8)(void *,uintptr_t,uint8_t);
    int (*read32)(void *,uintptr_t,uint32_t *);
    int (*write32)(void *,uintptr_t,uint32_t);
    int (*barrier)(void *);
    /* Metadata only; exact persistent token is retained even when registry
     * marking is pending. May return0; retry must never perform device I/O. */
    int (*retain)(void *,const struct a733_hdmi_reservation *);
};
/* Private shared access state owned by the composed controller. Callbacks,
 * requests and all token storage stay live, disjoint and immutable. One
 * serialized privileged task, no copying, rebinding, reentry or concurrent
 * leaf calls. Time and registry use one monotonic provider/history. */
struct a733_hdmi_io {
    struct a733_hdmi_io *self;
    struct a733_hdmi_ops ops;
    struct a733_hdmi_request request;
    struct a733_hdmi_reservation reservation;
    void *context;
    uint64_t now,deadline;
    unsigned admitted,fault,retained,writes;
};
struct a733_hdmi_step { uintptr_t address;uint32_t mask,value;uint8_t width; };
int a733_hdmi_timing(enum a733_hdmi_mode,struct a733_hdmi_timing *);
int a733_hdmi_tick(struct a733_hdmi_io *);
int a733_hdmi_fail(struct a733_hdmi_io *);
int a733_hdmi_read(struct a733_hdmi_io *,uintptr_t,unsigned,uint32_t *);
int a733_hdmi_write(struct a733_hdmi_io *,uintptr_t,unsigned,uint32_t);
/* RMW with masked readback, bounded to one field per call. W1C and command
 * registers use write() explicitly, never this helper. */
int a733_hdmi_field(struct a733_hdmi_io *,const struct a733_hdmi_step *);

struct a733_hdmi_phy {
    struct a733_hdmi_phy *self;
    struct a733_hdmi_io *io;
    uint64_t until,operation_deadline;
    unsigned stage,index,phase,locks,preflight,ready;
    uint16_t value;
};
int a733_hdmi_phy_init(struct a733_hdmi_phy *,struct a733_hdmi_io *);
int a733_hdmi_phy_preflight(struct a733_hdmi_phy *);
/* One bounded register field or internal I2C action per poll. TOP and PHY lock
 * observations establish programming completion, not sink video acceptance. */
int a733_hdmi_phy_poll(struct a733_hdmi_phy *);
#endif
