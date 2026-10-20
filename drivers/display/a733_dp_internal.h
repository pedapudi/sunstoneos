/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_DP_INTERNAL_H
#define SUNSTONE_A733_DP_INTERNAL_H
#include "a733_dp.h"
int a733_dp_read(struct a733_dp *,uintptr_t,unsigned,uint32_t *);
int a733_dp_write(struct a733_dp *,uintptr_t,unsigned,uint32_t);
int a733_dp_wait(struct a733_dp *,unsigned);
int a733_dp_fault(struct a733_dp *,enum a733_dp_error);
int a733_dp_cadence_prepare(struct a733_dp *);
int a733_dp_cadence_levels(struct a733_dp *,const uint8_t[4]);
#endif
