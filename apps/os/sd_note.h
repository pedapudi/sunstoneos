#ifndef SUNSTONE_OS_SD_NOTE_H
#define SUNSTONE_OS_SD_NOTE_H
/* What a failed bounded write leaves in the failure record that survives the watchdog reset (apps/os/large_sd.c write_fail, apps/os/sd_owner.c). Stage 34 (and 33) carry where the
 * transfer stopped (os_sd_where_detail and os_sd_where_aux below). The report that a session returns carries the mode log with the same position per fault. */
#include "sunxi_mmc.h"
/* Where a write fault stopped, for the failure record of stages 33 and 34 (a watchdog reset loses the report). The detail's 26 bits: the raw interrupt status bits 15:1 in 14:0, the bus
 * timing in 16:15, the descriptor the IDMAC had reached in 22:17 (62 unreadable, 63 outside the list) and the core's error (clamped to 7) in 25:23. The record's aux field (15 bits): in
 * 8:0 the 512-byte blocks the card side had taken at the fault (SMHC_TBC0 / 512), in 12:9 the IDMAC error bits (sunxi_mmc_idst4) and in 14:13 the piece's size class
 * (sunxi_mmc_piece_class: 0 one sector, 1 two to 32, 2 33 to 255, 3 256). Protocol step 35 (the CMD25 data phase) is implied. */
static inline uint64_t os_sd_where_detail(const struct sunxi_mmc *core, unsigned timing)
{
    unsigned e = (unsigned)core->result.error; if (e > 7u) e = 7u;
    return (uint64_t)(((core->result.interrupts | core->result.rint_late) >> 1) & 0x7fffu) | (uint64_t)(timing & 3u) << 15 | (uint64_t)(core->result.desc_index & 63u) << 17 | (uint64_t)e << 23;
}
static inline uint64_t os_sd_where_aux(const struct sunxi_mmc *core)
{
    return (uint64_t)((core->result.tbc0 / 512u) & 511u) | (uint64_t)sunxi_mmc_idst4(core->result.dma_status) << 9 | (uint64_t)sunxi_mmc_piece_class(core->write_request.blocks) << 13;
}
/* The first fault of a session, for the failure record of stages 37 to 40 (26 bits): the RINTSTS bits 15:2 of both reads (the one that raised the fault and the one taken as it was
 * raised) in 13:0, bit 18 of either (the write-busy timeout, outside the low 16 bits) in bit 14, and the source line of the check in sunxi_mmc.c that raised the fault in 25:15. */
static inline uint64_t os_sd_first_fault(const struct sunxi_mmc *core)
{
    uint32_t r = core->result.interrupts | core->result.rint_late;
    return (uint64_t)((r >> 2) & 0x3fffu) | (uint64_t)((r >> 18) & 1u) << 14 | (uint64_t)(core->result.fault_line & 0x7ffu) << 15;
}
/* The aux of a failed recovery (15 bits): the platform's result in 2:0 and its fault flag in 3; board_live_refusal() bits 30:28 (fatal, service window, session deadline) in 6:4; the
 * owner's lifetime deadline passed in 7; the first fault's piece size class in 9:8 and IDMAC error bits in 13:10; in 14 that the loop ended on a platform result and not on
 * board_live() or the lifetime. */
static inline uint64_t os_sd_recover_aux(unsigned result, unsigned platform_fault, unsigned refusal, unsigned lifetime, unsigned piece, unsigned idst, unsigned result_exit)
{
    return (uint64_t)(result & 7u) | (uint64_t)(platform_fault & 1u) << 3 | (uint64_t)(refusal & 7u) << 4 | (uint64_t)(lifetime & 1u) << 7 | (uint64_t)(piece & 3u) << 8 |
           (uint64_t)(idst & 15u) << 10 | (uint64_t)(result_exit & 1u) << 14;
}
/* The write-busy timeouts of the controller that DMA writes cleared and waited out (RINTSTS bit 18, result.busy_timeouts per write), summed over the session and saturating at 255, in
 * bits 47:40 of os_sd_mode word 2. */
static inline uint64_t os_sd_busy_field(unsigned total) { return (uint64_t)(total > 255u ? 255u : total) << 40; }
/* The value word of the record for a stage and a detail, as sd_owner.c's diag_word builds it. */
static inline uint32_t os_sd_note_value(unsigned stage, uint64_t detail) { return (uint32_t)(stage & 0x3fu) | (uint32_t)(detail & 0x3ffffffu) << 6; }
/* The first two faults that the recovery acted on, for the report (os_sd_mode word 4): each in 26 bits, the core's error in 3:0, its protocol step in 9:4 and the low 16 bits
 * of the raw interrupt status in 25:10; the first fault in bits 25:0 and the second in bits 57:32. A session that steps down and completes still carries them. */
static inline uint64_t os_sd_fault_events(const struct sunxi_mmc_card *c)
{
    uint64_t word = 0; unsigned seen = 0;
    for (unsigned i = 0; i < c->mode_steps && i < SUNXI_MMC_MODE_LOG && seen < 2; ++i) {
        if (c->mode_log[i].reason != SUNXI_MMC_MODE_RETUNE && c->mode_log[i].reason != SUNXI_MMC_MODE_ERRORS) continue;
        const struct sunxi_mmc_mode_event *e = &c->mode_log[i];
        word |= ((uint64_t)(e->error & 15u) | (uint64_t)(e->protocol & 63u) << 4 | (uint64_t)e->interrupts << 10 | (uint64_t)(e->descriptor & 63u) << 26) << (32 * seen++);
    }
    return word;
}
/* Where each of those two faults stopped, for the upper half of os_sd_mode word 3, 16 bits per fault (the first in bits 47:32, the second in 63:48): the 512-byte blocks the card side had
 * taken (SMHC_TBC0 / 512) in 8:0, the IDMAC error bits in 12:9 and the piece's size class in 14:13; the descriptor the IDMAC had reached is in bits 31:26 of each fault in word 4. */
static inline uint64_t os_sd_fault_blocks(const struct sunxi_mmc_card *c)
{
    uint64_t word = 0; unsigned seen = 0;
    for (unsigned i = 0; i < c->mode_steps && i < SUNXI_MMC_MODE_LOG && seen < 2; ++i) {
        if (c->mode_log[i].reason != SUNXI_MMC_MODE_RETUNE && c->mode_log[i].reason != SUNXI_MMC_MODE_ERRORS) continue;
        word |= (uint64_t)((c->mode_log[i].blocks & 511u) | (c->mode_log[i].idst & 15u) << 9 | (c->mode_log[i].piece & 3u) << 13) << (32 + 16 * seen++);
    }
    return word;
}
#endif
