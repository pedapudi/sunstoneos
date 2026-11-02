/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIM_DRAM_SCALING_APP_H
#define SHIM_DRAM_SCALING_APP_H
#include "sunstone_app.h"
#define DRAM_SCALING_REGION_BUFFER 0
void dram_scaling_init(void);
void dram_scaling_main(const struct ss_grant *);
#endif
