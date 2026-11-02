/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SHIM_DRAM_NSI_APP_H
#define SHIM_DRAM_NSI_APP_H
#include "sunstone_app.h"
#define DRAM_NSI_REGION_BUFFER 0
void dram_nsi_init(void);
void dram_nsi_main(const struct ss_grant *);
#endif
