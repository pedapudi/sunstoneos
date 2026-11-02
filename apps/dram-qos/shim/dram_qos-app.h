/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIM_DRAM_QOS_APP_H
#define SHIM_DRAM_QOS_APP_H
#include "sunstone_app.h"
#define DRAM_QOS_REGION_BUFFER 0
void dram_qos_init(void);
void dram_qos_main(const struct ss_grant *);
#endif
