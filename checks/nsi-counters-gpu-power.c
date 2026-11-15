/* SPDX-License-Identifier: Apache-2.0 */
/* The NSI port 6 counter guard (drivers/dram/a733_nsi_counters_guard.c) on the GPU power path: the actual GPU
 * platform and power controller (drivers/gpu/a733_gpu_power.c, built with SUNSTONE_ACCEL_NSI_COUNTERS) cycle the
 * modeled GPU_TOP (PCK domain 5) of checks/a733-gpu-second-on.c, and a model of NSI banks 6 and 18 records every
 * access with the state of GPU_TOP at that moment.
 *
 * A bank 0 (port 6) access is legal only while TOP5's policy and status both read ON (8). The OFF command is the
 * domain owner's TOP5 policy write of 0; from it until the next ON every bank 0 access is a violation. The cases:
 *   1. ON, OFF, ON with the counters never opened: the guard makes no access.
 *   2. OPEN before the GPU's first power-on: refused with no access.
 *   3. OFF with both banks counting: bank 0 is restored, with every word read back, before the OFF command; bank 1
 *      keeps counting; no bank 0 access follows the OFF command.
 *   4. While GPU_TOP is off: OPEN, BEGIN, SAMPLE and END on bank 0 are refused with no access; bank 1 still works.
 *   5. A power-off that arrives during a call on bank 0 (from another core): the bank is refused at once and the
 *      call's later bank 0 accesses never reach the bus; the OFF that follows makes none either.
 *   6. ON again: the bank is back at its hardware reset, and OPEN, BEGIN, SAMPLE, END and CLOSE work on both banks.
 *   7. A power-on that keeps port 6's words (the model's reset_at_on cleared), after a power-off that arrived during a
 *      call on bank 1 while bank 0 counted: the power-on restores bank 0's CYCLE, CLR and ENABLE, with GPU_TOP on,
 *      before any call, and the owner then closes without a failure.
 *   8. A power-on that arrives while a call on bank 1 holds the owner: that call restores the stale bank 0 before it
 *      returns, and BEGIN on bank 0 then works.
 * checks/nsi-counters-gpu-power.py builds this with the restore moved after the OFF command, without the gate test,
 * without the power-on restore and without the holder's restore, and requires each build to fail.
 */
#define main a733_gpu_second_on_main
#include "a733-gpu-second-on.c"
#undef main
#include "a733_nsi_counters.h"

int os_nsi_counters_call(struct a733_nsi_counter_call *);
void os_nsi_counters_power_off(unsigned bank);
void os_nsi_counters_power_on(unsigned bank);

#define WINDOW 60000000u
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static const uint32_t reset_word[2][9] = {
    {0x00000100u, 0, 0x1234u, 1, 2, 3, 4, 5, 6},
    {0x00000110u, 0, 0x1235u, 101, 102, 103, 104, 105, 106},
};
static struct {
    uint32_t word[2][9];
    unsigned accesses[2], violations, after_off;   /* after_off: bank 0 accesses since the last OFF command */
    unsigned seq, last_bank0, off_command;          /* sequence numbers of the last bank 0 access and the OFF */
    uint64_t now;
    int arm_power_off;                              /* the next tick calls the power-off, as another core would */
    unsigned arm_power_on;                          /* the tick this many ticks on calls the guard's power-on */
    int reset_at_on;                                /* a TOP5 power-on brings port 6's words back to reset */
    unsigned masked;
} n;

