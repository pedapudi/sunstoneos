/* SPDX-License-Identifier: Apache-2.0 */
/* Run the application's rail helper with the actual PMIC provider, clock
 * registry and rail ledger. Only bus registers, time and task yielding are
 * modeled, using the existing provider fixture. */
#define PMIC_MODEL_NO_MAIN 1
#include "a733-pmic.c"
#include "a733_cpu_clock.h"
#include "a733_range.h"
#include <setjmp.h>

#define VOLT_OWNER 6u
#define VOLT_SETTLE_US 1000u
static struct pmic_model pm;
static jmp_buf failure;
static const char *failed;
static uint64_t session_end, change_end, write_at[2];
static unsigned submissions, completions, changes, writes, active_domain, clock_reads;
static unsigned late_clock, bad_readback, late_settle, fail_final;
static uint8_t written_rails[4];

static a733_clock_mask_t clocks(unsigned d)
{ return d == A733_CPU_BIG ? A733_CLOCK_CPU_B : A733_CLOCK_CPU_L | A733_CLOCK_CPU_DSU; }
static int held(a733_clock_mask_t mask)
{
    for (unsigned i = 0; i < A733_CLOCK_CLIENTS; ++i)
        if (pm.clocks.slots[i].owner == VOLT_OWNER && (pm.clocks.slots[i].exclusive & mask) == mask)
            return 1;
    return 0;
}
static uint64_t bb_power_now(void) { return pm.now; }
static void bb_power_pause(const char *what)
{
    (void)what;
    pm.now += 10;
    (void)pmic_model_step(&pm);
}
static uint64_t request_deadline(void)
{
    ++changes;
    change_end = pm.now + 120000 < session_end ? pm.now + 120000 : session_end;
    return change_end;
}
static _Noreturn void fail(const char *what) { failed = what; longjmp(failure, 1); }
#define REQUIRE(v, what) do { if (!(v)) fail(what); } while (0)
static int cpu_read(void *unused, uintptr_t address, uint32_t *value)
{
    (void)unused;
    assert(held(clocks(active_domain)));
    assert(address >= 0x08871000 && address < 0x08874000);
    ++clock_reads;
    *value = *pmic_model_word(&pm, address);
    return 1;
}
static void written(void *unused, uint8_t reg, uint8_t value)
{
    (void)unused;
    assert(reg == 0x14 || reg == 0x16);
    assert(held(clocks(reg == 0x14 ? A733_CPU_BIG : A733_CPU_LITTLE)));
    assert(value == 0x9e || (reg == 0x14 && value == 0xb7));
    assert(writes < sizeof(written_rails));
    written_rails[writes++] = reg;
    write_at[reg == 0x14] = pm.now;
    if (bad_readback)
        pm.pmic[reg] = 0x9f;
    if (late_settle)
        pm.now = change_end - 500;
}
static enum a733_pmic_status submit(struct a733_pmic *provider, uint64_t owner,
    const struct a733_rail_token *token, enum a733_rail rail, uint64_t end, struct a733_pmic_request *request)
{
    assert(end == change_end && held(clocks(active_domain)));
    ++submissions;
    if (fail_final && submissions == 3)
        pm.pmic[rail == A733_RAIL_DCDC3 ? 0x14 : 0x16] = 0x9f;
    if (submissions == 3)
        assert(pm.now - write_at[rail == A733_RAIL_DCDC3] >= 1000);
    return a733_pmic_submit(provider, owner, token, rail, end, request);
}
static enum a733_pmic_status submit_write(struct a733_pmic *provider, uint64_t owner,
    const struct a733_rail_token *token, enum a733_rail rail, uint8_t expected, uint8_t target,
    uint64_t end, struct a733_pmic_request *request)
{
    assert(end == change_end && held(clocks(active_domain)) && submissions == 1);
    ++submissions;
    return a733_pmic_submit_write(provider, owner, token, rail, expected, target, end, request);
}
static enum a733_pmic_status tracked_result(struct a733_pmic *provider, const struct a733_pmic_request *request,
                                   struct a733_pmic_snapshot *snapshot)
{
    assert(held(clocks(active_domain)));
    enum a733_pmic_status status = a733_pmic_result(provider, request, snapshot);
    if (status == A733_PMIC_COMPLETE && ++completions == 1 && late_clock)
        *pmic_model_word(&pm, late_clock) ^= 1;
    return status;
}
/* Supply the application helper's existing provider field names. These
 * aliases do not replace provider behavior; the wrappers above call it. */
#define bb_power pm
#define pmic provider
#define a733_pmic_submit submit
#define a733_pmic_submit_write submit_write
#define a733_pmic_result tracked_result
#include "../apps/board-bundles-power/cpu-rail-change.h"
#undef a733_pmic_result
#undef a733_pmic_submit_write
#undef a733_pmic_submit
#undef pmic
#undef bb_power

