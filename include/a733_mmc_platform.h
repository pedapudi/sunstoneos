#ifndef SUNSTONE_A733_MMC_PLATFORM_H
#define SUNSTONE_A733_MMC_PLATFORM_H
#include "a733_clock_leases.h"
#include "a733_pins.h"
#include "a733_pmic.h"
#include "sunxi_mmc.h"
enum a733_mmc_emmc_host { A733_MMC_EMMC_SDC2=0, A733_MMC_EMMC_SDC3=1 };
/* Private, permanent provider. SD selects SDC0; eMMC explicitly selects SDC2
 * or SDC3. Zero preserves the existing default SDC2 selection.
 * Default APIs are read-only. A separate actual storage update service may bind
 * a permanent range owner. No close, power change, recovery or raw-command API exists. */
struct a733_mmc_platform_config {
    uint64_t owner, handoff, topology, deadline_us, lifetime_deadline_us;
    enum sunxi_mmc_kind kind;
    uint32_t voltage_mask;
    /* Exact inherited AHB/NSI/MBUS selector/divider profiles and rates. */
    uint32_t fabric[3], fabric_hz[3];
    /* SD: PF0..6. eMMC: PC0,1,5,6,8,9,10,11,13,14,15,16.
     * Expected mux is input or disabled. Desired route is fixed by this API;
     * caller supplies source-qualified drive/pull fields. Reset is never driven. */
    struct a733_pin_config expected[12], desired[12];
    enum a733_mmc_emmc_host emmc_host; /* SD requires zero. eMMC mux3/4 respectively. */
};
/* Fixed ROM JTAG route, not permission to take over a debugger. The caller's
 * physical authority must exclude debug clients before this exact handoff.
 * 1: input/disabled, 2: observed PF0/1/3/5 JTAG with PF2/4/6 disabled. */
static inline unsigned a733_mmc_sd0_inherited_mux(const struct a733_pin_config *pins)
{
    static const uint8_t rom[7]={3,3,15,3,15,3,15};
    unsigned passive=1,jtag=1;
    for(unsigned i=0;i<7;++i){
        passive&=pins[i].function==0||pins[i].function==15;
        jtag&=pins[i].function==rom[i];
    }
    return passive?1:jtag?2:0;
}
enum a733_mmc_platform_result { A733_MMC_PENDING, A733_MMC_BUSY,
    A733_MMC_READY, A733_MMC_COMPLETE, A733_MMC_RETAINED, A733_MMC_INVALID };
/* Bounded, non-reentrant callbacks; exactly1 means success. Authenticated
 * admission covers the immutable config and actual token, cold native card
 * state (or the documented first read-only SD CMD0 policy), populated eMMC,
 * source-qualified supplies/settling, pin pulls and reviewed recovery. It excludes every boot, DMA, IRQ, secure/remote and
 * bank-voltage writer for the complete lifetime. Inherited failed/active card
 * or controller state must not be presented as cold. All SDC2/SDC3/NAND/SPIF
 * alternate-pad and unselected-route writers are excluded before route enable.
 * Register/PMIC observations do not establish physical topology or settling.
 * All later writers must honor the same clock, pin and rail managers. */
struct a733_mmc_platform_ops {
    int (*now_us)(void *,uint64_t *);
    int (*current)(void *);
    int (*admit)(void *,const struct a733_mmc_platform_config *,const struct a733_clock_token *);
    int (*enter)(void *,uint64_t);
    void (*leave)(void *);
    void (*retain)(void *);
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
    int (*barrier)(void *);
    int (*outside)(void *,const void *,size_t);
};
struct a733_storage_update;
struct a733_mmc_platform {
    struct a733_mmc_platform_config config;
    struct a733_mmc_platform_ops ops;
    void *cookie;
    struct a733_clock_leases *clocks;
    struct a733_pins *pins;
    struct a733_rail_leases *rails;
    struct a733_pmic *pmic;
    struct a733_clock_token clock;
    struct a733_pin_lease_banked pin;
    struct a733_rail_token rail;
    struct a733_pmic_lifetime power;
    struct a733_pmic_request request;
    struct a733_pmic_snapshot snapshots[3];
    struct sunxi_mmc core;
    struct a733_storage_update *update; /* Exclusive permanent service binding. */
    atomic_uint busy;
    a733_clock_mask_t shared;
    uintptr_t base, module, bus;
    uint64_t last_now, deadline, reset_since;
    uint32_t initial_module, initial_bus, voltage_mode;
    unsigned initialized, stage, fault, held, clock_retained, rail_retained;
    unsigned rail_index, core_call;
};
/* Zero once. All managers, adapter storage and physical authority have platform
 * lifetime. Shared PMIC worker advances independently; the consumer never owns
 * its bus. Init performs no hardware operations. */
int a733_mmc_platform_init(struct a733_mmc_platform *,const struct a733_mmc_platform_config *,
    const struct a733_mmc_platform_ops *,void *,struct a733_clock_leases *,
    struct a733_pins *,struct a733_rail_leases *,struct a733_pmic *);
enum a733_mmc_platform_result a733_mmc_platform_poll(struct a733_mmc_platform *);
/* Pure metadata validation exposes required shared parent scopes. */
int a733_mmc_platform_scopes(const struct a733_mmc_platform_config *,a733_clock_mask_t *);
/* New operations supply fresh absolute deadlines. Idle poll returns cached
 * READY/COMPLETE only; it makes no current hardware-health claim. A read buffer
 * remains exclusive through completion until take; faults retain it forever. */
enum a733_mmc_platform_result a733_mmc_platform_read(struct a733_mmc_platform *,
    uint64_t first_sector,uint32_t blocks,uint8_t *,size_t,uint64_t deadline);
enum a733_mmc_platform_result a733_mmc_platform_take(struct a733_mmc_platform *,
    struct sunxi_mmc_result *,uint64_t deadline);
enum a733_mmc_platform_result a733_mmc_platform_card(struct a733_mmc_platform *,
    struct sunxi_mmc_card *,uint64_t deadline);
/* Explicit read-only cancellation/failure retention. The designated current
 * owner may call after expiry; no live-I/O admission or MMIO occurs. Rejects
 * update-bound providers. BUSY means serialization/metadata bookkeeping must
 * be retried; output must already be revoked and borrowed buffers retained. */
enum a733_mmc_platform_result a733_mmc_platform_retain(struct a733_mmc_platform *);
/* Private service composition. A copied pointer cannot establish ownership:
 * the service authenticates its self, exact platform and complete grant. */
int a733_mmc_platform_storage(struct a733_mmc_platform *,const void *,size_t);
int a733_mmc_platform_update_bind(struct a733_mmc_platform *,struct a733_storage_update *);
enum a733_mmc_platform_result a733_mmc_platform_update_poll(struct a733_mmc_platform *,struct a733_storage_update *);
enum a733_mmc_platform_result a733_mmc_platform_update_io(struct a733_mmc_platform *,struct a733_storage_update *,
    unsigned operation,uint64_t first,uint32_t blocks,void *,size_t,uint64_t deadline);
void a733_mmc_platform_update_retain(struct a733_mmc_platform *,struct a733_storage_update *);
#endif
