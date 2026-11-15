#ifndef SUNSTONE_A733_G2D_IRQ_H
#define SUNSTONE_A733_G2D_IRQ_H
/* What the G2D interrupt handler does with one INTID 83 interrupt (docs/g2d-os.md#rotate-module).
 *
 * The mixer and the rotate module share the line. The vendor's g2d_handle_irq (g2d.c 802-832) takes the mixer's RCQ
 * task end first (0x24 bit 0: write one to clear, then pulse the mixer AHB reset) and then the rotator's finish
 * (ROT_INT, G2D + 0x28004, bit 0: write the read word back). The OS-image service runs one operation at a time, so
 * it dispatches by the kind the owner armed instead: only that kind's finish completes the operation. A finish of the
 * other kind is a stray. It is counted and left set, not acknowledged, so that a block the service did not start
 * cannot complete an operation. The line then stays raised, and the next interrupt finds nothing armed and masks it,
 * as an unarmed interrupt does today.
 *
 * Pure and integer only: the handler, built with -mgeneral-regs-only, calls it with the two status words it read. */
#include <stdint.h>
enum a733_g2d_irq_kind { A733_G2D_IRQ_NONE = 0, A733_G2D_IRQ_MIXER = 1, A733_G2D_IRQ_ROTATE = 2 };
struct a733_g2d_irq_action {
    uint8_t ack_mixer;   /* write 1 to 0x24, then pulse 0x08 bit 0 */
    uint8_t ack_rotate;  /* write the read ROT_INT word back to ROT_INT */
    uint8_t seen;        /* 0 nothing armed (mask the line), 1 the armed kind finished, 2 armed but not finished */
    uint8_t stray;       /* 1 when the other kind's finish was set: counted, not acknowledged */
};
static inline struct a733_g2d_irq_action a733_g2d_irq_dispatch(unsigned armed, uint32_t rcq_status, uint32_t rot_int)
{
    const unsigned mixer = rcq_status & 1u, rotate = rot_int & 1u;
    struct a733_g2d_irq_action a = {0, 0, 0, 0};
    if (armed == A733_G2D_IRQ_MIXER) {
        a.ack_mixer = (uint8_t)mixer; a.seen = mixer ? 1 : 2; a.stray = (uint8_t)rotate;
    } else if (armed == A733_G2D_IRQ_ROTATE) {
        a.ack_rotate = (uint8_t)rotate; a.seen = rotate ? 1 : 2; a.stray = (uint8_t)mixer;
    } else a.stray = (uint8_t)(mixer | rotate);
    return a;
}
#endif
