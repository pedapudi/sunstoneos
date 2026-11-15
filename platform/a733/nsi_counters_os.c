/* SPDX-License-Identifier: Apache-2.0
 * The OS owner of the NSI port 6 and 18 traffic counters (include/a733_nsi_counters.h), reached from the GPU owner task
 * through the GPU call gateway (drivers/gpu/gpu_queue.c). Each access checks that the page is mapped as Device memory.
 * A failed session's GPU quiesce (apps/os/gpu.c) restores any bank the application left programmed.
 *
 * Port 6 sits in the GPU_TOP power domain (sun60iw2p1.dtsi, the nsi0 child gpu). Before that domain turns off, the GPU
 * backend's isolate step (platform/a733/gpu_backend.c) calls os_nsi_counters_power_off(0): the bank is restored and
 * every access to it is refused until the backend's power-on calls os_nsi_counters_power_on(0), so no access reaches a
 * bank whose domain is off. Port 18 sits in the NPU domain,
 * which stays on once the NPU has started (drivers/npu/a733_npu_startup.c, local stop). */
#include "a733_nsi_counters.h"
#include "a733_mmio.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"
#include <stdatomic.h>

static struct a733_nsi_counters owner;
static atomic_uint busy;   /* one call at a time: the owner task's call, the failure restore or a power-off */
static atomic_uint gated;  /* bit b: bank b's master domain is turning off or off; no access to it */
static int open_bank(uintptr_t address)
{
    for (unsigned b = 0; b < A733_NSI_COUNTER_BANKS; ++b) {
        const uintptr_t first = a733_nsi_counter_address(b, 0);
        if (address >= first && address < first + 4u * A733_NSI_COUNTER_WORDS)
            return !(atomic_load_explicit(&gated, memory_order_acquire) >> b & 1u);
    }
    return 0;
}
static int device_page(uintptr_t address)
{
    static const struct a733_mapping_rule device = {A733_MAPPING_READ_WRITE, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    return a733_mapping_check(aarch64_device_translate, address, address, &device);
}
static int read32(void *context, uintptr_t address, uint32_t *v)
{
    (void)context;
    if (!a733_nsi_counter_readable(address) || !open_bank(address) || !device_page(address)) return 0;
    *v = a733_mmio_read32_dsb_sy_both(address);
    return 1;
}
static int write32(void *context, uintptr_t address, uint32_t v)
{
    (void)context;
    if (!a733_nsi_counter_writable(address) || !open_bank(address) || !device_page(address)) return 0;
    a733_mmio_write32_dsb_sy_both(address, v);
    return 1;
}
static uint64_t ticks(void *context)
{
    (void)context;
    uint64_t t;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t)::"memory");
    return t;
}
static const struct a733_nsi_counter_ops ops = {read32, write32, ticks};
int os_nsi_counters_call(struct a733_nsi_counter_call *call)
{
    if (atomic_exchange_explicit(&busy, 1, memory_order_acquire)) { call->status = A733_NSI_C_REFUSED; return A733_NSI_C_REFUSED; }
    const int r = a733_nsi_counters_call(&owner, &ops, 0, call);
    atomic_store_explicit(&busy, 0, memory_order_release);
    return r;
}
/* Before bank's master domain turns off: restore the bank and refuse it from then on. A call in progress keeps the
 * lock, so the bank is only refused; the domain's power-off then resets its words. */
void os_nsi_counters_power_off(unsigned bank)
{
    if (bank >= A733_NSI_COUNTER_BANKS) return;
    if (!atomic_exchange_explicit(&busy, 1, memory_order_acquire)) {
        if (owner.opened) (void)a733_nsi_counters_restore(&owner, bank);
        atomic_fetch_or_explicit(&gated, 1u << bank, memory_order_release);
        atomic_store_explicit(&busy, 0, memory_order_release);
        return;
    }
    atomic_fetch_or_explicit(&gated, 1u << bank, memory_order_release);
}
/* After the domain is on again (the GPU backend's power-on): the bank's words are back at their hardware reset, and
 * accesses are allowed. */
void os_nsi_counters_power_on(unsigned bank)
{
    if (bank < A733_NSI_COUNTER_BANKS) atomic_fetch_and_explicit(&gated, ~(1u << bank), memory_order_release);
}
/* The failure path: restore and close, unless a call is in progress (the board's reset then restores the words). A
 * bank whose domain went off is refused, so its restore reports IO without an access. */
void os_nsi_counters_quiesce(void)
{
    if (atomic_exchange_explicit(&busy, 1, memory_order_acquire)) return;
    (void)a733_nsi_counters_close(&owner);
    atomic_store_explicit(&busy, 0, memory_order_release);
}
