/* UHS-I SDR104 state machine of the MMC core against a modeled UHS-I card and
 * controller. Reuses the accepted card model; its own cases never execute. */
#define main accepted_mmc_model_main
#include "sunxi-mmc.c"
#undef main
#include "sunxi-mmc-uhs-card.h"
#include "vendor-phases.h"   /* rows: 0 400 kHz, 1 26 MHz, 2 52 MHz, 3 104 MHz, 4 208 MHz */
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
static unsigned want_sdr25, want_reenter;
static int want_ceiling = -1;   /* sunxi_mmc_set_ceiling before open, when not negative */
static void begin(struct uhs *u, struct sunxi_mmc *h, unsigned caps)
{
    memset(u, 0, sizeof(*u)); seed(&u->base, h, SUNXI_MMC_SD, 1);
    field(u->base.csd, 84, 12, 5 | (1u << 10)); field(u->base.csd, 96, 8, 0x32); u->scr[0] = 2; u->scr[1] = 5;
    u->caps = caps; u->s18a = 1; u->sdr104 = 1; u->bank_polls = 3; u->fatal_delay = 99; u->done_delay = 1000;
    u->window[0][0] = 20; u->window[0][1] = 24;
    struct sunxi_mmc_config c = {{uhs_read, uhs_write, barrier, now_us, admit, retained, uhs_module,
        uhs_read_fifo, model_write_fifo, uhs_signal, NULL}, u, 0xa733, SUNXI_MMC_SD, 800000, 0x300000,
        SUNXI_MMC_SD_UHS_SDR104, NULL};
    assert(sunxi_mmc_init(h, &c));
    if (want_sdr25) assert(sunxi_mmc_sdr25_only(h));
    if (want_ceiling >= 0) assert(sunxi_mmc_set_ceiling(h, (unsigned)want_ceiling));
    if (want_reenter) { assert(sunxi_mmc_reenter(h)); u->phase = 3; u->bank = 2; }
    assert(sunxi_mmc_open(h, 100000000));
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
/* Like drive, with one injected fault armed before attempt inj_nth of delay inj_delay (see struct uhs). */
static void fuzz_drive(struct uhs *u, struct sunxi_mmc *h)
{
    for (unsigned i = 0; i < 400000; ++i) {
        if (h->state == SUNXI_MMC_READY || h->state == SUNXI_MMC_DONE || h->state == SUNXI_MMC_FAULT) return;
        if (u->sweep_ready && !u->inj_fired && h->tuning && h->tune_delay == u->inj_delay && h->tune_repeat == u->inj_nth) { u->inj_fired = 1; u->inj_armed = 1; }
        u->base.fifo_this_poll = 0; sunxi_mmc_poll(h);
    }
    /* Not terminal: the caller counts it as a failure (a sweep that hangs). */
}
static unsigned fuzz_runs, fuzz_failures, fuzz_by_mode[6];
/* One injected fault: the sweep must end READY (a window, or the no-window fallback) and never FAULT; removal bits must fault. */
static void fuzz_one(struct uhs *u, struct sunxi_mmc *h, unsigned mode, unsigned bits, unsigned delay, unsigned nth, unsigned must_fault)
{
    want_reenter = 1; begin(u, h, 3); want_reenter = 0; u->s18a = 0;
    if (getenv("FUZZ_TRACE")) fprintf(stderr, "mode %u bits %08x delay %u nth %u\n", mode, bits, delay, nth);
    u->inj_on = 1; u->inj_mode = mode; u->inj_bits = bits; u->inj_delay = delay; u->inj_nth = nth;
    fuzz_drive(u, h); ++fuzz_runs;
    int ok = must_fault ? (h->state == SUNXI_MMC_FAULT && u->inj_fired)
                        : (u->inj_fired && h->state == SUNXI_MMC_READY &&
                           (h->card.uhs_outcome == SUNXI_MMC_UHS_SDR104 || h->card.uhs_outcome == SUNXI_MMC_UHS_NO_WINDOW));
    if (!ok) {
        ++fuzz_by_mode[mode];
        if (fuzz_failures++ < 6)
            printf("FUZZ FAIL mode %u bits 0x%08x delay %u repeat %u: state %d error %d outcome %d fired %u\n", mode, bits, delay, nth,
                   (int)h->state, (int)h->result.error, (int)h->card.uhs_outcome, u->inj_fired);
    }
}
static void fuzz(struct uhs *u, struct sunxi_mmc *h)
{
    /* Every interrupt bit (mode 0), every R1 status bit (1), every control (4) and status (5) register bit, at every delay; the same at the
     * last repeat of four delays. Removal bits (18, 31) must fault; no other bit may. */
    for (unsigned mode = 0; mode <= 5; ++mode) {
        if (mode == 2 || mode == 3) continue;
        for (unsigned bit = 0; bit < 32; ++bit) {
            /* The control register's reset bits (0 to 2) report a reset in progress: flipping them lies about progress, which no software can detect. */
            if (mode == 4 && bit < 3) continue;
            unsigned must = mode == 0 && (bit == 18 || bit == 31);
            for (unsigned d = 0; d < 64; ++d) fuzz_one(u, h, mode, 1u << bit, d, 0, must);
            for (unsigned k = 0; k < 4; ++k) fuzz_one(u, h, mode, 1u << bit, 20 + k * 8 - (k == 3 ? 1 : 0), 19, must);
        }
    }
    /* Every bit at once. */
    for (unsigned d = 0; d < 64; ++d) { fuzz_one(u, h, 0, 0x7ffbffffu, d, 0, 0); fuzz_one(u, h, 1, 0xffffffffu, d, 0, 0); fuzz_one(u, h, 5, 0xffffffffu, d, 0, 0); }
    /* A command that never completes, with START clear (2) and set (3), at every delay and at the last repeat. */
    for (unsigned mode = 2; mode <= 3; ++mode)
        for (unsigned d = 0; d < 64; ++d) { fuzz_one(u, h, mode, 0, d, 0, 0); if (d >= 20 && d < 44) fuzz_one(u, h, mode, 0, d, 19, 0); }
    printf("fuzz: %u runs, %u failures (interrupt bits %u, R1 bits %u, hang %u, stuck %u, control register %u, status register %u)\n", fuzz_runs, fuzz_failures,
           fuzz_by_mode[0], fuzz_by_mode[1], fuzz_by_mode[2], fuzz_by_mode[3], fuzz_by_mode[4], fuzz_by_mode[5]);
    fflush(stdout); assert(!fuzz_failures);
}
static void read_back(struct uhs *u, struct sunxi_mmc *h)
{
    uint8_t data[4096]; struct sunxi_mmc_result result;
    assert(sunxi_mmc_read(h, 8, 8, data, sizeof(data), h->last_us + 10000000)); drive(u, h);
    assert(h->state == SUNXI_MMC_DONE && sunxi_mmc_take(h, &result) && result.received == 4096);
    assert(u->base.reg[0x10/4] == 512 && u->base.reg[8/4] == UINT32_MAX);
    /* SDR104 reads start a block only with 512 bytes free; other timings
     * leave the read threshold disabled. */
    assert((u->base.reg[0x100/4] & 0x1fff0001u) == (h->card.timing >= 2 ? (512u << 16) | 1u : 0u));
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
/* The mode ladder: what identification selects for each kind of card, and how it steps down. */
static void expect_mode(struct uhs *u, struct sunxi_mmc *h, unsigned mode, unsigned divisor, unsigned signal, enum sunxi_mmc_uhs_outcome outcome,
                        unsigned steps, unsigned first_from, unsigned first_to)
{
    drive(u, h);
    assert(h->state == SUNXI_MMC_READY && h->card.timing == mode && h->card.signal_1v8 == signal && h->card.uhs_outcome == outcome);
    assert((u->divisor & 0xff) == divisor && h->card.mode_steps == steps && u->sdr == mode);
    /* The drive and sample phases are the board kernel's for the clock range: default speed and SDR12 the 26 MHz row, high speed and SDR25 the 52 MHz row,
     * SDR50 the 104 MHz row, SDR104 the 208 MHz row (the board's sdc0 node sets no override). The sample phases are in the new timing register; a tuned mode
     * uses the old sample mode with the software delay. */
    {
        unsigned row = mode == 3 ? 4 : mode == 2 ? 3 : mode == 1 ? 2 : 1;
        uint32_t drive = u->base.reg[0x140 / 4], ntsr = u->base.reg[0x5c / 4];
        assert(((drive >> 16) & 1) == vendor_phase[row].cmd && ((drive >> 17) & 1) == vendor_phase[row].dat);
        if (mode < 2) assert(((ntsr >> 8) & 3) == vendor_phase[row].sam_dat && ((ntsr >> 4) & 3) == vendor_phase[row].sam_cmd);
        else assert(!(ntsr & 0x331u) && vendor_phase[row].sam_dat == 0 && vendor_phase[row].sam_cmd == 0);
    }
    if (steps) assert(h->card.mode_log[0].from == first_from && h->card.mode_log[0].to == first_to && h->card.mode_log[0].reason == SUNXI_MMC_MODE_NO_WINDOW);
    read_back(u, h);
}
static void ladder(struct uhs *u, struct sunxi_mmc *h)
{
    for (unsigned reenter = 0; reenter <= 1; ++reenter) {
        want_reenter = reenter;
        /* SDR104 with a window: 150 MHz (module 300 MHz, divisor 1 from the 300M parent). */
        begin(u, h, 3); u->s18a = !reenter; expect_mode(u, h, 3, 1, 1, SUNXI_MMC_UHS_SDR104, 0, 0, 0);
        /* S18A but no SDR104 in the card: SDR50 at 100 MHz when it has it, tuned, else SDR25. */
        begin(u, h, 3); u->s18a = !reenter; u->sdr104 = 0; u->sdr50 = 1; expect_mode(u, h, 2, 2, 1, SUNXI_MMC_UHS_SDR50, 0, 0, 0);
        assert(h->card.module_hz == 200000000 && h->card.card_hz == 100000000 && h->card.tuning_width >= 12);
        begin(u, h, 3); u->s18a = !reenter; u->sdr104 = 0; expect_mode(u, h, 1, 4, 1, SUNXI_MMC_UHS_NO_SDR104, 0, 0, 0);
        /* SDR104 with no window: SDR50 when the card has it and its sweep finds one; the step is recorded. */
        begin(u, h, 3); u->s18a = !reenter; u->sdr50 = 1; u->window[0][1] = 0; u->win50_start = 30; u->win50_width = 20;
        expect_mode(u, h, 2, 2, 1, SUNXI_MMC_UHS_SDR50, 1, 3, 2);
        assert(h->card.tuning_start == 30 && h->card.tuning_width == 20 && h->card.sample_delay == 40);
        /* Neither mode has a window: SDR25, two steps recorded. */
        begin(u, h, 3); u->s18a = !reenter; u->sdr50 = 1; u->window[0][1] = 0; u->win50_start = 30; u->win50_width = 5;
        expect_mode(u, h, 1, 4, 1, SUNXI_MMC_UHS_NO_WINDOW, 2, 3, 2);
        assert(h->card.mode_log[1].from == 2 && h->card.mode_log[1].to == 1 && h->card.mode_log[1].reason == SUNXI_MMC_MODE_NO_WINDOW);
        /* SDR104 with no window in a card without SDR50: straight to SDR25. */
        begin(u, h, 3); u->s18a = !reenter; u->window[0][1] = 0; expect_mode(u, h, 1, 4, 1, SUNXI_MMC_UHS_NO_WINDOW, 1, 3, 1);
        /* A SDR50 request the card refuses: SDR25. */
        begin(u, h, 3); u->s18a = !reenter; u->sdr104 = 0; u->sdr50 = 1; u->set_refuses50 = 1; expect_mode(u, h, 1, 4, 1, SUNXI_MMC_UHS_NO_SDR104, 0, 0, 0);
        /* Ceilings: SDR50 on a card that has SDR104, SDR25, and SDR12 at 25 MHz (module 50 MHz, divisor 8). */
        want_ceiling = 2; begin(u, h, 3); u->s18a = !reenter; u->sdr50 = 1; expect_mode(u, h, 2, 2, 1, SUNXI_MMC_UHS_SDR50, 0, 0, 0);
        want_ceiling = 1; begin(u, h, 3); u->s18a = !reenter; u->sdr50 = 1; expect_mode(u, h, 1, 4, 1, SUNXI_MMC_UHS_LIMITED, 0, 0, 0);
        want_ceiling = 0; begin(u, h, 3); u->s18a = !reenter; u->sdr50 = 1; expect_mode(u, h, 0, 8, 1, SUNXI_MMC_UHS_LIMITED, 0, 0, 0);
        assert(h->card.card_hz == 25000000 && h->card.ceiling == 0);
        want_ceiling = -1;
    }
    want_reenter = 0;
    /* The owner's rule for a card found at 1.8 V by a 3.3 V host: the card at 1.8 V (phase 3) answers S18A clear and reports SDR50 and SDR104; a 3.3 V card, a card
     * that declines UHS and a card that took the switch are not it. */
    begin(u, h, 3); u->s18a = 0; u->phase = 3; u->bank = 0; u->sdr50 = 1; want_reenter = 0; drive(u, h);
    assert(h->state == SUNXI_MMC_READY && !h->card.signal_1v8 && h->card.timing == 1 && sunxi_mmc_card_found_at_1v8(&h->card));
    begin(u, h, 3); drive(u, h); assert(h->card.signal_1v8 && !sunxi_mmc_card_found_at_1v8(&h->card));                                   /* the switch was taken */
    begin(u, h, 3); u->s18a = 0; u->nohs = 0; drive(u, h); assert(h->state == SUNXI_MMC_READY && !sunxi_mmc_card_found_at_1v8(&h->card)); /* 3.3 V card, S18A clear, no UHS functions at 3.3 V */
    /* A card that only signals 3.3 V (no S18A): high speed at 50 MHz, no switch, whatever it supports. */
    begin(u, h, 3); u->s18a = 0; u->sdr50 = 1; expect_mode(u, h, 1, 4, 0, SUNXI_MMC_UHS_DECLINED, 0, 0, 0);
    assert(!u->cmd11 && !h->card.signal_1v8 && h->card.card_hz == 50000000);
    /* A card without high speed (SCR version 1.0) at 3.3 V: 25 MHz default speed. */
    begin(u, h, 3); u->s18a = 0; u->nohs = 1; u->scr[0] = 0; expect_mode(u, h, 0, 8, 0, SUNXI_MMC_UHS_NO_SDR104, 0, 0, 0);
    assert(h->card.card_hz == 25000000);
    /* A ceiling of default speed at 3.3 V. */
    want_ceiling = 0; begin(u, h, 3); u->s18a = 0; expect_mode(u, h, 0, 8, 0, SUNXI_MMC_UHS_LIMITED, 0, 0, 0); want_ceiling = -1;
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
    assert(((u->base.reg[0x140/4] >> 16) & 3) == 1 && (u->base.reg[0x100/4] & 0x1fff0001u) == ((64u << 16) | 1u));
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
    /* SDR25 only at 1.8 V: the switch runs, SDR104 and tuning are never tried, and the CMD6 response of the switch is kept. */
    want_sdr25 = 1; want_reenter = 0;
    begin(u, &h, 3); sdr25_18(u, &h, SUNXI_MMC_UHS_LIMITED);
    assert(u->cmd11 == 1 && u->sets == 1 && !u->cmd6_uhs && !u->cmd19 && !h.card.reentered && (h.card.switch_status[13] & 3) == 3);
    assert((h.card.switch_status[13] & 8) && (h.card.switch_status[16] & 15) == 1);   /* SDR104 support is visible at 1.8 V; function 1 selected */
    /* Re-entry at 1.8 V: the bank is already there, the card signals 1.8 V, no CMD11 and no selector write. */
    for (unsigned s18a = 0; s18a < 2; ++s18a) {
        want_sdr25 = 1; want_reenter = 1;
        begin(u, &h, 3); u->s18a = s18a; sdr25_18(u, &h, SUNXI_MMC_UHS_LIMITED);
        assert(!u->cmd11 && !u->sets && !u->cmd19 && h.card.reentered && h.card.signal_1v8 && u->s18r_seen);
        assert((h.card.switch_status[13] & 8) && !u->cmd6_uhs);
    }
    want_sdr25 = 0; want_reenter = 1;                  /* a re-entry that goes on to SDR104 and tuning */
    begin(u, &h, 3); drive(u, &h);
    assert(h.state == SUNXI_MMC_READY && h.card.reentered && !u->cmd11 && !u->sets && h.card.timing == 3 &&
           h.card.uhs_outcome == SUNXI_MMC_UHS_SDR104 && h.card.sample_delay == 32);
    read_back(u, &h);
    want_sdr25 = want_reenter = 0;
    /* A card whose every failing delay leaves the command stuck and DAT0 low: the immediate reset and the masked clock update clear each one, the sweep
     * completes with the same window. */
    want_reenter = 1; begin(u, &h, 3); want_reenter = 0; u->stuck_always = 1; drive(u, &h);
    assert(h.state == SUNXI_MMC_READY && h.card.uhs_outcome == SUNXI_MMC_UHS_SDR104 && h.card.sample_delay == 32);
    /* A controller that a reset does not free: the recovery repeats the reset and gives up after 12 consecutive recoveries that did not complete. */
    want_reenter = 1; begin(u, &h, 3); want_reenter = 0; u->stuck_always = 1; u->unfixable = 1; faulted(u, &h, SUNXI_MMC_TIMEOUT);
    /* A card that never answers at any failing delay: each attempt fails its delay after the attempt timer, and 12 in a row stop the sweep. */
    want_reenter = 1; begin(u, &h, 3); want_reenter = 0; u->hang_always = 1; faulted(u, &h, SUNXI_MMC_TIMEOUT);
    /* A card that fails the sweep the way the board's does: every kind of sampling error, including the FIFO run error. */
    want_reenter = 1; begin(u, &h, 3); want_reenter = 0; u->tune_kinds = 8; u->s18a = 0; drive(u, &h);
    assert(h.state == SUNXI_MMC_READY && h.card.uhs_outcome == SUNXI_MMC_UHS_SDR104 && h.card.sample_delay == 32);
    /* Both options are for the UHS-I profile, before open. */
    {
        struct sunxi_mmc g; struct uhs v;
        memset(&v, 0, sizeof v); seed(&v.base, &g, SUNXI_MMC_SD, 1);
        struct sunxi_mmc_config c = {{uhs_read, uhs_write, barrier, now_us, admit, retained, uhs_module,
            uhs_read_fifo, model_write_fifo, NULL, NULL}, &v, 0xa733, SUNXI_MMC_SD, 800000, 0x300000, SUNXI_MMC_SD_HIGH_SPEED4, NULL};
        assert(sunxi_mmc_init(&g, &c) && !sunxi_mmc_reenter(&g) && !sunxi_mmc_sdr25_only(&g));
    }
    begin(u, &h, 3); assert(!sunxi_mmc_reenter(&h) && !sunxi_mmc_sdr25_only(&h));       /* already open */
    /* Capability observation: ACMD41 carries S18R and the card's S18A is kept, but there is no CMD11 and the card stays at 3.3 V. */
    for (unsigned s18a = 0; s18a < 2; ++s18a) {
        struct sunxi_mmc_config c = {{uhs_read, uhs_write, barrier, now_us, admit, retained, uhs_module,
            uhs_read_fifo, model_write_fifo, NULL, NULL}, u, 0xa733, SUNXI_MMC_SD, 800000, 0x300000,
            SUNXI_MMC_SD_HIGH_SPEED4, NULL};
        memset(u, 0, sizeof(*u)); seed(&u->base, &h, SUNXI_MMC_SD, 1);
        field(u->base.csd, 84, 12, 5 | (1u << 10)); field(u->base.csd, 96, 8, 0x32); u->scr[0] = 2; u->scr[1] = 5;
        u->caps = 1; u->s18a = s18a; u->sdr104 = 1; u->fatal_delay = 99; u->done_delay = 1000;   /* the model expects S18R in the argument */
        assert(sunxi_mmc_init(&h, &c) && sunxi_mmc_observe_s18(&h) && sunxi_mmc_open(&h, 100000000));
        assert(!sunxi_mmc_observe_s18(&h));
        high_speed_33(u, &h, SUNXI_MMC_UHS_NONE);
        assert(u->s18r_seen && !u->queries && (!!(h.card.ocr & (1u << 24))) == (int)s18a);
    }
    /* UHS-I and the observation do not combine, and the observation is for SD only. */
    {
        struct sunxi_mmc_config c = {{uhs_read, uhs_write, barrier, now_us, admit, retained, uhs_module,
            uhs_read_fifo, model_write_fifo, uhs_signal, NULL}, u, 0xa733, SUNXI_MMC_SD, 800000, 0x300000,
            SUNXI_MMC_SD_UHS_SDR104, NULL};
        memset(&h, 0, sizeof(h)); assert(sunxi_mmc_init(&h, &c) && !sunxi_mmc_observe_s18(&h));
    }
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
    ladder(u, &h);
    fuzz(u, &h);
    /* The profile requires the voltage callback. */
    struct sunxi_mmc probe = {0}; struct sunxi_mmc_config c = {{uhs_read, uhs_write, barrier, now_us, admit,
        retained, uhs_module, uhs_read_fifo, model_write_fifo, NULL, NULL}, u, 0xa733, SUNXI_MMC_SD, 800000, 0x300000,
        SUNXI_MMC_SD_UHS_SDR104, NULL};
    assert(!sunxi_mmc_init(&probe, &c));
    printf("PASS UHS-I: S18R/CMD11/1.8 V switch with 1 ms and 10 ms waits, SDR104 at 150 and 100 MHz, "
           "%u CMD19 for a 24-delay window centered at 32, eight declines or fallbacks to high speed without "
           "a power cycle, terminal switch or tuning failures, bounded restart and post-delay deadline, window selection rules\n", first);
    free(u); return 0;
}
