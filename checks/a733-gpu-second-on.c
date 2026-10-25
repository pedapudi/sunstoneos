/* SPDX-License-Identifier: Apache-2.0 */
/* Two GPU ON/OFF cycles through the actual GPU platform, resource manager,
 * controller, supply consumer, PMIC provider, rail ledger and power-domain
 * owner, with the GPU master gates modeled from board-session-20261025-7
 * s11-gpo-run: a TOP5 OFF closes CCU 0x5c0 bit 7 (shown on the board) and
 * 0x5e0 bit 16 (not read there), and a write does not reopen them while TOP5
 * is OFF. A TOP5 ON reopens what the OFF closed, unless the case keeps them
 * closed; neither has run on a board.
 *
 * Built against the tree before the fix (-DBOARD_RUN), the run reproduces
 * the board's log of s11-gpo-run: cycle 1 ON reaches CORE6 status 8, the OFF
 * completes with its release, and the cycle 2 resource acquisition opens the
 * AHB gate, reads it back closed and fails: platform resource_result 3
 * (FAULT), manager error 7 (GATE), stage 8, last 0x1101037f, with the
 * controller still in its cycle 1 state (phase 4 OFF, error 0, step 18).
 * Built against the fixed tree, cycle 2 completes when TOP5 ON reopens the
 * gates, and fails at the controller's gate check after TOP5 ON, with TOP5
 * ON and the GPU clocked, when they stay closed.
 *
 * Build: clang-21 -std=c11 -O1 -g -Wall -Wextra -Werror
 *   -fsanitize=address,undefined -Iinclude -Ichecks
 *   checks/a733-gpu-second-on.c drivers/power/a733_pmic.c
 *   drivers/power/a733_rail_leases.c drivers/clock/a733_clock_leases.c
 *   drivers/gpu/a733_gpu_resources.c drivers/pinctrl/a733_pins.c
 *   drivers/pinctrl/a733_pin_tables.c drivers/power/a733_power_domains.c
 *   drivers/power/a733_pmic_consumer.c drivers/gpu/a733_gpu_power.c
 *   drivers/gpu/a733_gpu_platform.c */
#include "power-domains-model.h"
#include "a733_gpu_platform.h"
#include "a733_gpu_resources_native.h"

/* The native resource adapter's two binding calls, as in
 * checks/a733-gpu-supply-raise.c: that adapter is AArch64 code. */
int a733_gpu_resources_native_use_pmic(struct a733_gpu_resource_native *n,
    struct a733_gpu_resource_manager *manager, struct a733_pmic *p)
{
    if (n->pmic) return n->pmic == p && n->pmic_manager == manager;
    n->pmic = p; n->pmic_manager = manager; return 1;
}
int a733_gpu_resources_native_bind_view(struct a733_gpu_resource_native *n, struct a733_pmic_consumer *c,
    unsigned client)
{
    if (n->pmic_view || c->config.provider != n->pmic) return 0;
    c->recipient = n; c->destination = &n->pmic_manager->rail; n->pmic_view = c; n->pmic_client = client;
    return 1;
}

static struct a733_gpu_platform gpu;
static struct a733_gpu_resource_native native;
static uint32_t core[0x24/4]; /* CORE6 policy..PWCR, by word */
static unsigned core_pending, gate_writes;

/* CORE6's PPU (always-on PCK block): a pending transition completes at a
 * status read once the GPU is clocked and out of reset. */
static uint32_t core_read(uint32_t off)
{
    if (off == 8 && core_pending && (d.ccu_b20 & 0x80000000) && (d.ccu_b24 & 0x10001) == 0x10001) {
        core[2] = core[0]; core_pending = 0;
    }
    return off == 0xfb0 ? 0x10130101 : off == 0x14 ? (core[2] == 8 ? 0x100 : 0) : core[off/4];
}
static uint32_t *ccu_word(uintptr_t a)
{
    uint32_t *w = level_word(a); assert(w);
    /* PLL_GPU0 locks once enabled with its LDO and lock enable. */
    if (a == CCU_BASE + 0xe0)
        *w = (*w & 0xe0000000) == 0xe0000000 ? *w | (1u << 28) : *w & ~(1u << 28);
    return w;
}
/* A master-gate write takes only while TOP5 is ON. */
static void gate_write(uint32_t *w, uint32_t bit, uint32_t value)
{
    ++gate_writes;
    if ((d.ppu[5][2] & 15) == 8) *w = (*w & ~bit) | (value & bit);
}