static void setup(void)
{
    pmic_model_setup(&pm);
    for (unsigned i = 0; i < 16 && a733_pmic_live(&pm.provider, &pm.lifetime) != A733_PMIC_READY; ++i)
        assert(pmic_model_step(&pm) != A733_PMIC_RETAINED);
    assert(a733_pmic_live(&pm.provider, &pm.lifetime) == A733_PMIC_READY);
    static const uint32_t boot[3][2] = {
        {0xf8802600, 0x03010000}, {0xf8802700, 0x03000000}, {0xf8802600, 0x03010105}};
    for (unsigned d = 0; d < 3; ++d) {
        *pmic_model_word(&pm, 0x08871000 + 0x1000 * d) = boot[d][0];
        *pmic_model_word(&pm, 0x0887101c + 0x1000 * d) = boot[d][1];
    }
    pm.pmic[0x14] = 0xb7;
    pm.pmic[0x16] = 0xb2;
    pm.pmic_written = written;
    session_end = pm.now + 1500000;
    failed = NULL;
    submissions = completions = changes = writes = clock_reads = late_clock = bad_readback = late_settle = fail_final = 0;
    cpu_rail_state.rail = cpu_rail_state.stage = 0;
}
static void change(unsigned d, unsigned from, unsigned to)
{
    submissions = completions = 0;
    active_domain = d;
    struct a733_pmic_snapshot snapshot;
    cpu_rail_change((enum a733_cpu_domain)d, from, to, &snapshot);
    assert(snapshot.programmed_uv == to * 1000 && submissions == 3 && completions == 3);
    assert(!held(clocks(d)) && cpu_rail_state.stage == CPU_RAIL_IDLE);
}
static void restore(void)
{
    change(A733_CPU_BIG, 1050, 800);
    change(A733_CPU_LITTLE, 1000, 800);
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "os-return-dcdc3-800")) {
        setup(); pm.pmic[0x14] = 0x9e;
        assert(!setjmp(failure));
        change(A733_CPU_LITTLE, 1000, 800);
        assert(!failed && writes == 1 && changes == 1 && clock_reads == 20 && written_rails[0] == 0x16);
        assert(pm.pmic[0x14] == 0x9e && pm.pmic[0x16] == 0x9e);
        puts("PASS OS return rails: DCDC3 unchanged at 800 mV; one DCDC5 write, three requests, 20 clock reads");
        return 0;
    }
    setup();
    assert(!setjmp(failure));
    const uint64_t start = pm.now;
    restore();
    assert(!failed && writes == 2 && changes == 2 && clock_reads == 30 &&
           written_rails[0] == 0x14 && written_rails[1] == 0x16);
    assert(pm.pmic[0x14] == 0x9e && pm.pmic[0x16] == 0x9e);
    printf("PASS restore: two selector writes, six helper requests, 30 clock reads, no clock writes, elapsed_us=%llu\n",
           (unsigned long long)(pm.now - start));
    /* Existing bundle 21 round trip. */
    setup(); pm.pmic[0x14] = pm.pmic[0x16] = 0x9e;
    assert(!setjmp(failure));
    change(A733_CPU_BIG, 800, 1050);
    assert(pm.pmic[0x14] == 0xb7 && pm.pmic[0x16] == 0x9e);
    change(A733_CPU_BIG, 1050, 800);
    assert(writes == 2 && pm.pmic[0x14] == 0x9e);
    puts("PASS existing DCDC3 raise and lowering");
    /* Bundle 30 raises DCDC3 after CPU-L and the DSU reach their top rates. */
    setup(); pm.pmic[0x14] = 0x9e;
    *pmic_model_word(&pm, 0x08871000) = 0xf8804500;
    *pmic_model_word(&pm, 0x0887101c) = 0x03000000;
    *pmic_model_word(&pm, 0x08873000) = 0xf8803100;
    *pmic_model_word(&pm, 0x0887301c) = 0x03000105;
    assert(!setjmp(failure));
    change(A733_CPU_BIG, 800, 1050);
    assert(writes == 1 && pm.pmic[0x14] == 0xb7 && pm.pmic[0x16] == 0xb2);
    puts("PASS existing DCDC3 raise with CPU-L and the DSU at top rates");
    /* Bundle 33 entries. The application skips a rail already at 800 mV,
     * so each case calls the helper only for the rails above it. */
    static const struct { const char *name; uint8_t dcdc5, dcdc3; unsigned five_from, three_from; } entries[] = {
        {"1000/1050", 0xb2, 0xb7, 1000, 1050}, {"800/1050", 0x9e, 0xb7, 0, 1050},
        {"1000/800", 0xb2, 0x9e, 1000, 0}, {"900/950", 0xa8, 0xad, 900, 950}, {"800/800", 0x9e, 0x9e, 0, 0}};
    for (unsigned c = 0; c < sizeof(entries) / sizeof(*entries); ++c) {
        setup(); pm.pmic[0x16] = entries[c].dcdc5; pm.pmic[0x14] = entries[c].dcdc3;
        assert(!setjmp(failure));
        if (entries[c].three_from)
            change(A733_CPU_BIG, entries[c].three_from, 800);
        if (entries[c].five_from)
            change(A733_CPU_LITTLE, entries[c].five_from, 800);
        const unsigned expected = !!entries[c].three_from + !!entries[c].five_from;
        assert(!failed && writes == expected && changes == expected && pm.pmic[0x14] == 0x9e && pm.pmic[0x16] == 0x9e);
        assert(!entries[c].three_from || written_rails[0] == 0x14);
        printf("PASS entry DCDC5/DCDC3 %s mV: %u selector writes, both rails 0x9e\n", entries[c].name, writes);
    }
    /* Targets the helper refuses before any lease or bus request: no change,
     * above the bin's top point on either rail, off the 10 mV grid. */
    static const struct { unsigned domain, from, to; } bad[] = {
        {A733_CPU_BIG, 800, 800}, {A733_CPU_LITTLE, 800, 800}, {A733_CPU_BIG, 1060, 800},
        {A733_CPU_LITTLE, 1010, 800}, {A733_CPU_LITTLE, 1050, 800}, {A733_CPU_BIG, 905, 800},
        {A733_CPU_DSU, 1000, 800}, {A733_CPU_LITTLE, 800, 1000}};
    for (unsigned c = 0; c < sizeof(bad) / sizeof(*bad); ++c) {
        setup();
        if (!setjmp(failure)) {
            active_domain = bad[c].domain;
            struct a733_pmic_snapshot snapshot;
            cpu_rail_change((enum a733_cpu_domain)bad[c].domain, bad[c].from, bad[c].to, &snapshot);
            assert(!"target must be refused");
        }
        assert(failed && !strcmp(failed, "CPU rail target") && !writes && !changes && !submissions);
        assert(!held(clocks(A733_CPU_BIG)) && !held(clocks(A733_CPU_LITTLE)));
        assert(pm.pmic[0x14] == 0xb7 && pm.pmic[0x16] == 0xb2);
    }
    printf("PASS %u refused targets: no lease, request or selector write\n", (unsigned)(sizeof(bad) / sizeof(*bad)));
    /* Each request names its own failure, so a log says whether the write
     * was submitted; stages: 1 read, 2 write, 3 settle, 4 final. */
    static const char *const reasons[] = {"CPU rail boot clocks", "CPU rail boot clocks", "CPU rail before write",
        "CPU rail write result", "CPU rail read result deadline", "CPU rail settle deadline", "CPU rail after settle",
        "CPU rail boot clocks", "CPU rail boot clocks", "CPU rail clock lease", "CPU rail write result",
        "CPU rail settle deadline", "CPU rail after settle", "CPU rail before write", "CPU rail boot clocks"};
    static const unsigned stages[] = {1, 2, 1, 2, 1, 3, 4, 1, 2, 1, 2, 3, 4, 1, 2};
    for (unsigned c = 0; c < sizeof(reasons) / sizeof(*reasons); ++c) {
        setup();
        switch (c) {
        case 0: *pmic_model_word(&pm, 0x08872000) = 0xf8804d00; break;
        case 1: late_clock = 0x0887201c; break;
        case 2: pm.pmic[0x14] = 0xb6; break;
        case 3: bad_readback = 1; break;
        case 4: session_end = pm.now + 20; break;
        case 5: late_settle = 1; break;
        case 6: fail_final = 1; break;
        case 7: *pmic_model_word(&pm, 0x08873008) = 1u << 31; break;
        case 8: late_clock = 0x0887301c; break;
        case 9: {
            struct a733_clock_token other = {0};
            assert(a733_clock_acquire(&pm.clocks, 99, 0, A733_CLOCK_CPU_DSU, session_end, &other) == A733_CLOCK_OK);
            break;
        }
        /* DCDC5 with DCDC3 already lowered: readback mismatch, settle
         * deadline, changed final snapshot, entry other than the caller's
         * reading, and CPU-L leaving its boot word before the write. */
        case 10: bad_readback = 1; break;
        case 11: late_settle = 1; break;
        case 12: fail_final = 1; break;
        case 13: pm.pmic[0x16] = 0xa8; break;
        case 14: late_clock = 0x08871000; break;
        }
        const int five = c >= 7;
        if (c >= 10)
            pm.pmic[0x14] = 0x9e;
        const uint8_t dcdc5 = pm.pmic[0x16];
        if (!setjmp(failure)) {
            if (five)
                change(A733_CPU_LITTLE, 1000, 800);
            else
                restore();
            assert(!"fault must stop the sequence");
        }
        const int wrote = c == 3 || c == 5 || c == 6 || c == 10 || c == 11 || c == 12;
        assert(failed && !strcmp(failed, reasons[c]) && changes == 1 && cpu_rail_state.stage == stages[c]);
        assert(cpu_rail_state.rail == (five ? A733_RAIL_DCDC5 : A733_RAIL_DCDC3));
        assert(writes == (unsigned)wrote && (!wrote || written_rails[0] == (five ? 0x16 : 0x14)));
        /* An unwritten DCDC5 keeps its entry selector; a written one holds
         * the readback the model produced (0x9f for a mismatch). */
        assert(five && wrote ? pm.pmic[0x16] == (c == 11 ? 0x9e : 0x9f) : pm.pmic[0x16] == dcdc5);
        printf("PASS fault %u: %s, selector_writes=%u\n", c, failed, writes);
    }
    return 0;
}