static int top5_on(void) { return (d.ppu[5][0] & 15) == 8 && (d.ppu[5][2] & 15) == 8; }
static int locate(uintptr_t a, unsigned *b, unsigned *w)
{
    for (unsigned i = 0; i < 2; ++i) {
        const uintptr_t first = a733_nsi_counter_address(i, 0);
        if (a >= first && a < first + 36 && !((a - first) & 3)) { *b = i; *w = (unsigned)((a - first) / 4); return 1; }
    }
    return 0;
}
static void note(unsigned b)
{
    ++n.accesses[b]; ++n.seq;
    CHECK(n.masked == 1);   /* every access runs with IRQs masked */
    if (b) return;
    n.last_bank0 = n.seq;
    if (!top5_on()) ++n.violations;
    if (n.off_command) ++n.after_off;
}
static int nread(void *ctx, uintptr_t a, uint32_t *v)
{
    (void)ctx; unsigned b, w;
    CHECK(locate(a, &b, &w)); note(b);
    *v = n.word[b][w];
    return 1;
}
static int nwrite(void *ctx, uintptr_t a, uint32_t v)
{
    (void)ctx; unsigned b, w;
    CHECK(locate(a, &b, &w) && w <= A733_NSI_W_CYCLE); note(b);
    if (w == A733_NSI_W_CLR) {
        if (v & 1u) for (unsigned i = 3; i < 9; ++i) n.word[b][i] = 0;
        n.word[b][w] = v & ~1u;
    } else n.word[b][w] = v;
    return 1;
}
/* Traffic on the ports: an enabled bank on a powered port counts. */
static void traffic(void)
{
    for (unsigned b = 0; b < 2; ++b)
        if (n.word[b][A733_NSI_W_ENABLE] & 1u && (b || top5_on())) for (unsigned i = 3; i < 9; ++i) n.word[b][i] += 1u + b;
}
static uint64_t nticks(void *ctx)
{
    (void)ctx;
    if (n.arm_power_off) { n.arm_power_off = 0; os_nsi_counters_power_off(0); }
    if (n.arm_power_on && !--n.arm_power_on) os_nsi_counters_power_on(0);
    traffic();
    return n.now += 8;
}
static const struct a733_nsi_counter_ops nops = {nread, nwrite, nticks};
const struct a733_nsi_counter_ops *os_nsi_counters_ops(void) { return &nops; }
uint64_t os_nsi_counters_mask(void) { CHECK(!n.masked); n.masked = 1; return 0x3c0; }
void os_nsi_counters_unmask(uint64_t mask) { CHECK(n.masked && mask == 0x3c0); n.masked = 0; }

/* The domain owner's TOP5 policy writes: 0 is the OFF command; 8 starts a power-on, which brings port 6's words back
 * to their reset values. */
static void written(uintptr_t a, uint32_t value)
{
    unsigned id, offset;
    if (!ppu_block(a, &id, &offset) || id != 5 || offset) return;
    if (!(value & 15)) { ++n.seq; n.off_command = n.seq; n.after_off = 0; }
    else if ((value & 15) == 8) {
        if (n.reset_at_on) memcpy(n.word[0], reset_word[0], sizeof(n.word[0]));
        n.off_command = 0;
    }
}
static int controls_reset(uint32_t w[2][9])
{
    for (unsigned b = 0; b < 2; ++b) if (memcmp(w[b], reset_word[b], 3 * sizeof(uint32_t))) return 0;
    return 1;
}
static int call(uint32_t op, uint32_t bank, struct a733_nsi_counter_call *c)
{
    memset(c, 0, sizeof(*c)); c->op = op; c->bank = bank; c->cycle = WINDOW;
    const int r = os_nsi_counters_call(c);
    CHECK(c->status == (uint32_t)r);
    return r;
}
static void gpu_on(void)
{
    CHECK(a733_gpu_platform_begin(&gpu, session_deadline) && step(a733_gpu_platform_poll, &gpu) == 1 && top5_on());
}
static void gpu_off(void)
{
    CHECK(a733_gpu_platform_off_begin(&gpu, session_deadline) && step(off_poll, &gpu) == 1 && !(d.ppu[5][2] & 15));
    CHECK(n.off_command);
}

