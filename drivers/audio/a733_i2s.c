#include "a733_i2s.h"

/* Register facts: pinned A733 audio sources and board route in docs/i2s.md.
 * The stream driver programs the registers from this plan. */

static unsigned divider(uint32_t input, uint32_t output)
{
    static const unsigned divisors[] = {1,2,4,6,8,12,16,24,32,48,64,96,128,176,192};
    if (!output || input % output) return 0;
    for (unsigned i = 0; i < sizeof(divisors) / sizeof(*divisors); ++i)
        if (input / output == divisors[i]) return i + 1;
    return 0;
}

int a733_i2s_plan(const struct a733_i2s_format *f, struct a733_i2s_plan *out)
{
    if (!f || !out || f->rate_hz < 8000 || f->rate_hz > 192000 ||
        (f->container_bits != 16 && f->container_bits != 32) ||
        (f->slot_bits != 16 && f->slot_bits != 32) || f->slot_bits < f->container_bits ||
        (f->channels != 1 && f->channels != 2) || f->first_slot > 1 ||
        (f->channels == 2 && f->first_slot)) return 0;
    unsigned bclk = divider(f->module_hz, f->rate_hz * 2u * f->slot_bits);
    unsigned mclk = f->mclk_hz ? divider(f->module_hz, f->mclk_hz) : 0;
    if (!bclk || (f->mclk_hz && !mclk)) return 0;
    struct a733_i2s_plan p = {0};
    p.format = *f;
    p.ctl = (1u << 4) | (1u << 17) | (1u << 18);
    p.fmt0 = ((f->slot_bits - 1u) << 8) |
             ((f->container_bits / 4u - 1u) << 4) | (f->slot_bits / 4u - 1u);
    p.clkdiv = bclk << 4;
    if (mclk) p.clkdiv |= (1u << 8) | mclk;
    p.chcfg = (f->channels - 1u) << 4;
    p.rxchsel = (1u << 20) | ((f->channels - 1u) << 16);
    p.rxmap3 = f->channels == 2 ? 0x100u : f->first_slot;
    p.pb_mask = (1u << 5) | (1u << 6) | (1u << 8) | (mclk ? 1u << 4 : 0);
    p.dma_width = f->container_bits / 8;
    p.frame_bytes = p.dma_width * f->channels;
    *out = p;
    return 1;
}
