/* SPDX-License-Identifier: GPL-2.0-only */
/* The A733 board's inputs to the hardware facts capture (platform/a733/hw_facts.c). */
#include "a733_hw_facts.h"
#include "a733_mmio.h"
#include "cached.h"
#include "FreeRTOS.h"

static uintptr_t redistributor(unsigned core) { return a733_cached_runtime[core].redistributor; }
static uint32_t read32(uintptr_t address) { return a733_mmio_read32(address); }
const struct hwf_platform hwf_platform = {
    .platform = HWF_PLATFORM_A733, .distributor = 0x03400000, .redistributor = redistributor, .read32 = read32,
    .api_mask = portMAX_API_PRIORITY_MASK, .ppi30_priority = portLOWEST_USABLE_INTERRUPT_PRIORITY << portPRIORITY_SHIFT,
};
