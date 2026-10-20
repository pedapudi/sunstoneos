/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_DEINTERLACE_H
#define SUNSTONE_A733_DEINTERLACE_H
#include "a733_deinterlace_application.h"
#define A733_DEINTERLACE_BASE UINT32_C(0x05400000)
#define A733_DEINTERLACE_SOURCE_IOVA UINT32_C(0x10000000)
#define A733_DEINTERLACE_OUTPUT_IOVA UINT32_C(0x10400000)
#define A733_DEINTERLACE_CURRENT_IOVA UINT32_C(0x10800000)
#define A733_DEINTERLACE_FLAG_IN_IOVA UINT32_C(0x10c00000)
#define A733_DEINTERLACE_FLAG_OUT_IOVA UINT32_C(0x10c20000)
#define A733_DEINTERLACE_WEAVE_WORDS 34u
#define A733_DEINTERLACE_MODULE UINT32_C(0x00000001)
#define A733_DEINTERLACE_PLAN_BYTES 0x5000u
/* CPU-only register generation. This image is never mapped into master9. */
struct a733_deinterlace_plan {uint32_t words[A733_DEINTERLACE_PLAN_BYTES/4];uintptr_t self;unsigned ready;};
struct a733_deinterlace_word {uint32_t offset,mask;};
int a733_deinterlace_commands(struct a733_deinterlace_plan *,const struct a733_deinterlace_session *);
const struct a733_deinterlace_word *a733_deinterlace_words(size_t *);
int a733_deinterlace_plan_valid(const struct a733_deinterlace_plan *);
void a733_deinterlace_bitfields(uint32_t [6]);
#endif
