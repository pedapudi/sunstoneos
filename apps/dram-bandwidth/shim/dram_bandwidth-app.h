/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIM_DRAM_BANDWIDTH_APP_H
#define SHIM_DRAM_BANDWIDTH_APP_H
#include "sunstone_app.h"
#define DRAM_BANDWIDTH_REGION_BUFFER 0
void dram_bandwidth_init(void);
void dram_bandwidth_main(const struct ss_grant *);
#endif
