#include "gicv3_spi.h"

static int valid(const struct gicv3_spi *s)
{
    return s && s->distributor && !(s->distributor & 0xffff) &&
        s->distributor <= UINTPTR_MAX - 0xffff && s->intid >= 32 && s->intid < 1020;
}
static volatile uint32_t *reg(const struct gicv3_spi *s, uint32_t offset)
{ return (volatile uint32_t *)(s->distributor + offset); }
int gicv3_spi_mask(const struct gicv3_spi *s)
{
    if (!valid(s)) return 0;
    *reg(s, 0x180 + 4 * (s->intid / 32)) = UINT32_C(1) << (s->intid % 32);
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
int gicv3_spi_rearm(const struct gicv3_spi *s)
{
    if (!valid(s)) return -1;
    if (*reg(s, 0) & (UINT32_C(1) << 31)) return 0;
    uint32_t bit = UINT32_C(1) << (s->intid % 32);
    volatile uint32_t *enable = reg(s, 0x100 + 4 * (s->intid / 32));
    __asm__ volatile("dsb sy" ::: "memory");
    *enable = bit;
    __asm__ volatile("dsb sy" ::: "memory");
    if (*enable & bit) return 1;
    /* A failed readback does not prove the enable write had no effect. Issue
     * disable before reporting failure; independent recovery still covers
     * inaccessible or stalled hardware and uncompleted propagation. */
    (void)gicv3_spi_mask(s);
    return -1;
}
