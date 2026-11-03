#ifndef SUNSTONE_OS_SD_NOTE_H
#define SUNSTONE_OS_SD_NOTE_H
/* What a failed bounded write leaves in the failure record that survives the watchdog reset (apps/os/large_sd.c write_fail, apps/os/sd_owner.c). The record's value
 * word carries the stage in bits 5:0 and, for stages 33 and 34, the core's error in 9:6, its protocol step in 15:10 and the low 16 bits of the controller's raw
 * interrupt status at the fault in 31:16; the record's aux field (15 bits) carries the pieces written so far in 12:0 and the bus timing in force in 14:13. */
#include "sunxi_mmc.h"
static inline uint64_t os_sd_write_detail(const struct sunxi_mmc *core)
{ return (uint64_t)(core->result.error & 15u) | (uint64_t)(core->protocol & 63u) << 4 | (uint64_t)(core->result.interrupts & 0xffffu) << 10; }
static inline uint64_t os_sd_write_aux(uint64_t pieces, unsigned timing) { return (pieces & 0x1fffu) | (uint64_t)(timing & 3u) << 13; }
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