/* Resource-manager callbacks. */
static int res_read(void *v, uintptr_t a, uint32_t *out)
{
    (void)v;
    if (a == 0x07090160) { *out = *pmic_model_word(&m, a); return 1; }
    if (a == 0x07065008) { *out = d.ppu[5][2]; return 1; }
    if (a == 0x07066000 || a == 0x07066008) { *out = core_read((uint32_t)(a - 0x07066000)); return 1; }
    *out = *ccu_word(a); return 1;
}
static int res_update(void *v, uintptr_t a, uint32_t required, uint32_t set, uint64_t deadline)
{
    (void)v; (void)deadline;
    uint32_t *w = level_word(a); assert(w && (*w & required) == required);
    if (a == CCU_BASE + 0x5c0) gate_write(w, 1u << 7, set);
    else if (a == CCU_BASE + 0x5e0) gate_write(w, 1u << 16, set);
    else *w |= set;
    return 1;
}
static int res_rail(void *v, struct a733_gpu_resource_rail *out, uint64_t deadline)
{
    (void)v; (void)deadline;
    *out = (struct a733_gpu_resource_rail){0, 3, 0, m.pmic[0x10], m.pmic[0x15], 0};
    return 1;
}
/* Controller register callbacks of the platform. */
static int gpu_now(void *v, uint64_t *out) { (void)v; return pmic_model_now(&m, out); }
static int gpu_read(void *v, enum a733_gpu_power_bank b, uint32_t off, uint32_t *out)
{
    (void)v;
    if (b == A733_GPU_CORE) { *out = core_read(off); return 1; }
    *out = *ccu_word((b == A733_GPU_CCU ? CCU_BASE : RCCU_BASE) + off); return 1;
}
static int gpu_update(void *v, enum a733_gpu_power_bank b, uint32_t off, uint32_t c, uint32_t s, uint64_t dl)
{
    (void)v; (void)dl;
    if (b == A733_GPU_CORE) { core[off/4] = s; if (!off) core_pending = 1; return 1; }
    assert(b == A733_GPU_CCU && (off == 0xb20 || off == 0xb24 || off == 0xe0));
    uint32_t *w = level_word(CCU_BASE + off); *w = (*w & ~c) | s;
    if (off == 0xb20) *w &= ~(1u << 27);
    return 1;
}

static int step(int (*poll)(void *), void *arg)
{
    for (unsigned i = 0; i < 400; ++i) {
        const int r = poll(arg);
        if (r) return r;
        service_step(); m.now += TICK_US;
    }
    assert(0); return 0;
}
static int off_poll(void *p) { return a733_gpu_platform_off_poll(p); }

/* Returns the result of the cycle 2 ON poll. */
static int run(unsigned stuck)
{
    domains_model_setup(0x28, 23);
    d.ack_us = 0; d.top5_gates = 1; d.top5_gates_stuck = stuck;
    /* The board: TOP5 ON from reset, the CCU's reset gate words, the
     * power-up CORE6 state and DCDC4 at 800 mV with bit 7. */
    d.ppu[5][0] |= 8; d.ppu[5][2] |= 8;
    d.ccu_5c0 = 0x110103ff; d.ccu_5e0 = 0xf1055803; d.ccu_b20 = d.ccu_b24 = 0;
    memset(core, 0, sizeof(core)); core[0] = 8; core[8] = A733_GPU_CORE_RESET_PWCR; core_pending = 1;
    gate_writes = 0;
    m.pmic[0x15] = 0x80 | A733_GPU_DCDC4_BOOT;
    m.gpu.ops.read32 = res_read; m.gpu.ops.update32 = res_update; m.gpu.ops.rail = res_rail;
    memset(&gpu, 0, sizeof(gpu)); memset(&native, 0, sizeof(native)); native.initialized = 1;
    static const struct a733_gpu_platform_ops ops = {gpu_now, gpu_read, gpu_update};
    assert(a733_gpu_platform_init(&gpu, &m.gpu, 3, &ops, 0));
    assert(a733_gpu_platform_bind_pmic(&gpu, &native, &m.provider, &m.lifetime, 9));
    domains_boot();
    /* Cycle 1. */
    assert(a733_gpu_platform_begin(&gpu, m.now + 2000000));
    assert(step(a733_gpu_platform_poll, &gpu) == 1);
    assert(core[2] == 8 && gpu.power.core_note[A733_GPU_NOTE_AFTER_POLICY] == 8 &&
           gpu.power.pll_word == 0xf9105300 && (d.ppu[5][2] & 15) == 8);
    assert(a733_gpu_platform_off_begin(&gpu, m.now + 2000000));
    assert(step(off_poll, &gpu) == 1);
    assert(gpu.resource_result == A733_GPU_RESOURCE_OK && !gpu.token && !(d.ppu[5][2] & 15) && !core[2]);
    assert(gpu.power.phase == A733_GPU_POWER_OFF && !gpu.power.error);
    /* The modeled TOP5 OFF closed both gates. */
    assert(d.ccu_5c0 == 0x1101037f && d.ccu_5e0 == 0xf1045803);
    /* Cycle 2. */
    assert(a733_gpu_platform_begin(&gpu, m.now + 2000000));
    return step(a733_gpu_platform_poll, &gpu);
}

