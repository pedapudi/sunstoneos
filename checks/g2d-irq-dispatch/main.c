/* Host driver of checks/g2d-irq-dispatch.py: one "armed rcq_status rot_int" line in, one "ack_mixer ack_rotate seen
 * stray" line out. */
#include "a733_g2d_irq.h"
#include <inttypes.h>
#include <stdio.h>

int main(void)
{
    unsigned armed;
    uint32_t rcq, rot;
    while (scanf("%u %" SCNx32 " %" SCNx32, &armed, &rcq, &rot) == 3) {
        const struct a733_g2d_irq_action a = a733_g2d_irq_dispatch(armed, rcq, rot);
        printf("%u %u %u %u\n", a.ack_mixer, a.ack_rotate, a.seen, a.stray);
    }
    return 0;
}
