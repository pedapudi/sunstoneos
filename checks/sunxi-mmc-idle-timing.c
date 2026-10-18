/* Reuse the established card/controller model without replaying its suite. */
#define main existing_controller_checks
#include "sunxi-mmc.c"
#undef main

static int fixed_now(void *context, uint64_t *out)
{ struct model *m = context; ++m->calls; *out = m->now; return 1; }

static void clock_ready(struct model *m, struct sunxi_mmc *h, unsigned kind)
{
    init(m, h, kind, 1); m->now = 100; h->config.ops.now_us = fixed_now;
    for (unsigned i = 0; i < 6; ++i) sunxi_mmc_poll(h);
    assert(h->state == SUNXI_MMC_IDENTIFY && !m->command_count);
    assert(m->reg[4/4] == (1u << 16));
}

static void idle_complete(struct model *m, struct sunxi_mmc *h)
{
    m->now = 1100;
    sunxi_mmc_poll(h); sunxi_mmc_poll(h);
    assert(m->command_count == 1 && m->commands[0] == 0);
    m->now = 1500;
    sunxi_mmc_poll(h); sunxi_mmc_poll(h);
    assert(h->state == SUNXI_MMC_IDENTIFY && m->command_count == 1);
}

int main(void)
{
    struct model m; struct sunxi_mmc h;
    for (unsigned kind = SUNXI_MMC_SD; kind <= SUNXI_MMC_EMMC; ++kind) {
        clock_ready(&m, &h, kind);
        m.now = 1099;
        for (unsigned i = 0; i < 3; ++i) sunxi_mmc_poll(&h);
        assert(!m.command_count);
        idle_complete(&m, &h);
        m.now = 3499;
        for (unsigned i = 0; i < 3; ++i) sunxi_mmc_poll(&h);
        assert(m.command_count == 1);
        m.now = 3500;
        sunxi_mmc_poll(&h); sunxi_mmc_poll(&h);
        assert(m.command_count == 2 && m.commands[1] == (kind == SUNXI_MMC_SD ? 8u : 1u));
        h.config.ops.now_us = now_us; run(&m);
        assert(h.state == SUNXI_MMC_READY);
    }
    clock_ready(&m, &h, SUNXI_MMC_SD);
    h.deadline_us = m.now = 1000; sunxi_mmc_poll(&h);
    terminal(&m, SUNXI_MMC_TIMEOUT, 0); assert(!m.command_count);

    clock_ready(&m, &h, SUNXI_MMC_SD); idle_complete(&m, &h);
    h.deadline_us = m.now = 3400; sunxi_mmc_poll(&h);
    terminal(&m, SUNXI_MMC_TIMEOUT, 0); assert(m.command_count == 1);

    clock_ready(&m, &h, SUNXI_MMC_SD); idle_complete(&m, &h);
    m.retain_result = 0; sunxi_mmc_poll(&h);
    terminal(&m, SUNXI_MMC_ADMISSION, 0); assert(m.command_count == 1);

    init(&m, &h, SUNXI_MMC_SD, 1);
    h.config.ops.now_us = fixed_now; m.now = UINT64_MAX - 500; h.deadline_us = UINT64_MAX;
    for (unsigned i = 0; i < 6; ++i) sunxi_mmc_poll(&h);
    terminal(&m, SUNXI_MMC_TIME, 0); assert(!m.command_count);
    puts("PASS: SD/eMMC CMD0 pacing, both wait deadlines, retained admission loss and time overflow");
    return 0;
}
