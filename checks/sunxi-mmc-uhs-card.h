/* Modeled UHS-I card and controller additions on top of the accepted card
 * model (struct model, read32 and write32 from checks/sunxi-mmc.c): S18A,
 * CMD11 with DAT low and switch-done, CMD6 function group 1 and CMD19 tuning
 * blocks that pass only inside configured sample-delay windows. */
#ifndef SUNSTONE_CHECK_MMC_UHS_CARD_H
#define SUNSTONE_CHECK_MMC_UHS_CARD_H
#include <stdlib.h>
#include "sunxi_smhc.h"
#define VSW (1u << 28)
#define VSD (1u << 10)
#define S18R (1u << 24)
static const uint8_t pattern[64] = {
    0xff, 0x0f, 0xff, 0x00, 0xff, 0xcc, 0xc3, 0xcc, 0xc3, 0x3c, 0xcc, 0xff, 0xfe, 0xff, 0xfe, 0xef,
    0xff, 0xdf, 0xff, 0xdd, 0xff, 0xfb, 0xff, 0xfb, 0xbf, 0xff, 0x7f, 0xff, 0x77, 0xf7, 0xbd, 0xef,
    0xff, 0xf0, 0xff, 0xf0, 0x0f, 0xfc, 0xcc, 0x3c, 0xcc, 0x33, 0xcc, 0xcf, 0xff, 0xef, 0xff, 0xee,
    0xff, 0xfd, 0xff, 0xfd, 0xdf, 0xff, 0xbf, 0xff, 0xbb, 0xff, 0xf7, 0xff, 0xf7, 0x7f, 0x7b, 0xde };
/* Card switch phases: 0 3.3 V, 1 CMD11 answered (DAT low), 2 clock stopped,
 * 3 clock restarted at 1.8 V (DAT high), 4 failed restart (DAT stays low). */
