/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_ISP0_REPLAY_H
#define SUNSTONE_ISP0_REPLAY_H
#include "a733_isp0.h"
/* Reuse the same immutable input, parameters and DEVICE-owned output extents.
 * The caller must first retire its copied output consumer and complete DMA
 * retained readout. This clears fresh ISP/VIPP flags and reloads parameters;
 * it neither starts TOP nor proves global drain or releases any allocation.
 * Returns 0 while pending, 1 when a separate trigger is allowed, -1 on fault.
 * No a733_isp0_poll/trigger/complete calls may interleave with this operation. */
int a733_isp0_rearm(struct a733_isp0 *);
#endif
