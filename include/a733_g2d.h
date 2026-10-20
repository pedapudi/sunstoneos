/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_G2D_H
#define SUNSTONE_A733_G2D_H
#include <stddef.h>
#include <stdint.h>
#include "a733_g2d_application.h"
#define A733_G2D_BASE UINT32_C(0x05440000)
#define A733_G2D_SOURCE_IOVA UINT32_C(0x10000000)
#define A733_G2D_OUTPUT_IOVA UINT32_C(0x10400000)
#define A733_G2D_RCQ_IOVA UINT32_C(0x10800000)
#define A733_G2D_MODULE UINT32_C(0x01000003)
/* One immutable job, no reclamation or reset on any outcome. RCQ memory is a
 * separate zeroed, uncached 4096-byte READ-only IOMMU mapping. */
int a733_g2d_commands(void *,size_t,const struct a733_g2d_session *);
#endif
