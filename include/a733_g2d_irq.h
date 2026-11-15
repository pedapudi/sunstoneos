#ifndef SUNSTONE_A733_G2D_IRQ_H
#define SUNSTONE_A733_G2D_IRQ_H
/* What the G2D interrupt handler does with one INTID 83 interrupt (docs/g2d-os.md#rotate-module).
 *
 * The mixer and the rotate module share the line. The vendor's g2d_handle_irq (g2d.c 802-832) takes the mixer's RCQ
 * task end first (0x24 bit 0: write one to clear, then pulse the mixer AHB reset) and then the rotator's finish
 * (ROT_INT, G2D + 0x28004, bit 0: write the read word back). The OS-image service runs one operation at a time, so
 * it dispatches by the kind the owner armed instead: only that kind's finish completes the operation. A finish of the
 * other kind is a stray: a block the service did not start cannot complete an operation.
 *
 * The line is level-triggered, so a stray left raising it would re-enter the handler until the armed operation ends,
 * an interrupt storm on core 0. The handler therefore masks the stray's source at the block: it clears the mixer's
 * task-end enable (0x20 bit 4) or the rotator's finish enable (ROT_INT bit 16, written with bit 0 as 0, which leaves
 * the write-one-to-clear status set for the close to read). It counts the stray and records its word. The next launch
 * of that kind writes its enable again (0x20 = 0x10, ROT_INT = 0x10000).
 *
 * A stray alone leaves the operation armed and does not wake the owner. With nothing armed, every finish is a stray and
 * the line is masked at the distributor, as before.
 *
 * Pure and integer only: the handler, built with -mgeneral-regs-only, calls it with the three words it read. */
#include <stdint.h>
enum a733_g2d_irq_kind { A733_G2D_IRQ_NONE = 0, A733_G2D_IRQ_MIXER = 1, A733_G2D_IRQ_ROTATE = 2 };
struct a733_g2d_irq_action {
    uint8_t ack_mixer;    /* write 1 to 0x24, then pulse 0x08 bit 0 */
    uint8_t ack_rotate;   /* write the read ROT_INT word back to ROT_INT */
    uint8_t mask_mixer;   /* a stray mixer task end: clear 0x20 bit 4 */
    uint8_t mask_rotate;  /* a stray rotator finish: write ROT_INT with bits 16 and 0 clear */
    /* 0 nothing armed: mask the line at the distributor;
     * 1 the armed kind finished: complete the operation;
     * 2 armed, and neither finish set: fail the operation and mask the line, as today;
     * 3 only a stray: the operation stays armed and the owner is not woken. */
    uint8_t seen;
    uint8_t stray;        /* 1 when a finish other than the armed kind's was set: counted, its word recorded */
};
/* rcq_irq_ctl is 0x20, rcq_status 0x24 and rot_int ROT_INT, as the handler read them. A finish counts only while its
 * enable is set, so a stray masked earlier, whose status stays set, is not counted again. */
static inline struct a733_g2d_irq_action a733_g2d_irq_dispatch(unsigned armed, uint32_t rcq_irq_ctl, uint32_t rcq_status,
                                                               uint32_t rot_int)
{
    const unsigned mixer = (rcq_status & 1u) && (rcq_irq_ctl & 0x10u);
    const unsigned rotate = (rot_int & 1u) && (rot_int & 0x10000u);
    struct a733_g2d_irq_action a = {0, 0, 0, 0, 0, 0};
    if (armed == A733_G2D_IRQ_MIXER) {
        a.ack_mixer = (uint8_t)mixer; a.stray = a.mask_rotate = (uint8_t)rotate;
        a.seen = mixer ? 1 : rotate ? 3 : 2;
    } else if (armed == A733_G2D_IRQ_ROTATE) {
        a.ack_rotate = (uint8_t)rotate; a.stray = a.mask_mixer = (uint8_t)mixer;
        a.seen = rotate ? 1 : mixer ? 3 : 2;
    } else a.stray = (uint8_t)(mixer | rotate);
    return a;
}
/* The ROT_INT word that masks a stray rotator finish: its enable cleared, and 0 written to the status bit, which
 * leaves it set. */
static inline uint32_t a733_g2d_rot_int_masked(uint32_t rot_int) { return rot_int & ~UINT32_C(0x10001); }
#endif
