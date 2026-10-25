/* SPDX-License-Identifier: Apache-2.0
 * The arguments of the vendor-state supply unit cases, written by
 * vendor-state-units.py before each call. */
#ifndef SUNSTONE_GPU_VENDOR_STATE_UNITS_SUPPLY_H
#define SUNSTONE_GPU_VENDOR_STATE_UNITS_SUPPLY_H
#include <stdint.h>
struct vsu_supply {
    uint32_t rail, target;
    uint32_t page_before, chip, revision, enable, selector, page_after;
};
extern volatile struct vsu_supply vsu_supply;
#endif
