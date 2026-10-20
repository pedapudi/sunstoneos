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
#define A733_DISPLAYPORT_CORE_RESULT_BYTES 352u
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
static inline int a733_displayport_fields_valid(const volatile struct a733_displayport_session *s)
{
    if(!s||!s->owner||!s->authority||s->pck_client>=16||
       !s->typec_owner||!s->typec_qualification||!s->typec_recovery||
       s->owner==s->typec_owner||s->owner==s->board.pmic_owner||
       s->owner==s->board.usb0_owner||s->typec_owner==s->board.pmic_owner||
       s->typec_owner==s->board.usb0_owner||
       s->iommu_gates[0]!=0x10007||s->iommu_gates[1]!=0x10007||
       !s->latch.identity||!s->latch.owner||s->latch.known_mask!=0x3fff||
       (s->latch.value&~UINT32_C(0x3fff))||
       s->source_min_mv<4750||s->source_max_mv>5250||
       s->source_min_mv>s->source_max_mv||s->source_ma<500||
       !s->zero_threshold_max_mv||s->zero_threshold_max_mv>800||
       s->fusb_device_id<0x80||s->fusb_device_id>0x82||
       s->apb_hz<10000000||s->apb_hz>400000000||s->apb_hz%1000000||
       s->reserved0)return 0;
    for(unsigned i=0;i<4;++i)
        if(s->reserved[i]||
           (s->typec_pins[i][0]!=0&&s->typec_pins[i][0]!=7)||
           s->typec_pins[i][1]>2||s->typec_pins[i][2]>3)return 0;
    return 1;
}
/* Source-backed clock/route profile predicate. Pure: no MMIO or provider access.
 * The native provider separately checks live registers under held ownership.
 */
int a733_displayport_parameters_valid(const struct a733_displayport_session *);
int a733_displayport_session_valid(const struct a733_displayport_session *);
/* One copied observation; state words retain their provider meanings.
 * READY means the declared observation was captured, not physical qualification.
 * No ACK or resource release follows a read. Reserved bytes remain zero.
 */
enum a733_displayport_reply_status {
    A733_DISPLAYPORT_PENDING=0,A733_DISPLAYPORT_READY=1,A733_DISPLAYPORT_FAULT=2
};
struct a733_displayport_reply {
    uint64_t magic;
    uint32_t version,bytes,status,error,stage,retained;
    uint64_t epoch,generation,lease,pd_generation,hpd_irq;
    uint8_t session_digest[32],build_id[32],pixel_digest[32];
    uint64_t allocation_cpu,allocation_physical,allocation_bytes;
    uint32_t pixel_iova,pitch,width,height;
    uint32_t de_state,iommu_state,scanned_masters,guards;
    uint32_t polarity,pin_assignment,hpd,padding_valid;
    uint32_t core_bytes,reserved[3];
    uint8_t core[A733_DISPLAYPORT_CORE_RESULT_BYTES],tail[416];
};
_Static_assert(sizeof(struct a733_displayport_reply)==A733_DISPLAYPORT_RESULT_BYTES,"DP reply size");
_Static_assert(offsetof(struct a733_displayport_reply,session_digest)==72,"DP reply identity");
_Static_assert(offsetof(struct a733_displayport_reply,allocation_cpu)==168,"DP reply allocation");
_Static_assert(offsetof(struct a733_displayport_reply,core)==256,"DP reply core offset");
#endif
