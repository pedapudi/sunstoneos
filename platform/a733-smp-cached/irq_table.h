#ifndef SUNSTONE_A733_IRQ_TABLE_H
#define SUNSTONE_A733_IRQ_TABLE_H
#include <stdint.h>
/* One fixed table of shared peripheral interrupts for an image that combines
 * several device owners. Each entry names the interrupt ID (INTID), the one
 * core that handles it, its GIC priority byte and the owner's handler. The
 * image defines a733_irq_routes and a733_irq_route_count; there is no runtime
 * registration. An interrupt outside the table, or delivered to another core,
 * keeps the existing status-4 stop. */
struct a733_irq_route {
    uint16_t intid;
    uint8_t core;
    uint8_t priority;
    void (*handler)(void);
};
/* Images define the table const; the host model varies it per case. */
#ifndef A733_IRQ_TABLE_CONST
#define A733_IRQ_TABLE_CONST const
#endif
extern A733_IRQ_TABLE_CONST struct a733_irq_route a733_irq_routes[];
extern A733_IRQ_TABLE_CONST unsigned a733_irq_route_count;
/* Handlers call FreeRTOS FromISR functions, so their stored (Secure-view)
 * GIC priority must not be numerically below the stored value of the port's
 * API mask. The port writes the mask 11 << 4 = 0xb0 at Non-secure EL1, which
 * the GIC stores as 0xd8 (docs/hardware/a733-facts.md, "Priority views"). */
#include "a733_gic_priority.h"
#define A733_IRQ_TABLE_MIN_PRIORITY 0xd8u
#ifdef portMAX_API_PRIORITY_MASK
_Static_assert(A733_IRQ_TABLE_MIN_PRIORITY>=A733_GIC_STORED_API_FLOOR(portMAX_API_PRIORITY_MASK),"IRQ table API ceiling");
#endif
/* Every INTID is a distinct SPI (32..1019), every core is below eight, every
 * handler is set and every priority meets A733_IRQ_TABLE_MIN_PRIORITY. */
int a733_irq_table_valid(void);
/* IRQ context: run the handler routed to this INTID and core; 0 otherwise. */
int a733_irq_table_dispatch(unsigned intid, unsigned core);
/* Primary EL3 boot only, after common boot has disabled every SPI and before
 * the distributor groups are re-enabled. Programs each entry as a level-
 * sensitive nonsecure Group 1 SPI routed to its core and verifies the result
 * before the deadline (counter ticks). Enabling stays with the device owner.
 * It waits for GICD_CTLR.RWP before reading the distributor state and after
 * the route writes. Returns A733_IRQ_TABLE_ROUTED on success,
 * A733_IRQ_TABLE_RWP_TIMEOUT if RWP stays set until the deadline, and
 * A733_IRQ_TABLE_REFUSED on any other refusal; nothing is partially enabled. */
#define A733_IRQ_TABLE_REFUSED 0
#define A733_IRQ_TABLE_ROUTED 1
#define A733_IRQ_TABLE_RWP_TIMEOUT 2
int a733_irq_table_route(uint64_t deadline, uint64_t (*counter)(void));
#endif
