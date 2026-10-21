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
/* Handlers call FreeRTOS FromISR functions, so their GIC priority byte must
 * not be numerically below the port's API ceiling: 11 << 4 in the secure-
 * monitor configuration, the value the existing USB host route (0xd8) meets. */
#define A733_IRQ_TABLE_MIN_PRIORITY 0xb0u
#ifdef portMAX_API_PRIORITY_MASK
_Static_assert(A733_IRQ_TABLE_MIN_PRIORITY>=portMAX_API_PRIORITY_MASK,"IRQ table API ceiling");
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
 * Returns 1 on success and 0 on any refusal; nothing is partially enabled. */
int a733_irq_table_route(uint64_t deadline, uint64_t (*counter)(void));
#endif
