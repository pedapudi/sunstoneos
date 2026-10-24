/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BOARD_BUNDLES_POWER_TRACE_H
#define SUNSTONE_BOARD_BUNDLES_POWER_TRACE_H
/* Records of bundles 6 and 7 (docs/designs/board-bundles-next.md), kept
 * free of FreeRTOS and hardware so that the host model runs them unchanged.
 *
 * The TWI observer sees every S_TWI0 access the PMIC provider makes, in
 * order, and classifies the reads of the control register CNTR (+0x0c):
 * after a START or repeated START (CNTR write 0x68) or a byte (0x48), each
 * read is a poll of the interrupt flag (bit 3) until the flag reads 1; after
 * a STOP (0x58), each read is a poll of the STOP bit (bit 4) until it reads
 * 0; any other read (the provider's idle checks) is neither. Every CNTR read
 * also records the interrupt enable (bit 7), which stays 0: the provider
 * never writes it, and no GIC register is read for this record. */
#include <stdint.h>

#define BB_TWI_CNTR UINT32_C(0x0708300c)
struct bb_twi {
    unsigned mode;                      /* 0 none, 1 flag poll, 2 STOP poll */
    unsigned condition;                 /* the pending flag poll follows a START */
    unsigned polls;                     /* reads in the pending poll */
    uint32_t enable_seen;               /* OR of CNTR bit 7 over every read */
    /* Cumulative counts. */
    unsigned conditions, bytes, stops;  /* completed START/RESTART, byte and STOP phases */
    unsigned first_set, first_clear;    /* flag phases whose first read had the flag set or clear */
    unsigned polls_max, reads;
};
void bb_twi_observe(struct bb_twi *, unsigned write, uintptr_t address, uint32_t value);
/* Wire time of completed phases, in bit times: two per START, repeated START
 * or STOP and nine per byte with its acknowledgement, as the provider's work
 * query and checks/power-domains-service.c count them. */
static inline uint64_t bb_twi_wire_bits(unsigned conditions, unsigned bytes, unsigned stops)
{ return 2ull * conditions + 9ull * bytes + 2ull * stops; }

/* Domain status read back to back after the ON command, with CNTPCT stamps
 * relative to the command's completion, until the status reads ON (8) or the
 * bound passes. Each change of the status value is recorded. */
#define BB_STATUS_CHANGES 16u
struct bb_status_trace {
    unsigned reads, changes, on, bounded;
    uint32_t first;
    uint64_t on_at, last_at;
    struct { uint32_t value; uint64_t at; } change[BB_STATUS_CHANGES];
};
void bb_status_spin(int (*read)(void *, uintptr_t, uint32_t *), void *context, uint64_t (*now)(void),
    uintptr_t status, uint64_t start, uint64_t bound, struct bb_status_trace *);
#endif
