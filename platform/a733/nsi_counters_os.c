/* SPDX-License-Identifier: Apache-2.0
 * The OS owner of the NSI port 6 and 18 traffic counters (include/a733_nsi_counters.h), reached from the GPU owner task
 * through the GPU call gateway (drivers/gpu/gpu_queue.c). Each access checks that the page is mapped as Device memory.
 * A failed session's GPU quiesce (apps/os/gpu.c) restores any bank the application left programmed. */
#include "a733_nsi_counters.h"
#include "a733_mmio.h"
#include "aarch64_device_memory.h"
#include "a733_mapping_check.h"
#include <stdatomic.h>

static struct a733_nsi_counters owner;
static atomic_uint busy;   /* one call at a time: the owner task's call or the failure restore */
static int device_page(uintptr_t address)
{
    static const struct a733_mapping_rule device = {A733_MAPPING_READ_WRITE, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    return a733_mapping_check(aarch64_device_translate, address, address, &device);
}
static int read32(void *context, uintptr_t address, uint32_t *v)
{
    (void)context;
    if (!a733_nsi_counter_readable(address) || !device_page(address)) return 0;
    *v = a733_mmio_read32_dsb_sy_both(address);
    return 1;
}
static int write32(void *context, uintptr_t address, uint32_t v)
{
    (void)context;
    if (!a733_nsi_counter_writable(address) || !device_page(address)) return 0;
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
/* The failure path: restore and close, unless a call is in progress (the board's reset then restores the words). */
void os_nsi_counters_quiesce(void)
{
    if (atomic_exchange_explicit(&busy, 1, memory_order_acquire)) return;
    (void)a733_nsi_counters_close(&owner);
    atomic_store_explicit(&busy, 0, memory_order_release);
}
