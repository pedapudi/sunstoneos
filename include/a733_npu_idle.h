#ifndef SUNSTONE_A733_NPU_IDLE_H
#define SUNSTONE_A733_NPU_IDLE_H
#include "npu_idle.h"

/* VIPLite 2 internal clock gating (vip_drv_pm.c:142-220): clock control at
 * +0x000 (reload pulse bit 9, gate bit 0, scaler bits 8:2 held at 0x100),
 * shader scaler at +0x10c (pulse bit 0, scaler bits 7:1 held at 0x80) and the
 * all-units idle word at +0x004. Gating starts after 10 ms of idle time and
 * resume must finish within 100 us; no single wait exceeds 1 ms. */
static const struct npu_idle_part a733_npu_idle_part = {
    .clock = 0x000, .status = 0x004, .shader = 0x10c,
    .clock_pulse = 1u << 9, .clock_gate = 1u,
    .clock_field_mask = 0x1fc, .clock_field_value = 0x100,
    .shader_pulse = 1u, .shader_field_mask = 0xfe, .shader_field_value = 0x80,
    .full_idle = 0x7fffffff,
    .delay_us = 10000, .resume_us = 100, .hard_us = 1000,
};
#endif