struct uhs {
    struct model base;
    uint8_t scr[8], sw[64], data[64];
    unsigned caps, s18a, phase, bank, bank_polls, queries, sets, polls, module_calls, divisor;
    unsigned width, sdr, sdr104, sdr104_busy, set_refuses, cmd11_timeout, no_dat_low, no_dat_high;
    unsigned no_vsd, set_fails, bad_caps, fatal_delay, cmd19, cmd11, cmd6_uhs, cmd6_hs;
    unsigned window[2][2], s18r_seen, cmd11_before_cmd2, cid_seen, dat0_masked;
    uint64_t answered, gated, restarted;
};
static unsigned passes(struct uhs *u, unsigned d)
{
    for (unsigned i = 0; i < 2; ++i)
        if (u->window[i][1] && d >= u->window[i][0] && d < u->window[i][0] + u->window[i][1]) return 1;
    return 0;
}
static int uhs_read(void *v, uint32_t off, uint32_t *out)
{
    struct uhs *u = v; struct model *m = &u->base;
    if (off == 0x200 && m->data_command && ((m->data_command & 63) == 51 || (m->data_command & 63) == 6 ||
                                            (m->data_command & 63) == 19)) {
        assert(++m->fifo_this_poll <= SUNXI_MMC_FIFO_WORDS && m->fifo_pos < m->fifo_total);
        unsigned c = m->data_command & 63; const uint8_t *src = c == 51 ? u->scr : c == 6 ? u->sw : u->data;
        *out = 0;
        for (unsigned b = 0; b < 4; ++b) *out |= (uint32_t)src[m->fifo_pos++] << (8 * b);
        return 1;
    }
    int r = read32(m, off, out);
    if (off == 0x3c) {
        unsigned low = u->phase == 1 || u->phase == 2 || u->phase == 4;
        if (!low || (u->phase == 1 && u->no_dat_low)) *out |= SUNXI_SMHC_STATUS_DAT3;
        if (low && !(u->phase == 1 && u->no_dat_low)) *out |= 1u << 9; /* DAT0 held low. */
    }
    return r;
}
static inline int uhs_read_fifo(void *v, uint32_t *w, unsigned n)
{for (unsigned i = 0; i < n; ++i) if (uhs_read(v, 0x200, &w[i]) != 1) return 0; return 1;}
static void reply(struct model *m, unsigned cmd, uint32_t value, uint32_t arg)
{
    if (cmd != 19) { assert(m->command_count < 64); m->commands[m->command_count] = cmd; m->args[m->command_count++] = arg; }
    m->reg[0x18/4] = value & ~(1u << 31); m->reg[0x38/4] = 4; m->reg[0x20/4] = 0x900;
    m->data_command = 0; m->fifo_total = m->fifo_pos = 0;
}
static int uhs_write(void *v, uint32_t off, uint32_t value)
{
    struct uhs *u = v; struct model *m = &u->base;
    if (off == 4 && (value & (1u << 31))) u->dat0_masked = 1;
    if (off != 0x18) return write32(m, off, value);
    unsigned cmd = value & 63, arg = m->reg[0x1c/4];
    if (value & (1u << 21)) {
        if (value & VSW) {
            assert(u->phase == 1 || u->phase == 2);
            assert(m->reg[4/4] & (1u << 31)); /* DAT0 is masked while the card holds it low. */
            if (u->phase == 1) {
                assert(m->reg[4/4] == (1u << 31) && m->now - u->answered >= 1000); /* Linux waits 1 ms. */
                u->phase = 2; u->gated = m->now;
            }
            else {
                assert(u->bank == 2 && m->reg[4/4] == ((1u << 16) | (1u << 31)));
                assert(m->now - u->restarted >= 10000); /* Clock gated 10 ms after the bank switch. */
                if (u->no_dat_high) u->phase = 4;
                else { u->phase = 3; if (!u->no_vsd) m->reg[0x38/4] |= VSD | 4; }
            }
            m->reg[0x18/4] = 0; return 1;
        }
        assert(u->phase == 0 || u->phase == 3);
        return write32(m, off, value);
    }
    assert(!(value & VSW) || cmd == 11);
    if (cmd == 41) {
        assert(m->app && arg == (0x40300000u | (u->caps & 1 ? S18R : 0)));
        if (arg & S18R) u->s18r_seen = 1;
        reply(m, 41, value, arg); m->app = 0; ++m->ocr_calls;
        m->reg[0x20/4] = 0x00300000u | (m->sector ? 1u << 30 : 0);
        if (m->ocr_calls == 2) {
            m->reg[0x20/4] |= 1u << 31; m->state = 1;
            if ((arg & S18R) && u->s18a) m->reg[0x20/4] |= S18R;
        }
        return 1;
    }
    if (cmd == 11) {
        assert(value & VSW && !arg && m->state == 1 && !u->phase && !u->cid_seen && u->s18a);
        reply(m, 11, value, arg); ++u->cmd11; m->reg[0x20/4] = 0x200;
        if (u->cmd11_timeout) { m->reg[0x38/4] = 1u << 8; return 1; }
        u->phase = 1; u->answered = m->now; return 1;
    }
    if (cmd == 2) { u->cid_seen = 1; if (u->cmd11) u->cmd11_before_cmd2 = 1; assert(u->phase == 0 || u->phase == 3); }
    if (m->selected && (cmd == 55 || cmd == 51 || cmd == 6 || cmd == 19)) {
        reply(m, cmd, value, arg);
        if (cmd == 55) { assert(arg == 0x12340000); m->app = 1; m->reg[0x20/4] |= 1u << 5; }
        else if (cmd == 51) {
            assert(m->app && !arg && m->reg[0x10/4] == 8); m->app = 0; m->data_command = value; m->fifo_total = 8;
        } else if (cmd == 6 && !(value & (1u << 9))) { assert(m->app && arg == 2); m->app = 0; u->width = 4; }
        else if (cmd == 6) {
            assert(!m->app && u->width == 4 && m->reg[0xc/4] == 1 && m->reg[0x10/4] == 64);
            m->data_command = value; m->fifo_total = 64; memset(u->sw, 0, 64);
            u->sw[13] = 3 | (u->sdr104 ? 8 : 0); u->sw[29] = u->sdr104_busy ? 8 : 0;
            if ((arg & 0xf) == 3) {
                assert(u->phase == 3 && (arg == 0x00fffff3 || arg == 0x80fffff3)); ++u->cmd6_uhs;
                u->sw[16] = u->sdr104 && !u->sdr104_busy ? 3 : 0xf;
                if (arg >> 31) { if (u->set_refuses) u->sw[16] = 0xf; else if (u->sw[16] == 3) u->sdr = 3; }
            } else {
                assert(arg == 0x00fffff1 || arg == 0x80fffff1); ++u->cmd6_hs; u->sw[16] = 1;
                if (arg >> 31) u->sdr = 1;
            }
        } else {
            unsigned d = m->reg[0x144/4] & 63;
            assert(u->sdr == 3 && u->phase == 3 && !arg && m->reg[0x10/4] == 64 && (m->reg[0x144/4] & 0x80));
            assert(!(m->reg[0x5c/4] & (1u << 31)) && (m->reg[0x100/4] & 0x1fff0001u) == ((512u << 16) | 1u));
            assert(m->reg[8/4] == 0x040000ffu);
            ++u->cmd19; m->data_command = value; m->fifo_total = 64; memcpy(u->data, pattern, 64);
            if (d == u->fatal_delay) m->reg[0x38/4] |= 1u << 11;
            else if (!passes(u, d)) {
                if (d % 3 == 0) m->reg[0x38/4] |= 1u << 7;      /* Data CRC error. */
                else if (d % 3 == 1) u->data[5] ^= 0x10;         /* Wrong pattern, clean CRC. */
                else { m->reg[0x38/4] = 1u << 9; m->data_command = 0; m->fifo_total = 0; } /* No data. */
            }
        }
        return 1;
    }
    return write32(m, off, value);
}
#endif
