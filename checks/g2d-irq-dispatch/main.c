/* Host driver of checks/g2d-irq-dispatch.py.
 *
 * "d armed rcq_irq_ctl rcq_status rot_int" prints the dispatch: "ack_mixer ack_rotate mask_mixer mask_rotate seen stray".
 *
 * "s armed stray_at finish_at steps" runs a level-triggered model of INTID 83 for steps time steps: the armed kind's
 * block finishes at finish_at, and the other block raises a finish at stray_at. The line is raised while a block's
 * status bit and its enable are both set (0x24 bit 0 with 0x20 bit 4, ROT_INT bit 0 with bit 16) and the distributor
 * has not masked it. Each step with the line raised enters the handler once, and the handler applies the dispatch as
 * the service's handler does. It prints "entries strays completed masked". */
#include "a733_g2d_irq.h"
#include <inttypes.h>
#include <stdio.h>

static void simulate(unsigned kind, unsigned stray_at, unsigned finish_at, unsigned steps)
{
    uint32_t ctl20 = 0, rcq = 0, rot = 0;
    unsigned armed = kind, entries = 0, strays = 0, completed = 0, gic_masked = 0;
    if (kind == A733_G2D_IRQ_MIXER) ctl20 = 0x10; else if (kind == A733_G2D_IRQ_ROTATE) rot = 0x10000;
    /* The block the service did not start has its enable from its last launch. */
    if (kind != A733_G2D_IRQ_MIXER) ctl20 = 0x10;
    if (kind != A733_G2D_IRQ_ROTATE) rot |= 0x10000;
    for (unsigned t = 0; t < steps; ++t) {
        if (t == stray_at) { if (kind == A733_G2D_IRQ_ROTATE) rcq |= 5; else rot |= 1; }
        if (t == finish_at) { if (kind == A733_G2D_IRQ_MIXER) rcq |= 5; else if (kind == A733_G2D_IRQ_ROTATE) rot |= 1; }
        const int line = ((rcq & 1) && (ctl20 & 0x10)) || ((rot & 1) && (rot & 0x10000));
        if (!line || gic_masked) continue;
        ++entries;
        const struct a733_g2d_irq_action a = a733_g2d_irq_dispatch(armed, ctl20, rcq, rot);
        strays += a.stray;
        if (a.mask_mixer) ctl20 &= ~UINT32_C(0x10);
        if (a.mask_rotate) rot = (rot & ~UINT32_C(0x10001)) | (rot & 1); /* writing 0 to bit 0 leaves it */
        if (a.ack_mixer) rcq &= ~UINT32_C(1);
        if (a.ack_rotate) rot &= ~UINT32_C(1);
        if (a.seen == 1) { ++completed; armed = A733_G2D_IRQ_NONE; }
        else if (a.seen == 0 || a.seen == 2) { gic_masked = 1; armed = A733_G2D_IRQ_NONE; }
    }
    printf("%u %u %u %u\n", entries, strays, completed, gic_masked);
}

int main(void)
{
    char mode;
    while (scanf(" %c", &mode) == 1) {
        if (mode == 'd') {
            unsigned armed;
            uint32_t ctl, rcq, rot;
            if (scanf("%u %" SCNx32 " %" SCNx32 " %" SCNx32, &armed, &ctl, &rcq, &rot) != 4) return 1;
            const struct a733_g2d_irq_action a = a733_g2d_irq_dispatch(armed, ctl, rcq, rot);
            printf("%u %u %u %u %u %u\n", a.ack_mixer, a.ack_rotate, a.mask_mixer, a.mask_rotate, a.seen, a.stray);
        } else {
            unsigned kind, stray_at, finish_at, steps;
            if (scanf("%u %u %u %u", &kind, &stray_at, &finish_at, &steps) != 4) return 1;
            simulate(kind, stray_at, finish_at, steps);
        }
    }
    return 0;
}
