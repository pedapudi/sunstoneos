/* UHS-I SDR104 state machine of the MMC core against a modeled UHS-I card and
 * controller. Reuses the accepted card model; its own cases never execute. */
#define main accepted_mmc_model_main
#include "sunxi-mmc.c"
#undef main
#include "sunxi-mmc-uhs-card.h"
static int uhs_module(void *v, uintptr_t token, unsigned divisor)
{
    struct uhs *u = v; struct model *m = &u->base;
    assert(token == 0xa733 && m->reg[4/4] == 0 && !(m->reg[0x18/4] & (1u << 31)) && u->width == 4);
    ++u->module_calls; u->divisor = divisor; return 1;
}
static int uhs_signal(void *v, uintptr_t token, unsigned operation, unsigned *state)
{
    struct uhs *u = v; struct model *m = &u->base; assert(token == 0xa733 && state);
    if (operation == SUNXI_MMC_SIGNAL_QUERY) { ++u->queries; *state = u->bad_caps ? 4 : u->caps; return 1; }
    if (operation == SUNXI_MMC_SIGNAL_SET) {
        assert(u->phase == 2 && m->reg[4/4] == 0 && !u->bank && !u->sets); ++u->sets;
        if (u->set_fails) return 0;
        u->bank = 1; *state = 0; return 1;
    }
    assert(operation == SUNXI_MMC_SIGNAL_POLL && u->bank && u->phase == 2); ++u->polls;
    if (u->bank == 1 && u->bank_polls && !--u->bank_polls) { u->bank = 2; u->restarted = m->now; }
    *state = u->bank == 2; return 1;
}
static void begin(struct uhs *u, struct sunxi_mmc *h, unsigned caps)
{
    memset(u, 0, sizeof(*u)); seed(&u->base, h, SUNXI_MMC_SD, 1);
    field(u->base.csd, 84, 12, 5 | (1u << 10)); field(u->base.csd, 96, 8, 0x32); u->scr[0] = 2; u->scr[1] = 5;
    u->caps = caps; u->s18a = 1; u->sdr104 = 1; u->bank_polls = 3; u->fatal_delay = 99; u->done_delay = 1000;
    u->window[0][0] = 20; u->window[0][1] = 24;
    struct sunxi_mmc_config c = {{uhs_read, uhs_write, barrier, now_us, admit, retained, uhs_module,
        uhs_read_fifo, model_write_fifo, uhs_signal}, u, 0xa733, SUNXI_MMC_SD, 800000, 0x300000,
        SUNXI_MMC_SD_UHS_SDR104};
    assert(sunxi_mmc_init(h, &c) && sunxi_mmc_open(h, 100000000));
}
static void drive(struct uhs *u, struct sunxi_mmc *h)
{
    for (unsigned i = 0; i < 400000; ++i) {
        if (h->state == SUNXI_MMC_READY || h->state == SUNXI_MMC_DONE || h->state == SUNXI_MMC_FAULT) return;
        u->base.fifo_this_poll = 0; sunxi_mmc_poll(h);
        if (u->clock_started && u->restart_gap) { u->base.now += u->restart_gap; u->restart_gap = 0; }
    }
    assert(!"UHS model did not reach a terminal state");
}
static void read_back(struct uhs *u, struct sunxi_mmc *h)
{
    uint8_t data[4096]; struct sunxi_mmc_result result;
    assert(sunxi_mmc_read(h, 8, 8, data, sizeof(data), h->last_us + 10000000)); drive(u, h);
    assert(h->state == SUNXI_MMC_DONE && sunxi_mmc_take(h, &result) && result.received == 4096);
    assert(u->base.reg[0x10/4] == 512 && u->base.reg[8/4] == UINT32_MAX);
    /* SDR104 reads start a block only with 512 bytes free; other timings
     * leave the read threshold disabled. */
    assert((u->base.reg[0x100/4] & 0x1fff0001u) == (h->card.timing == 3 ? (512u << 16) | 1u : 0u));
}
static void faulted(struct uhs *u, struct sunxi_mmc *h, enum sunxi_mmc_error error)
{
    drive(u, h); assert(h->state == SUNXI_MMC_FAULT && h->result.error == error);
    unsigned calls = u->base.calls; sunxi_mmc_poll(h); assert(calls == u->base.calls);
}
static void high_speed_33(struct uhs *u, struct sunxi_mmc *h, unsigned outcome)
{
    drive(u, h); assert(h->state == SUNXI_MMC_READY);
    assert(!u->cmd11 && !u->sets && !h->card.signal_1v8 && h->card.uhs_outcome == outcome);
    assert(h->card.timing == 1 && h->card.card_hz == 50000000 && u->divisor == 4 && u->sdr == 1 && !u->cmd6_uhs);
    read_back(u, h);
}
static void sdr25_18(struct uhs *u, struct sunxi_mmc *h, unsigned outcome)
{
    drive(u, h); assert(h->state == SUNXI_MMC_READY && h->card.signal_1v8 && h->card.uhs_outcome == outcome);
    assert(h->card.timing == 1 && h->card.card_hz == 50000000 && (u->divisor & 0xff) == 4 && u->sdr == 1);
    assert((u->base.reg[0x5c/4] & 0x80000331u) == 0x80000110u && u->base.reg[0x144/4] == 0x80);
    read_back(u, h);
}
int main(void)
{
    struct uhs *u = calloc(1, sizeof(*u)); struct sunxi_mmc h; assert(u);
    /* 150 MHz from 300 MHz: switch order, timing and a centered sample delay. */
    begin(u, &h, 3); drive(u, &h); assert(h.state == SUNXI_MMC_READY);
    assert(u->s18r_seen && u->cmd11 == 1 && u->cmd11_before_cmd2 && u->sets == 1 && u->dat0_masked);
    assert(h.card.signal_1v8 && h.card.uhs_outcome == SUNXI_MMC_UHS_SDR104 && h.card.timing == 3);
    assert(u->divisor == (SUNXI_MMC_MODULE_300M | 1u) && h.card.module_hz == 300000000 && h.card.card_hz == 150000000);
    assert(h.card.tuning_start == 20 && h.card.tuning_width == 24 && h.card.sample_delay == 32);
    assert(h.card.tuning_map == (((UINT64_C(1) << 24) - 1) << 20) && u->cmd19 == 24 * 20 + 40);
    assert(u->base.reg[0x144/4] == (0x80u | 32) && !(u->base.reg[0x5c/4] & 0x80000331u));
    assert(((u->base.reg[0x140/4] >> 16) & 3) == 3 && (u->base.reg[0x100/4] & 0x1fff0001u) == ((64u << 16) | 1u));
    read_back(u, &h); unsigned first = u->cmd19;
    /* 100 MHz from 400 MHz when the 300 MHz branch is absent. */
    begin(u, &h, 1); drive(u, &h);
    assert(h.state == SUNXI_MMC_READY && u->divisor == 2 && h.card.card_hz == 100000000 && h.card.timing == 3);
    /* Restart completion and flag clearing must share a step: two START
     * reads followed by a scheduler gap must preserve the later switch bits. */
    begin(u, &h, 3); u->restart_reads = 2; u->restart_gap = 2000; drive(u, &h);
    assert(h.state == SUNXI_MMC_READY && !u->restart_reads);
    /* The separate 10 ms starts after the mandatory 1 ms delay. */
    begin(u, &h, 3); u->done_delay = 10500; drive(u, &h); assert(h.state == SUNXI_MMC_READY);
    begin(u, &h, 3); u->done_delay = 12000; faulted(u, &h, SUNXI_MMC_SIGNAL);
    begin(u, &h, 3); u->restart_stuck = 1; faulted(u, &h, SUNXI_MMC_TIMEOUT);
    assert(u->base.now-u->clock_started >= 250000 && u->base.now-u->clock_started < 251000);
    begin(u, &h, 3); u->restart_stuck = 1;
    while (!u->clock_started && h.state != SUNXI_MMC_FAULT) {
        if (h.sd_sig == 3) h.deadline_us = h.last_us + 15000;
        u->base.fifo_this_poll = 0; sunxi_mmc_poll(&h);
    }
    assert(h.state == SUNXI_MMC_FAULT && h.result.error == SUNXI_MMC_TIMEOUT);
    assert(u->base.now-u->clock_started < 250000);
    /* Host or card declines: no CMD11, high speed at 3.3 V. */
    begin(u, &h, 2); high_speed_33(u, &h, SUNXI_MMC_UHS_HOST); assert(!u->s18r_seen);
    begin(u, &h, 0); high_speed_33(u, &h, SUNXI_MMC_UHS_HOST);
    begin(u, &h, 3); u->s18a = 0; high_speed_33(u, &h, SUNXI_MMC_UHS_DECLINED); assert(u->s18r_seen);
    /* After the card accepts CMD11 every failure is terminal: the specification
     * requires a card power cycle, available here only as board power removal. */
    begin(u, &h, 3); u->cmd11_timeout = 1; faulted(u, &h, SUNXI_MMC_CONTROLLER); assert(!u->sets && !u->bank);
    begin(u, &h, 3); u->no_dat_low = 1; faulted(u, &h, SUNXI_MMC_SIGNAL); assert(!u->sets && !u->bank);
    begin(u, &h, 3); u->bank_polls = 0; faulted(u, &h, SUNXI_MMC_SIGNAL); assert(u->sets == 1 && u->polls >= 50);
    begin(u, &h, 3); u->set_fails = 1; faulted(u, &h, SUNXI_MMC_IO); assert(u->sets == 1);
    begin(u, &h, 3); u->no_dat_high = 1; faulted(u, &h, SUNXI_MMC_SIGNAL); assert(u->bank == 2 && !u->cid_seen);
    begin(u, &h, 3); u->no_vsd = 1; faulted(u, &h, SUNXI_MMC_SIGNAL); assert(u->bank == 2 && !u->cid_seen);
    begin(u, &h, 3); u->no_cc = 1; faulted(u, &h, SUNXI_MMC_SIGNAL); assert(u->bank == 2 && !u->cid_seen);
    begin(u, &h, 3); u->bad_caps = 1; faulted(u, &h, SUNXI_MMC_IO); assert(!u->base.ocr_calls);
    /* At 1.8 V without SDR104 or a usable window: SDR25 at 50 MHz, no power cycle. */
    begin(u, &h, 3); u->sdr104 = 0; sdr25_18(u, &h, SUNXI_MMC_UHS_NO_SDR104); assert(!u->cmd19 && u->cmd6_uhs == 1);
    begin(u, &h, 3); u->sdr104_busy = 1; sdr25_18(u, &h, SUNXI_MMC_UHS_NO_SDR104); assert(!u->cmd19);
    begin(u, &h, 3); u->set_refuses = 1; sdr25_18(u, &h, SUNXI_MMC_UHS_NO_SDR104); assert(u->cmd6_uhs == 2 && !u->cmd19);
    begin(u, &h, 3); u->window[0][1] = 0; sdr25_18(u, &h, SUNXI_MMC_UHS_NO_WINDOW);
    assert(!h.card.tuning_map && h.card.tuning_width == 0 && u->cmd19 == 64 && u->cmd6_hs == 2 && u->module_calls == 2);
    begin(u, &h, 3); u->window[0][1] = 11; sdr25_18(u, &h, SUNXI_MMC_UHS_NO_WINDOW); assert(h.card.tuning_width == 11);
    /* Window choice: the longest run wins, the first on a tie, the top edge counts. */
    begin(u, &h, 3); u->window[0][0] = 2; u->window[0][1] = 12; u->window[1][0] = 30; u->window[1][1] = 15;
    drive(u, &h); assert(h.state == SUNXI_MMC_READY && h.card.tuning_start == 30 && h.card.sample_delay == 37);
    begin(u, &h, 3); u->window[0][0] = 2; u->window[0][1] = 14; u->window[1][0] = 30; u->window[1][1] = 14;
    drive(u, &h); assert(h.state == SUNXI_MMC_READY && h.card.tuning_start == 2 && h.card.sample_delay == 9);
    begin(u, &h, 3); u->window[0][0] = 50; u->window[0][1] = 14;
    drive(u, &h); assert(h.state == SUNXI_MMC_READY && h.card.tuning_start == 50 && h.card.sample_delay == 57);
    /* A FIFO overrun during tuning is not a sampling error. */
    begin(u, &h, 3); u->fatal_delay = 5; faulted(u, &h, SUNXI_MMC_CONTROLLER);
    /* The profile requires the voltage callback. */
    struct sunxi_mmc probe = {0}; struct sunxi_mmc_config c = {{uhs_read, uhs_write, barrier, now_us, admit,
        retained, uhs_module, uhs_read_fifo, model_write_fifo, NULL}, u, 0xa733, SUNXI_MMC_SD, 800000, 0x300000,
        SUNXI_MMC_SD_UHS_SDR104};
    assert(!sunxi_mmc_init(&probe, &c));
    printf("PASS UHS-I: S18R/CMD11/1.8 V switch with 1 ms and 10 ms waits, SDR104 at 150 and 100 MHz, "
           "%u CMD19 for a 24-delay window centered at 32, eight declines or fallbacks to high speed without "
           "a power cycle, terminal switch or tuning failures, bounded restart and post-delay deadline, window selection rules\n", first);
    free(u); return 0;
}
