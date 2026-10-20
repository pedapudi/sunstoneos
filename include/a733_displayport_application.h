#ifndef SUNSTONE_A733_DISPLAYPORT_APPLICATION_H
#define SUNSTONE_A733_DISPLAYPORT_APPLICATION_H
#include "a733_board_application.h"
#include "a733_pins.h"
#define A733_DISPLAYPORT_SESSION_MAGIC UINT64_C(0x4153373344504131)
#define A733_DISPLAYPORT_CONTRACT_MAGIC UINT64_C(0x4153373344504331)
#define A733_DISPLAYPORT_RESULT_MAGIC UINT64_C(0x4153373344505231)
#define A733_DISPLAYPORT_REQUEST_MAGIC UINT64_C(0x4153373344504c31)
#define A733_DISPLAYPORT_SESSION_SECONDS 20u
#define A733_DISPLAYPORT_RESULT_BYTES 1024u
#define A733_DISPLAYPORT_WIDTH 1280u
#define A733_DISPLAYPORT_HEIGHT 720u
#define A733_DISPLAYPORT_STRIDE 2560u
#define A733_DISPLAYPORT_ALLOCATION_BYTES 2097152u
#define A733_DISPLAYPORT_PIXEL_OFFSET 36864u
#define A733_DISPLAYPORT_PIXEL_IOVA UINT32_C(0x10001000)
/* Pointer-free input for one retained 720p60 RGB565 display. The board prefix
 * selects workload 2: shared PMIC and USB0 providers, without USB host service.
 * The extension supplies separate TYPEC24 authority for DP negotiation.
 * Evidence identifies reviewed physical facts; nonzero hashes do not establish
 * those facts. Pixels are generated privately, never accepted as pointers.
 * All clocks, mappings, allocations and provider leases remain retained.
 */
struct a733_displayport_session {
    struct a733_board_session board;
    uint64_t magic,version,bytes,owner,authority,pck_client;
    uint8_t isolation[32],memory[32],analog[32],power[32],serdes[32],supervision[32];
    uint32_t iommu_gates[2],oscillator,reference,video1_pll,serdes_cfg,
        rtc_serdes0,apb_hz;
    uint64_t typec_owner,typec_qualification,typec_recovery;
    struct a733_pin_latch_handoff latch;
    uint32_t source_min_mv,source_max_mv,source_ma,zero_threshold_max_mv;
    uint32_t fusb_device_id,reserved0;
    uint8_t typec_pins[4][3],reserved[4];
};
_Static_assert(sizeof(struct a733_displayport_session)==952,"DP immutable wire layout");
_Static_assert(offsetof(struct a733_displayport_session,isolation)==640,"DP evidence offset");
_Static_assert(offsetof(struct a733_displayport_session,iommu_gates)==832,"DP clock offset");
_Static_assert(offsetof(struct a733_displayport_session,typec_owner)==864,"DP Type-C offset");
_Static_assert(offsetof(struct a733_displayport_session,latch)==888,"DP latch offset");
_Static_assert(offsetof(struct a733_displayport_session,typec_pins)==936,"DP pin offset");
/* Source-backed clock/route profile predicate. Pure: no MMIO or provider access.
 * The native provider separately checks live registers under held ownership.
 */
int a733_displayport_parameters_valid(const struct a733_displayport_session *);
int a733_displayport_session_valid(const struct a733_displayport_session *);
#endif
