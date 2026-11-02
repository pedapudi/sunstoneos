/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_A733_GIC_PRIORITY_H
#define SUNSTONE_A733_GIC_PRIORITY_H
/* Priority between the stored (Secure) view and the Non-secure view on the A733 (docs/hardware/a733-facts.md).
 *
 * The GIC has two security states (DS clear) and five priority bits. EL3 programs and compares priorities in the
 * stored view; a Group 1 interrupt read or written at Non-secure EL1 shows the Non-secure view: the stored byte
 * shifted left by one bit (IHI0069 4.8.1, Linux irq-gic-v3.c:95-115). A Non-secure write of `ns` stores
 * ((ns >> 1) | 0x80) & 0xf8. ICC_PMR_EL1 shows the same view. Example: the route priority 0xd8 of INTID 97 is
 * stored; a Non-secure read of GICD_IPRIORITYR returns 0xb0 (facts gic.dist.spi97.priority). Compare a priority
 * with a literal only through these macros; checks/hw-facts.py refuses other comparisons. */
#include <stdint.h>

#define A733_GIC_NS_VIEW(stored) ((uint8_t)((unsigned)(stored) << 1))
#define A733_GIC_STORED_FROM_NS(ns) ((uint8_t)((((unsigned)(ns) >> 1) | 0x80u) & 0xf8u))
/* An interrupt may call the FreeRTOS FromISR API when its stored priority is not below the stored value of the
 * port's mask, which is written and read as the Non-secure value `api_ns`. */
#define A733_GIC_STORED_API_FLOOR(api_ns) A733_GIC_STORED_FROM_NS(api_ns)

_Static_assert(A733_GIC_NS_VIEW(0xd8) == 0xb0, "stored 0xd8 reads 0xb0 at Non-secure EL1");
_Static_assert(A733_GIC_STORED_FROM_NS(0xd8) == 0xe8 && A733_GIC_NS_VIEW(0xe8) == 0xd0, "a Non-secure write of 0xd8 reads back 0xd0");
_Static_assert(A733_GIC_STORED_FROM_NS(0xb0) == 0xd8, "the API mask 0xb0 is stored as 0xd8");

/* Control class. A hard real-time handler runs at a stored priority numerically below the API floor, so the kernel's
 * mask (ICC_PMR_EL1 = the API mask) never delays it: the GIC signals an interrupt only when its priority, in the
 * Non-secure view, is numerically below the mask. Such a handler calls no kernel API. Write the priority register
 * with A733_GIC_NS_WRITE_FOR_STORED so that the stored value is the one named here. */
#define A733_GIC_NS_WRITE_FOR_STORED(stored) ((uint8_t)(((unsigned)(stored) << 1) & 0xffu))
#define A733_GIC_CONTROL_STORED 0xa0u
/* The GIC signals an interrupt to the core when its Non-secure-view priority is below the mask (Non-secure view). */
#define A733_GIC_SIGNALLED(stored, pmr_ns) (A733_GIC_NS_VIEW(stored) < (unsigned)(pmr_ns))

/* The task path of the comparison: the API floor itself, the highest priority that may call the FromISR API. */
#define A733_GIC_TASK_PATH_STORED A733_GIC_STORED_API_FLOOR(0xb0)
_Static_assert(A733_GIC_STORED_FROM_NS(A733_GIC_NS_WRITE_FOR_STORED(A733_GIC_TASK_PATH_STORED)) == A733_GIC_TASK_PATH_STORED, "the task path priority is the value stored by its Non-secure write");
_Static_assert(A733_GIC_STORED_FROM_NS(A733_GIC_NS_WRITE_FOR_STORED(A733_GIC_CONTROL_STORED)) == A733_GIC_CONTROL_STORED,
               "the control priority is the value stored by its Non-secure write");
_Static_assert(A733_GIC_CONTROL_STORED < A733_GIC_STORED_API_FLOOR(0xb0), "the control class is above the API floor");
_Static_assert(A733_GIC_SIGNALLED(A733_GIC_CONTROL_STORED, 0xb0), "the control class is signalled while the API mask is raised");
_Static_assert(!A733_GIC_SIGNALLED(A733_GIC_STORED_API_FLOOR(0xb0), 0xb0), "the API floor is held off by the API mask");
#endif
