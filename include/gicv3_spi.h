#ifndef SUNSTONE_GICV3_SPI_H
#define SUNSTONE_GICV3_SPI_H
#include <stdint.h>

/* Immutable binding for one already-configured shared peripheral interrupt.
 * The platform owns its security group, route, level/edge mode and priority.
 * The distributor uses a mapped Device-nGnRnE address. Calls require EL1 and
 * external serialization with other users of this interrupt. SGIs/PPIs and
 * extended interrupt IDs are outside this interface.
 */
struct gicv3_spi { uintptr_t distributor; uint32_t intid; };

/* Bounded write + DSB, without an RWP spin loop. Success means the disable
 * command was issued, not that it propagated through the distributor. The
 * caller retains its own software mask until rearm finishes. MMIO aborts or
 * a stalled interconnect still require independent platform recovery.
 */
int gicv3_spi_mask(const struct gicv3_spi *);
/* One RWP read; returns 0 while propagation is pending, 1 after set-enable and
 * readback succeed, or -1 for an invalid binding/unwritable enable. A failed
 * readback issues disable before returning; its propagation can still require
 * platform recovery. Enable is not retried. The caller bounds deferred attempts.
 * Neither operation acknowledges, clears pending state, or sends EOI.
 */
int gicv3_spi_rearm(const struct gicv3_spi *);
#endif
