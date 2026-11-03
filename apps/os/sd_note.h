#ifndef SUNSTONE_OS_SD_NOTE_H
#define SUNSTONE_OS_SD_NOTE_H
/* What a failed bounded write leaves in the failure record that survives the watchdog reset (apps/os/large_sd.c write_fail, apps/os/sd_owner.c). Stage 34 (and 33) carry where the
 * transfer stopped (os_sd_where_detail and os_sd_where_aux below). The report that a session returns carries the mode log with the same position per fault. */
#include "sunxi_mmc.h"
static inline uint64_t os_sd_write_detail(const struct sunxi_mmc *core)
{ return (uint64_t)(core->result.error & 15u) | (uint64_t)(core->protocol & 63u) << 4 | (uint64_t)(core->result.interrupts & 0xffffu) << 10; }
/* Where a write fault stopped, for the failure record of stages 33 and 34 (a watchdog reset loses the report). The detail's 26 bits: the raw interrupt status bits 15:1 in 14:0, the bus
 * timing in 16:15, the descriptor the IDMAC had reached in 22:17 (62 unreadable, 63 outside the list) and the core's error (clamped to 7) in 25:23. The record's aux field (15 bits): in 8:0 the 512-byte
 * blocks the card side had taken at the last fault (SMHC_TBC0 / 512), in 14:9 the same count of the first fault of the session in 4 KiB units (one descriptor). The piece's sector count and the
 * pieces written are not in the record; protocol step 35 (the CMD25 data phase) is implied by the status bits. */
static inline uint64_t os_sd_where_detail(const struct sunxi_mmc *core, unsigned timing)
{
    unsigned e = (unsigned)core->result.error; if (e > 7u) e = 7u;
    return (uint64_t)((core->result.interrupts >> 1) & 0x7fffu) | (uint64_t)(timing & 3u) << 15 | (uint64_t)(core->result.desc_index & 63u) << 17 | (uint64_t)e << 23;
}
static inline uint64_t os_sd_where_aux(const struct sunxi_mmc *core, unsigned first_units)
{ return (uint64_t)((core->result.tbc0 / 512u) & 511u) | (uint64_t)(first_units & 63u) << 9; }
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
        word |= ((uint64_t)(e->error & 15u) | (uint64_t)(e->protocol & 63u) << 4 | (uint64_t)e->interrupts << 10) << (32 * seen++);
    }
    return word;
}
#endif
