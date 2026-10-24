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
 * a STOP (0x58), each read is a poll of the STOP wait, which ends when the
 * STOP bit (bit 4) reads 0 and then the status register (+0x10) reads the
 * idle state 0xf8; any other read (the provider's idle checks) is neither.
 * Every CNTR read also records the interrupt enable (bit 7), which stays 0:
 * the provider never writes it, and no GIC register is read for this
 * record.
 *
 * Every log line of both images is built here in a bounded buffer. The
 * summary lines come first and end with the outcome line; the detail lines
 * after them are printed while they fit, so that the summary and outcome can
 * never be cut. The host model formats each line with every field at its
 * widest value and checks the bound. */
#include <stddef.h>
#include <stdint.h>

#define BB_TWI_CNTR UINT32_C(0x0708300c)
#define BB_TWI_STAT UINT32_C(0x07083010)
/* A snapshot is six read transactions, each ending in a STOP. */
#define BB_TWI_SNAPSHOT_STOPS 6u
struct bb_twi {
    unsigned mode;                      /* 0 none, 1 flag poll, 2 STOP bit poll, 3 idle status wait */
    unsigned condition;                 /* the pending flag poll follows a START */
    unsigned polls;                     /* CNTR reads in the pending phase */
    uint32_t enable_seen;               /* OR of CNTR bit 7 over every read */
    /* Cumulative counts. */
    unsigned conditions, bytes, stops;  /* completed START/RESTART, byte and STOP phases */
    /* Re-spins: CNTR polls of a phase beyond the second, that is, polls after
     * a service spin that ended before the phase did. The first poll reads
     * right after the phase-start write and the second after the spin. They
     * are counted for flag phases and STOP waits alike; respins holds both,
     * stop_respins the STOP waits' share. */
    unsigned respins, stop_respins;
    unsigned snapshot_respins;          /* re-spins since the last snapshot's final STOP */
    unsigned snapshot_respins_max, snapshots; /* most re-spins of one snapshot; snapshots ended */
    unsigned first_set, first_clear;    /* flag phases whose first read had the flag set or clear */
    unsigned polls_max, reads;
};
void bb_twi_observe(struct bb_twi *, unsigned write, uintptr_t address, uint32_t value);
/* Wire time of completed phases, in bit times: two per START, repeated START
 * or STOP and nine per byte with its acknowledgement, as the provider's work
 * query and checks/power-domains-service.c count them. */
static inline uint64_t bb_twi_wire_bits(unsigned conditions, unsigned bytes, unsigned stops)
{ return 2ull * conditions + 9ull * bytes + 2ull * stops; }
/* The same wire time in nanoseconds with a byte of byte_ns, which bundle 6
 * sets to its measured byte wire time when that exceeds nine bit times. */
static inline uint64_t bb_twi_wire_ns(unsigned conditions, unsigned bytes, unsigned stops, uint32_t bit_ns,
                                      uint32_t byte_ns)
{ return (2ull * conditions + 2ull * stops) * bit_ns + (uint64_t)bytes * byte_ns; }

/* One service step that completed a bus phase. */
struct bb_step {
    uint64_t ticks;                    /* CNTPCT across the step */
    uint32_t interrupts;               /* scheduler ticks inside it */
    uint16_t conditions, bytes, stops, first_set, first_clear, polls_max, respins;
};
/* Step evaluation against each step's measured wire time. Ratios round up
 * to thousandths using the counter ticks directly: at most 1000, 1050,
 * 1100, BB_BOUND_PERMILLE, then above. The provisional empirical limit is
 * 1268 permille for the median (larger middle value for an even count), and
 * the same wire-relative limit plus 40 us for the first step. It applies to
 * bundle 6 at CPU-L 494 MHz with its observer and write stamps enabled.
 * Re-spins are reported; see docs/designs/pmic-access.md. */
#define BB_BOUND_PERMILLE 1268u
#define BB_BOUND_FIRST_EXTRA_NS 40000u
#define BB_BOUND_STEPS 160u
struct bb_step_bins { unsigned bins[5], over_ms; uint64_t worst; };
struct bb_bound { uint32_t median; uint64_t first_ns, first_limit_ns; unsigned pass; };
void bb_steps_evaluate(const struct bb_step *, unsigned count, uint32_t bit_ns, uint32_t byte_ns,
                       struct bb_step_bins *, struct bb_bound *);
/* The byte time of the wire model. The measured byte is the lower bracket of
 * the wire-time spin: CNTPCT ticks from the return of a byte phase's CNTR
 * write to the last read that still saw INT_FLAG clear. The byte time is nine
 * bit times, or the measured byte when it is longer and at most 110% of nine
 * bit times. Returns 1 with *byte_ns set; returns 0 with nine bit times when
 * the spin never saw INT_FLAG set or the measured byte exceeds 110%, and the
 * bound then fails. */
#define BB_BYTE_LIMIT_PERMILLE 1100u
unsigned bb_byte_ns(uint32_t bit_ns, unsigned seen, uint64_t clear_ticks, uint32_t *byte_ns);

/* A register read back to back, with CNTPCT stamps relative to start, until
 * (value & mask) == done or the bound passes: the domain status after the ON
 * command (mask 15, done 8, ON) and S_TWI0 CNTR after a byte-phase write
 * (mask and done 8, INT_FLAG). Each change of the value is recorded with its
 * stamp and the stamp of the read before it. */
#define BB_STATUS_CHANGES 16u
struct bb_status_trace {
    unsigned reads, changes, on, bounded;
    uint32_t first;
    uint64_t on_at, on_before, last_at; /* the first done read, the read before it, the last read */
    struct { uint32_t value; uint64_t at, before; } change[BB_STATUS_CHANGES];
};
void bb_status_spin(int (*read)(void *, uintptr_t, uint32_t *), void *context, uint64_t (*now)(void),
    uintptr_t status, uint32_t mask, uint32_t done, uint64_t start, uint64_t bound, struct bb_status_trace *);

/* Fixed lines of both images, which the host model counts in the log bound. */
#define BB6_BEGIN "BEGIN board bundle 6: PMIC service and power-domain owner, read-only\n"
#define BB6_COMPLETE "COMPLETE board bundle 6: PMIC service and domain adoption reads\n"
#define BB6_BOUND_MISSED "FAILED board bundle 6: provisional step bound\n"
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

/* The parts of a provider register access, measured once by the service
 * task, which is the provider task, on its core and clock, with the I2C mutex
 * entered and the provider idle, at the park. Each window is `reads`
 * iterations timed by CNTPCT in BB_COST_SUBWINDOWS equal sub-windows; the
 * record keeps the total and the fastest and slowest sub-window, so that the
 * upper end of a cost's range is the slowest sub-window over its iterations.
 * All but the task-check window run with IRQs masked. Writes are timed in
 * place inside the snapshots' own transfers, so no extra bus write is made;
 * those two counter stamps and the observer call per write (about 60 per
 * snapshot) are inside the step times. */
#define BB_COST_SUBWINDOWS 8u
struct bb_window { uint32_t ticks, sub_min, sub_max; };
struct bb_cost {
    uint32_t reads, core;              /* iterations per window; MPIDR_EL1 Aff1 (the core number) of the service task */
    struct bb_window dsb;              /* CNTR reads through a733_mmio_read32_dsb_sy_both */
    struct bb_window plain;            /* plain CNTR reads, value discarded: issue throughput */
    struct bb_window dependent;        /* plain CNTR reads, each address depending on the last value: latency */
    struct bb_window time_read, adapter_read; /* the native adapter's now and read operations */
    struct bb_window task_check;       /* a733_task_context_owner_el1, IRQs unmasked */
    struct bb_window observer;         /* bb_twi_observe on a scratch record, as a flag poll that reads the flag clear */
    uint32_t task_ticks;               /* scheduler ticks inside the task-check window */
    uint32_t writes, write_min, write_max; /* writes timed in transfers, the fastest and the slowest */
    uint64_t write_ticks;              /* their sum */
    /* The wire-time spin, with IRQs masked: CNTR read back to back after one
     * byte-phase write until INT_FLAG sets. */
    uint32_t wire_reads, wire_seen;    /* reads; 1 when INT_FLAG was seen set */
    uint64_t wire_clear, wire_set;     /* ticks from the write's return to the last read with INT_FLAG clear, the first set */
};
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
    unsigned wire_excluded;             /* 1: the step that held the wire-time spin is left out */
    unsigned respins;                   /* respins over the recorded steps */
    unsigned stop_respins, snapshot_respins_max, snapshots; /* from the observer, over every transfer */
    uint32_t byte_ns, byte_ok;          /* byte time of the wire model; 0 when bb_byte_ns() refused the sample */
    struct bb_bound bound;
    struct bb_cost cost;
};
#define BB6_SUMMARY_LINES 23u
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