int main(void)
{
    memcpy(n.word, reset_word, sizeof(n.word));
    n.reset_at_on = 1;
    struct a733_nsi_counter_call c;

    /* 2. Before the GPU's first power-on, port 6 is refused: OPEN reads both banks, so it is refused whole. */
    CHECK(call(A733_NSI_OPEN, 0, &c) == A733_NSI_C_REFUSED && !n.accesses[0] && !n.accesses[1]);

    /* 1. Two ON/OFF cycles of checks/a733-gpu-second-on.c, ending ON, with the counters never opened. */
    CHECK(run(0) == 1 && top5_on());
    CHECK(!n.accesses[0] && !n.accesses[1]);
    d.written = written;

    /* 3. Both banks counting, then a power-off. */
    CHECK(call(A733_NSI_OPEN, 0, &c) == A733_NSI_C_OK);
    CHECK(call(A733_NSI_BEGIN, 0, &c) == A733_NSI_C_OK && call(A733_NSI_BEGIN, 1, &c) == A733_NSI_C_OK);
    CHECK(n.word[0][A733_NSI_W_ENABLE] & 1u && n.word[1][A733_NSI_W_ENABLE] & 1u);
    CHECK(call(A733_NSI_SAMPLE, 0, &c) == A733_NSI_C_OK && c.value[0]);
    CHECK(call(A733_NSI_SAMPLE, 1, &c) == A733_NSI_C_OK);
    const uint32_t bank1_before = c.value[0];
    const unsigned before_off = n.accesses[0];
    gpu_off();
    /* The restore: ENABLE without bit 0, CYCLE, CLR, then ENABLE, each read back: 8 accesses, all before the OFF
     * command, with GPU_TOP on. */
    CHECK(n.accesses[0] == before_off + 8 && n.last_bank0 < n.off_command);
    CHECK(!n.violations && !n.after_off);
    CHECK(!memcmp(n.word[0], reset_word[0], 3 * sizeof(uint32_t)));
    CHECK(n.word[1][A733_NSI_W_ENABLE] & 1u);

    /* 4. GPU_TOP off: every call that would reach bank 0 is refused without an access; bank 1 still works. */
    const unsigned off_accesses = n.accesses[0];
    traffic();
    CHECK(call(A733_NSI_BEGIN, 0, &c) == A733_NSI_C_REFUSED);
    CHECK(call(A733_NSI_SAMPLE, 0, &c) == A733_NSI_C_REFUSED);
    CHECK(call(A733_NSI_END, 0, &c) == A733_NSI_C_REFUSED);
    CHECK(call(A733_NSI_OPEN, 0, &c) == A733_NSI_C_REFUSED);
    CHECK(n.accesses[0] == off_accesses && !n.violations);
    CHECK(call(A733_NSI_SAMPLE, 1, &c) == A733_NSI_C_OK && c.value[0] > bank1_before);
    CHECK(call(A733_NSI_END, 1, &c) == A733_NSI_C_OK && !memcmp(n.word[1], reset_word[1], 3 * sizeof(uint32_t)));
    /* CLOSE with only bank 1 left programmed reaches no bank 0 word. */
    CHECK(call(A733_NSI_CLOSE, 0, &c) == A733_NSI_C_OK && n.accesses[0] == off_accesses && !n.violations);

    /* 6. ON again: port 6 is back at its reset, and the counters work on both banks. */
    gpu_on();
    CHECK(!memcmp(n.word[0], reset_word[0], sizeof(n.word[0])));
    CHECK(call(A733_NSI_OPEN, 0, &c) == A733_NSI_C_OK && controls_reset(c.reset));
    CHECK(call(A733_NSI_BEGIN, 0, &c) == A733_NSI_C_OK && call(A733_NSI_BEGIN, 1, &c) == A733_NSI_C_OK);
    CHECK(call(A733_NSI_SAMPLE, 0, &c) == A733_NSI_C_OK && c.value[0]);
    CHECK(call(A733_NSI_END, 0, &c) == A733_NSI_C_OK && c.held[0] && c.held[0] == c.value[0]);
    CHECK(call(A733_NSI_END, 1, &c) == A733_NSI_C_OK);
    CHECK(call(A733_NSI_CLOSE, 0, &c) == A733_NSI_C_OK);
    CHECK(controls_reset(n.word) && !n.violations);

    /* 5. A power-off during END on bank 0: END disables the bank and reads its values, then the 10 us settle ticks;
     * the first tick calls the power-off as another core's GPU stop would. The bank is refused at once: END's held
     * reads and restore fail without an access, and the OFF that follows makes no access. */
    CHECK(call(A733_NSI_OPEN, 0, &c) == A733_NSI_C_OK && call(A733_NSI_BEGIN, 0, &c) == A733_NSI_C_OK);
    const unsigned before_end = n.accesses[0];
    n.arm_power_off = 1;
    CHECK(call(A733_NSI_END, 0, &c) == A733_NSI_C_IO && !n.arm_power_off);
    CHECK(n.accesses[0] == before_end + 2 + 6);   /* ENABLE written and read back, then the six values */
    gpu_off();
    CHECK(n.accesses[0] == before_end + 8 && !n.violations && !n.after_off);
    gpu_on();
    /* The failed owner closes with port 6 back at reset; a new OPEN then works. */
    CHECK(call(A733_NSI_CLOSE, 0, &c) == A733_NSI_C_IO);
    CHECK(call(A733_NSI_OPEN, 0, &c) == A733_NSI_C_OK && call(A733_NSI_CLOSE, 0, &c) == A733_NSI_C_OK);
    CHECK(!n.violations);

    /* 7. Bank 0 counting, then a power-off during BEGIN on bank 1 (its 10 us settle ticks). BEGIN on bank 1 completes;
     * bank 0 is refused with its words as BEGIN left them. The OFF makes no bank 0 access, and the power-on that
     * follows keeps the words, so only the guard's restore can bring them back. */
    CHECK(call(A733_NSI_OPEN, 0, &c) == A733_NSI_C_OK && call(A733_NSI_BEGIN, 0, &c) == A733_NSI_C_OK);
    n.arm_power_off = 1;
    CHECK(call(A733_NSI_BEGIN, 1, &c) == A733_NSI_C_OK && !n.arm_power_off);
    CHECK(n.word[0][A733_NSI_W_ENABLE] & 1u && n.word[0][A733_NSI_W_CYCLE] == WINDOW);
    gpu_off();
    CHECK(!n.violations && !n.after_off);
    n.reset_at_on = 0;
    const unsigned before_on = n.accesses[0];
    gpu_on();
    CHECK(n.accesses[0] == before_on + 8 && !n.violations);   /* the restore's four writes, each read back */
    CHECK(!memcmp(n.word[0], reset_word[0], 3 * sizeof(uint32_t)));
    CHECK(call(A733_NSI_END, 1, &c) == A733_NSI_C_OK && call(A733_NSI_CLOSE, 0, &c) == A733_NSI_C_OK);
    CHECK(controls_reset(n.word));

    /* 8. Bank 0 counting; during BEGIN on bank 1 the power-off and then the power-on arrive, as if GPU_TOP turned off
     * and on meanwhile. The power-on finds the owner held, so BEGIN on bank 1 restores bank 0 before it returns. */
    CHECK(call(A733_NSI_OPEN, 0, &c) == A733_NSI_C_OK && call(A733_NSI_BEGIN, 0, &c) == A733_NSI_C_OK);
    n.arm_power_off = 1;
    n.arm_power_on = 3;
    CHECK(call(A733_NSI_BEGIN, 1, &c) == A733_NSI_C_OK && !n.arm_power_off && !n.arm_power_on);
    CHECK(!memcmp(n.word[0], reset_word[0], 3 * sizeof(uint32_t)) && n.word[1][A733_NSI_W_ENABLE] & 1u);
    CHECK(call(A733_NSI_BEGIN, 0, &c) == A733_NSI_C_OK);   /* bank 0 is no longer programmed */
    CHECK(call(A733_NSI_END, 0, &c) == A733_NSI_C_OK && call(A733_NSI_END, 1, &c) == A733_NSI_C_OK);
    CHECK(call(A733_NSI_CLOSE, 0, &c) == A733_NSI_C_OK && controls_reset(n.word) && !n.violations);

    printf("PASS NSI port 6 guard on the GPU power path: no access with the counters unused; OPEN refused before the "
           "first ON; the OFF restores bank 0 (%u accesses, all before the OFF command) while bank 1 keeps counting; "
           "bank 0 calls refused with no access while GPU_TOP is off; a power-off during END refuses the rest of the "
           "call; ON again restores use of both banks; a power-on that keeps the words restores a bank refused during a call, "
           "and a call that holds the owner at the power-on restores it; %u bank 0 accesses in all, none with GPU_TOP off\n",
           8u, n.accesses[0]);
    return 0;
}
