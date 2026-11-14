/* complete() of drivers/npu/npu_runtime_queue.c, at a given revision, against a register model of the
 * read-to-acknowledge event word (+0x010) and the all-units idle word (+0x004). checks/npu-completion-idle.py
 * extracts ticks(), window(), slot_of() and complete() from the driver source, keeps their line numbers with
 * #line so that SUNSTONE_CRUMB_FAIL_SITE carries the board's site, and replaces only the system-register reads,
 * the barriers, the raw MMIO reads and the crumb SVC. Nothing else of the driver is changed.
 *
 * Board records this model reproduces (docs/evidence/npu.jsonl, docs/evidence/board-runner.jsonl):
 *   1794506252076994875-247497f5 and 1794596079966938149-24a0a868 stopped with ESR 0x4001a072 and FAR 0x7fffbffe:
 *   site line 114, register word index 1 (+0x004), value the idle word with bits 0 and 14 clear.
 *   1794506933827430720-5ffeeae8 and 1794508867203238346-402089f4, with the bounded wait, recorded idle_busy_bits
 *   0x00004001 and waits of at most 0.25 us on the subsampling graph only. */
#include "npu_runtime_queue.h"
#include "a733_npu_idle.h"
#include "sunstone_crumb.h"
#include "strict_mmio.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define NPU_BASE ((uintptr_t)0x03600000u)
#define FREQUENCY UINT64_C(24000000) /* CNTFRQ_EL0 of the A733 */
#define FULL_IDLE UINT32_C(0x7fffffff)
#define BUSY_FE_MC UINT32_C(0x7fffbffe)
#define SETTLE_TICKS 6u /* 0.25 us at 24 MHz: the longest wait of runs 5ffeeae8 and 402089f4 */

static uint64_t clock_ticks;
static uint64_t host_ticks(void) { return ++clock_ticks; }
static void host_dsb(void) {}
static uint32_t host_read32(uintptr_t address) { ++clock_ticks; return (uint32_t)strict_mmio_read(address, 4); }
static unsigned crumbs;
static uint32_t crumb_site, crumb_value;
static void host_crumb_fail(uint32_t site, uint32_t value, const void *caller)
{
    (void)caller;
    if (!crumbs++) { crumb_site = site; crumb_value = value; }
}

#include "npu_completion_extract.h"

static uint64_t settle_at;
static uint32_t busy_word;
static uint64_t read_events(struct strict_reg *reg)
{
    const uint64_t value = reg->value; reg->value = 0; /* read-to-acknowledge */
    return value;
}
static uint64_t read_status(struct strict_reg *reg)
{
    (void)reg;
    return clock_ticks < settle_at ? busy_word : FULL_IDLE;
}
static struct strict_reg regs[] = {
    {NPU_BASE + 0x010, 4, "events",
     "vip2/os/linux/vip_drv_device_driver.c:382 drv_irq_handler: one read of +0x010 per interrupt, read-to-acknowledge",
     0, read_events, NULL, NULL, 0, 0},
    {NPU_BASE + 0x004, 4, "idle",
     "vip2/vip_drv_hardware.c:1045-1089 vipdrv_hw_wait_idle and its module names (bit 0 FE, bit 14 MC), mask 0x7fffffff "
     "(vip_drv_hardware.h:69); busy word and settle time from board runs 1794506933827430720-5ffeeae8 and "
     "1794508867203238346-402089f4 (idle_busy_bits 0x4001, longest wait 0.25 us)",
     0, read_status, NULL, NULL, 0, 0},
};

static struct a733_npu_startup startup;
static struct npu_runtime runtime;
static struct npu_runtime_queue queue;

static int scenario(uint32_t events, uint32_t busy, uint64_t settle)
{
    memset(&runtime, 0, sizeof runtime); memset(&queue, 0, sizeof queue);
    atomic_store(&startup.window_open, 1u);
    runtime.startup = &startup;
    runtime.native.registers = NPU_BASE;
    runtime.service.signals = &runtime.service.own_signals;
    runtime.service.direct = &queue.direct;
    queue.runtime = &runtime; queue.claimed = 1; queue.frequency = FREQUENCY;
    queue.started = 0; queue.timeout_ticks = FREQUENCY; /* 1 s: the job deadline plays no part */
    clock_ticks = 0; crumbs = 0; crumb_site = crumb_value = 0;
    busy_word = busy; settle_at = settle;
    strict_mmio_define(regs, sizeof regs / sizeof regs[0]);
    regs[0].value = events;
    const int done = complete(&queue);
    assert(!strict_mmio_refusals());
    printf("events %#x busy %#x settle %llu: complete %d crumbs %u site %#x value %#x wait %llu busy_bits %#x\n",
           events, busy, (unsigned long long)settle, done, crumbs, crumb_site, crumb_value,
           (unsigned long long)queue.idle_wait_ticks, queue.idle_busy_bits);
    return done;
}

/* The decoder's register offset (tools/a733-crumbs.py): (aux & 0x3fff) << 2, aux the site's bits 31:16. */
static unsigned crumb_offset(void) { return ((crumb_site >> 16) & 0x3fffu) << 2; }

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "board-crumb")) {
        /* The revision of the failing images: one idle read after the event. */
        assert(scenario(1, BUSY_FE_MC, SETTLE_TICKS) == 0);
        assert(crumbs == 1 && crumb_site == UINT32_C(0x4001a072) && crumb_value == BUSY_FE_MC && crumb_offset() == 0x4);
    } else if (!strcmp(argv[1], "events-reading")) {
        /* Had the event word itself read 0x7fffbffe, the crumb would name +0x010 at the event check, not +0x004. */
        assert(scenario(BUSY_FE_MC, FULL_IDLE, 0) == 0);
        assert(crumbs == 1 && crumb_offset() == 0x10 && crumb_site != UINT32_C(0x4001a072) && crumb_value == BUSY_FE_MC);
    } else if (!strcmp(argv[1], "bounded")) {
        /* The bringup revision: the same register sequence completes after the settle, recording the busy units. */
        assert(scenario(1, BUSY_FE_MC, SETTLE_TICKS) == 1);
        assert(!crumbs && queue.idle_busy_bits == 0x4001u);
        assert(queue.idle_wait_ticks >= 1 && queue.idle_wait_ticks <= SETTLE_TICKS);
    } else if (!strcmp(argv[1], "bounded-stuck")) {
        /* A unit still busy at the 100 us bound remains a fault; its value names the units and the slot (none: 0xff). */
        assert(scenario(1, BUSY_FE_MC, UINT64_MAX) == 0);
        assert(crumbs == 1 && crumb_offset() == 0x4 && crumb_value == UINT32_C(0xff004001));
        assert(clock_ticks >= (uint64_t)NPU_IDLE_WAIT_US * FREQUENCY / 1000000u);
    } else assert(!"unknown case");
    printf("PASS %s\n", argv[1]);
    return 0;
}