int main(void)
{
#ifdef BOARD_RUN
    /* The tree before the fix: the log of s11-gpo-run. */
    assert(run(0) == -1);
    const struct a733_gpu_power *p = &gpu.power;
    assert(p->phase == A733_GPU_POWER_OFF && p->error == A733_GPU_POWER_NO_ERROR && p->index == 18 && !p->last_value);
    assert(gpu.resource_result == A733_GPU_RESOURCE_FAULT && m.gpu.error == A733_GPU_RESOURCE_GATE &&
           m.gpu.stage == 8 && m.gpu.last_value == 0x1101037f && gate_writes == 1);
    assert(p->core_seen == 0xff && p->core_policy == 8 && !p->core_status && p->core_config == 0x101);
    assert(p->core_note[A733_GPU_NOTE_IDR0] == 0x10130101 && !p->core_note[A733_GPU_NOTE_MISR] &&
           p->core_note[A733_GPU_NOTE_MISR_LATE] == 0x100 && !p->core_note[A733_GPU_NOTE_AFTER_PWCR] &&
           p->core_note[A733_GPU_NOTE_AFTER_POLICY] == 8);
    assert(p->pll_word == 0x1105300 && !p->pll_lock_reads[0] && !p->clock_update_reads[0]);
    printf("PASS board run reproduced: GPU power phase=%u error=%u step=%u last=0x%x; "
           "GPU resources result=%u error=%u stage=%u last=0x%x; pll=0x%x\n",
           p->phase, p->error, p->index, p->last_value, gpu.resource_result, m.gpu.error, m.gpu.stage,
           m.gpu.last_value, p->pll_word);
#else
    /* The fix: the acquisition leaves the closed gates to the controller,
     * TOP5 ON reopens them, and cycle 2 completes with no gate write. */
    assert(run(0) == 1);
    const struct a733_gpu_power *p = &gpu.power;
    assert(gpu.resource_result == A733_GPU_RESOURCE_OK && !gate_writes && core[2] == 8 &&
           p->pll_word == 0xf9105300 && p->pll_lock_reads[0] && p->core_policy == 0 && !p->core_config);
    assert(p->core_note[A733_GPU_NOTE_AHB_ENTRY] == 0x1101037f && p->core_note[A733_GPU_NOTE_MBUS_ENTRY] == 0xf1045803 &&
           p->core_note[A733_GPU_NOTE_AHB_TOP5_ON] == 0x110103ff && p->core_note[A733_GPU_NOTE_MBUS_TOP5_ON] == 0xf1055803 &&
           p->core_note[A733_GPU_NOTE_AHB_TOP5_OFF] == 0x1101037f && p->core_note[A733_GPU_NOTE_AHB_OFF_END] == 0x1101037f);
    assert(a733_gpu_platform_off_begin(&gpu, m.now + 2000000) && step(off_poll, &gpu) == 1);
    assert(d.top5_reopens == 1 && !(d.ppu[5][2] & 15));
    /* Gates that stay closed after TOP5 ON: the controller fails at its
     * AHB check after TOP5 ON, with the word, TOP5 ON, the GPU clocked and
     * CORE6 policy 0; the resources and the row stay held. */
    assert(run(1) == -1);
    assert(gpu.phase == A733_GPU_PLATFORM_FAILED && p->phase == A733_GPU_POWER_FAILED);
    assert(p->error == A733_GPU_POWER_STATE && p->index == 51 && p->last_value == 0x1101037f &&
           (d.ppu[5][2] & 15) == 8 && (d.ccu_b24 & 0x10001) == 0x10001 && d.ccu_e0 == 0xf9105300 &&
           !core[0] && gpu.token && p->row.counted && !gate_writes);
    puts("PASS GPU second ON: with a modeled TOP5 OFF that closes both master gates, cycle 2 defers them at acquisition, TOP5 ON "
         "reopens them and the ON completes with no gate write; gates that stay closed fail at the check "
         "after TOP5 ON with TOP5 ON, the GPU clocked and the resources held");
#endif
    return 0;
}
