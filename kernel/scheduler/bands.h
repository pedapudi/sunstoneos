/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_SCHEDULER_BANDS_H
#define SS_SCHEDULER_BANDS_H
/* Reschedule bands (docs/designs/scheduler-partitioned.md, "Reschedule bands:
 * the GIC holds publications that cannot preempt").
 *
 * Task priorities fall into three bands, and each band has its own reschedule
 * SGI: band b publishes with SGI b. While a core runs a task of band b, its
 * GIC priority mask outside the API mask is the priority of SGI b - 1, so the
 * core takes the SGIs of its own band and above and holds only lower bands'.
 * A held publication is for a task of a strictly lower band, which could not
 * preempt; the GIC keeps it pending until the core's band falls. Band 1 (and
 * the idle task) opens every priority.
 *
 * SGI 0 carries every reason other than a publication (owner commands, the
 * control count, migration, yield) at the API level, as do the scheduler tick
 * and every device interrupt, so no band mask holds them; the API mask holds
 * them all. The platform writes the SGI priorities on each core and sets
 * configSS_BAND_SGIS to 1; without it publications use SGI 0 and every core
 * opens every priority, as before. Include after FreeRTOS.h. */
#include <stdint.h>

#ifndef configSS_BAND_SGIS
#define configSS_BAND_SGIS 0
#endif
#if configSS_BAND_SGIS
#define SS_BANDS 3u
/* The GIC priority of SGI b (1-3): the three lowest usable levels, band 1
 * lowest, so a device interrupt at any level from the API level up to the
 * level above SGI 3 stays above every band mask. On the A733 (16 levels at
 * Non-secure EL1) SGIs 1-3 read 0xe0, 0xd0 and 0xc0; on QEMU (32 levels)
 * 0xf0, 0xe8 and 0xe0, which keeps the QEMU routes at 0xd8 above them. */
#define SS_BAND_SGI_PRIORITY(b) \
    ((uint8_t)(((unsigned)portLOWEST_USABLE_INTERRUPT_PRIORITY + 1u - (unsigned)(b)) << portPRIORITY_SHIFT))
/* The mask while band b runs: the priority of SGI b - 1; band 1 opens all. */
#define SS_BAND_MASK(b) ((b) <= 1u ? (uint8_t)0xffu : SS_BAND_SGI_PRIORITY((b) - 1u))
/* SGI 0 and the tick: the API level, held only by the API mask. */
#define SS_BAND_COMMAND_PRIORITY ((uint8_t)((unsigned)configMAX_API_CALL_INTERRUPT_PRIORITY << portPRIORITY_SHIFT))
_Static_assert(configMAX_API_CALL_INTERRUPT_PRIORITY + 3u <= portLOWEST_USABLE_INTERRUPT_PRIORITY,
               "three band levels fit between the API level and the lowest usable priority");
_Static_assert(SS_BAND_COMMAND_PRIORITY == portMAX_API_PRIORITY_MASK, "SGI 0 and the tick sit at the API level");

/* Band of each task priority, two bits per priority: 1 = priorities 0-3,
 * 2 = 4-5, 3 = 6-7. */
#define SS_BAND_TABLE 0xfa55u
_Static_assert(configMAX_PRIORITIES == 8, "the band table names eight task priorities");
static inline unsigned ss_band(unsigned priority)
{ return (SS_BAND_TABLE >> (2u * (priority & 7u))) & 3u; }
/* The open mask of each task priority, one byte per priority. */
#define SS_BAND_MASK_OF(p) ((uint64_t)SS_BAND_MASK((SS_BAND_TABLE >> (2u * (p))) & 3u) << (8u * (p)))
#define SS_BAND_MASKS (SS_BAND_MASK_OF(0u) | SS_BAND_MASK_OF(1u) | SS_BAND_MASK_OF(2u) | SS_BAND_MASK_OF(3u) | \
                       SS_BAND_MASK_OF(4u) | SS_BAND_MASK_OF(5u) | SS_BAND_MASK_OF(6u) | SS_BAND_MASK_OF(7u))
static inline unsigned ss_band_mask(unsigned priority)
{ return (unsigned)(SS_BAND_MASKS >> (8u * (priority & 7u))) & 0xffu; }

#ifdef SUNSTONE_LATENCY_BAND_SWITCH
/* Measurement build only. Bit SS_BAND_SWITCH_SHIFT of the CPU's generic
 * counter turns the band masks on (set) or off (clear: every core opens every
 * priority, as before the bands), so one run holds both settings. The SGI
 * priorities and the band of each publication do not change. 2^20 counts are
 * 43.7 ms at 24 MHz, so one setting covers about 43 wake periods of the
 * latency fixture and a probe window rarely crosses a change. Each core keeps
 * the setting it applied last (ss_band_switch_on). */
#define SS_BAND_SWITCH_SHIFT 20u
struct ss_band_switch_core { _Alignas(64) volatile uint32_t on; };
extern struct ss_band_switch_core ss_band_switch[];
static inline unsigned ss_band_switch_mask(unsigned c, unsigned mask)
{
    uint64_t now;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
    unsigned on = (unsigned)(now >> SS_BAND_SWITCH_SHIFT) & 1u;
    ss_band_switch[c].on = on;
    return on ? mask : 0xffu;
}
#endif
#endif
#endif
