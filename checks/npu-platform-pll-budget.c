/* Count PLL_NPU preparation work in the existing PLL register model. One poll
 * stands for one bootstrap tick, as in the native bootstrap loop. Pass
 * "budget" to also enforce the limits this controller meets. */
#define main pll_model_main
#include "npu-pll.c"
#undef main

static unsigned clock_reads, lock_enters;
static int counted_now(void *v, uint64_t *out) { ++clock_reads; return now(v, out); }
static int counted_enter(void *v, uint64_t d) { ++lock_enters; return enter(v, d); }
struct cost { unsigned polls, reads, writes, clock_reads, locks, samples; };
static void prepare(struct model *m, enum a733_npu_pll_mode mode)
{
    setup(m, A733_NPU_PLL_ENABLED_RETUNE);
    if (mode == A733_NPU_PLL_LIVE_ENABLE) {
        /* Live-DRAM entry: LDO on, PLL, lock detector and output off. */
        m->regs[0x2a0/4] = 0x40104520;
        m->authorized.mode = mode; m->authorized.initial_control = m->regs[0x2a0/4];
        memset(&m->p, 0, sizeof(m->p));
        assert(a733_npu_pll_init(&m->p, &m->authorized, &ops, m));
    }
    m->authorized.deadline_us = m->p.config.deadline_us = 100000000;
    m->p.ops.now_us = counted_now; m->p.ops.enter = counted_enter;
}
static struct cost run_ticks(struct model *m)
{
    unsigned reads = m->reads, writes = m->writes, nows = clock_reads, locks = lock_enters, polls = 0;
    enum a733_npu_pll_result r = A733_NPU_PLL_PENDING;
    while (r != A733_NPU_PLL_READY && r != A733_NPU_PLL_RETAINED && polls < 200) {
        r = a733_npu_pll_poll(&m->p); ++polls; m->now += 1000;
    }
    assert(r == A733_NPU_PLL_READY && !m->retained && !m->locked);
    return (struct cost){polls, m->reads - reads, m->writes - writes, clock_reads - nows,
        lock_enters - locks, m->p.samples};
}
int main(int argc, char **argv)
{
    int budget = argc == 2 && !strcmp(argv[1], "budget");
    static const struct { enum a733_npu_pll_mode mode; const char *name; } modes[] = {
        {A733_NPU_PLL_ENABLED_RETUNE, "retune"}, {A733_NPU_PLL_LIVE_ENABLE, "live-enable"}};
    for (unsigned i = 0; i < 2; ++i) {
        struct model m; prepare(&m, modes[i].mode);
        struct cost c = run_ticks(&m);
        printf("%s polls=%u reads=%u writes=%u clock_reads=%u locks=%u lock_samples=%u\n",
            modes[i].name, c.polls, c.reads, c.writes, c.clock_reads, c.locks, c.samples);
        if (budget) {
            /* Acquisition, the locked enable pass, three LOCK samples, then the
             * settled output step: one lock and at most four clock samples per
             * pass. The register pass shared with local startup, which the
             * design's combined budget needs, is not part of this controller. */
            assert(c.polls <= 6 && c.locks <= 5 && c.samples == 3);
            assert(c.clock_reads <= 16 + c.samples && c.reads <= 45);
        }
    }
    if (budget) puts("PASS NPU PLL preparation within its pass and sample limits");
    return 0;
}
