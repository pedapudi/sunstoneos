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
#endif
