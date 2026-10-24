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
 * never writes it, and no GIC register is read for this record.
 *
 * Every log line of both images is built here in a bounded buffer. The
 * summary lines come first and end with the outcome line; the detail lines
 * after them are printed while they fit, so that the summary and outcome can
 * never be cut. The host model formats each line with every field at its
 * widest value and checks the bound. */
#include <stddef.h>
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

/* One service step that completed a bus phase. */
struct bb_step {
    uint64_t ticks;                    /* CNTPCT across the step */
    uint32_t interrupts;               /* scheduler ticks inside it */
    uint16_t conditions, bytes, stops, first_set, first_clear, polls_max;
};
/* The distribution of steps against the wire time of their phases at bit
 * time bit_ns, in thousandths: at most 1000, 1050, 1100, above; steps longer
 * than 1.1 ms; and the largest ratio. */
struct bb_step_bins { unsigned bins[4], over_ms; uint64_t worst; };
void bb_step_bins(const struct bb_step *, unsigned count, uint32_t bit_ns, struct bb_step_bins *);

/* Domain status read back to back after the ON command, with CNTPCT stamps
 * relative to the command's completion, until the status reads ON (8) or the
 * bound passes. Each change of the status value is recorded with its stamp
 * and the stamp of the read before it. */
#define BB_STATUS_CHANGES 16u
struct bb_status_trace {
    unsigned reads, changes, on, bounded;
    uint32_t first;
    uint64_t on_at, last_at;
    struct { uint32_t value; uint64_t at, before; } change[BB_STATUS_CHANGES];
};
void bb_status_spin(int (*read)(void *, uintptr_t, uint32_t *), void *context, uint64_t (*now)(void),
    uintptr_t status, uint64_t start, uint64_t bound, struct bb_status_trace *);

/* Fixed lines of both images, which the host model counts in the log bound. */
#define BB6_BEGIN "BEGIN board bundle 6: PMIC service and power-domain owner, read-only\n"
#define BB6_COMPLETE "COMPLETE board bundle 6: PMIC service and domain adoption reads\n"
#define BB7_BEGIN "BEGIN board bundle 7: domain 1 ON through the owner\n"
#define BB7_ON "COMPLETE board bundle 7: domain 1 ON\n"
#define BB7_REFUSED "COMPLETE board bundle 7: inconclusive, the owner refused domain 1 before any PPU write\n"
#define BB7_FAILED "FAILED board bundle 7: domain 1\n"
#define BB7_TRACE_END "TRACE END board bundle 7\n"

/* One log line, NUL-terminated and ending in a newline. */
#define BB_LINE_MAX 128u
struct bb_line { char text[BB_LINE_MAX]; unsigned n; };
void bb_line_start(struct bb_line *, const char *);
void bb_line_dec(struct bb_line *, const char *name, uint64_t);
void bb_line_hex(struct bb_line *, const char *name, uint64_t);
void bb_line_end(struct bb_line *);

/* Bundle 6's summary. */
struct bb6_summary {
    uint32_t divisor, bit_ns, state, mapped, par_attributes, par_shareability;
    uint64_t par_physical;
    unsigned dcdc2, dcdc4, unchanged;
    uint32_t dcdc2_first, dcdc4_first;  /* (enable << 8) | selector of the first snapshot of each */
    unsigned steps, dropped, twi_reads;
    uint32_t enable_seen;
    struct bb_step_bins bins;
    uint32_t ppu, ahb, mbus, policy[3], status[3];
    int check[3];
};
#define BB6_SUMMARY_LINES 10u
unsigned bb6_summary_lines(const struct bb6_summary *, struct bb_line *out);
/* Bundle 7's summary. */
struct bb7_summary {
    unsigned row_state, domain_state, accesses, writes, owner_first, before_read, after_read;
    uint32_t state;
    struct bb_status_trace status;
};
#define BB7_SUMMARY_LINES 4u
unsigned bb7_summary_lines(const struct bb7_summary *, struct bb_line *out);
/* Detail lines. */
void bb_step_line(const struct bb_step *, struct bb_line *);
void bb_access_line(uint32_t address, uint32_t value, unsigned write, uint64_t at, struct bb_line *);
void bb_status_line(const struct bb_status_trace *, unsigned i, struct bb_line *);
void bb_word_line(const char *prefix, uint32_t offset, uint32_t value, struct bb_line *);
void bb_snapshot_line(unsigned i, uint32_t enable_selector, struct bb_line *);
#endif
