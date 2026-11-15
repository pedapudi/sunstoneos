/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_CE_CLOCK_H
#define SUNSTONE_A733_CE_CLOCK_H
#include "a733_ce.h"
enum a733_ce_clock_state { A733_CE_CLOCK_NEW,A733_CE_CLOCK_ATTEMPTING,
    A733_CE_CLOCK_READY,A733_CE_CLOCK_RETAINED,A733_CE_CLOCK_RETIRED };
struct a733_ce_clock_ops {
    int (*authorize)(void *); /* Actual task, sealed cold admission and live board. */
    int (*now_us)(void *,uint64_t *);
    int (*enter)(void *,uint64_t);
    void (*leave)(void *);
    void (*retain)(void *);
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
};
struct a733_ce_clock {
    struct a733_ce_clock *self;
    struct a733_ce_config config;
    struct a733_ce_clock_ops ops;
    struct a733_clock_leases *registry;
    struct a733_clock_token token;
    void *cookie;
    uint64_t last_now;
    enum a733_ce_clock_state state;
    unsigned held,writes;
    unsigned fail_line; /* The source line of the first failure; zero while healthy. */
};
/* Zero once. One designated task under the existing CE pool mutex. The caller
 * already owns CE exclusively and OSC/REF/PERI0/FABRIC continuously. Initial
 * both-world exclusion, prior DMA resolution and CCU write permission are
 * independently authenticated; reset-held readback establishes none of them.
 * Only fixed reset-held/gated -> CE400 is supported. No rollback or release. */
int a733_ce_clock_provision(struct a733_ce_clock *,const struct a733_ce_config *,
    const struct a733_ce_clock_ops *,void *,struct a733_clock_leases *,
    const struct a733_clock_token *);
/* The end of the owner's lifetime: a READY CE whose controller the caller has proved idle (a733_ce_idle, no task in
 * flight) is taken down by the same reset-and-gate cycle that provision applies to a CE found enabled, each step read
 * back, under the same register lock and deadline. 1: reset-held and gated with its MBUS gate closed and the neighbors
 * as found; state RETIRED, and the caller may release its clock token. 0: a refusal before any write leaves the CE
 * READY, the lock free and the token kept; a failure after a write retains the lock and the token (state RETAINED).
 * Nothing brings a retired CE back up. */
int a733_ce_clock_retire(struct a733_ce_clock *);
/* Separate from the operation adapter: only monotonic fixed local CE enables
 * and MBUS bit2, with all neighboring/readback bits preserved. */
int a733_ce_clock_native_write(void *,uintptr_t,uint32_t);
#endif
