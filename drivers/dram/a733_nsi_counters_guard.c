/* SPDX-License-Identifier: Apache-2.0
 * The OS owner of the NSI port 6 and 18 traffic counters (include/a733_nsi_counters.h) and its power-domain guard.
 * The platform supplies the accesses and an IRQ mask (platform/a733/nsi_counters_os.c); this unit holds the owner,
 * serializes its callers and refuses every access to a bank whose master's power domain is not on.
 *
 * Port 6 sits in the GPU_TOP power domain (sun60iw2p1.dtsi, nsi0 child gpu). The GPU power controller
 * (drivers/gpu/a733_gpu_power.c) calls os_nsi_counters_power_off(0) when a power-off begins, before any OFF step, so
 * the bank is restored while GPU_TOP is still on; the call returns with the bank refused and no access to it in
 * flight. Until the controller has finished a power-on and calls os_nsi_counters_power_on(0), every access to the
 * bank is refused without reaching the platform. The bank starts refused: the GPU is off until its first power-on.
 * Port 18 sits in the NPU domain, which stays on once the NPU has started (drivers/npu/a733_npu_startup.c refuses
 * every NPU domain OFF). checks/nsi-counters-gpu-power.c runs this unit with the GPU power controller and a modeled
 * GPU_TOP. */
#include "a733_nsi_counters.h"
#include <stdatomic.h>

const struct a733_nsi_counter_ops *os_nsi_counters_ops(void);
uint64_t os_nsi_counters_mask(void);         /* masks IRQs on this core; returns the previous mask */
void os_nsi_counters_unmask(uint64_t mask);

static struct a733_nsi_counters owner;
static atomic_uint busy;          /* one caller at a time: the owner task's call, a power transition or the quiesce */
static atomic_uint gated = 1u;    /* bit b: bank b's master domain is not on; bank 0 until the GPU's first power-on */
static atomic_uint inflight;      /* bit b: an access to bank b has passed the gate and not yet finished */

/* Each access sets its bank's in-flight bit before it tests the gate, with IRQs masked so that it cannot be
 * preempted in between; a power-off sets the gate before it tests the in-flight bit. With sequentially consistent
 * operations either the access sees the gate or the power-off sees the access and waits for it. */
static int bank_of(uintptr_t address, unsigned *bank)
{
    for (unsigned b = 0; b < A733_NSI_COUNTER_BANKS; ++b) {
        const uintptr_t first = a733_nsi_counter_address(b, 0);
        if (address >= first && address < first + 4u * A733_NSI_COUNTER_WORDS) { *bank = b; return 1; }
    }
    return 0;
}
static int guarded(uintptr_t address, int write, uint32_t *v)
{
    unsigned b;
    if (!bank_of(address, &b)) return 0;
    const uint64_t mask = os_nsi_counters_mask();
    atomic_fetch_or(&inflight, 1u << b);
    const int ok = !(atomic_load(&gated) >> b & 1u) &&
        (write ? os_nsi_counters_ops()->write(0, address, *v) : os_nsi_counters_ops()->read(0, address, v));
    atomic_fetch_and(&inflight, ~(1u << b));
    os_nsi_counters_unmask(mask);
    return ok;
}
static int guarded_read(void *context, uintptr_t address, uint32_t *v) { (void)context; return guarded(address, 0, v); }
static int guarded_write(void *context, uintptr_t address, uint32_t v) { (void)context; return guarded(address, 1, &v); }
static uint64_t guarded_ticks(void *context) { (void)context; return os_nsi_counters_ops()->ticks(0); }
static const struct a733_nsi_counter_ops guarded_ops = {guarded_read, guarded_write, guarded_ticks};

static int take(void) { return !atomic_exchange_explicit(&busy, 1, memory_order_acquire); }
static void give(void) { atomic_store_explicit(&busy, 0, memory_order_release); }
static int refused_bank(unsigned bank) { return bank >= A733_NSI_COUNTER_BANKS || (atomic_load(&gated) >> bank & 1u); }

/* A call that would access a refused bank returns REFUSED and leaves the owner as it was: OPEN reads both banks,
 * BEGIN, SAMPLE and END one. CLOSE always runs; a refused bank's restore reports IO without an access. */
int os_nsi_counters_call(struct a733_nsi_counter_call *call)
{
    if (!take()) { call->status = A733_NSI_C_REFUSED; return A733_NSI_C_REFUSED; }
    int r;
    if (call->op == A733_NSI_OPEN ? atomic_load(&gated) != 0 :
        call->op != A733_NSI_CLOSE && refused_bank(call->bank)) {
        call->status = A733_NSI_C_REFUSED; r = A733_NSI_C_REFUSED;
    } else
        r = a733_nsi_counters_call(&owner, &guarded_ops, 0, call);
    give();
    return r;
}
/* Before bank's master domain turns off, while it is still on. With no call in progress, the bank is restored and
 * then refused. A call in progress keeps the owner, so the bank is refused at once and left as the call had it;
 * the wait covers at most the one access that had already passed the gate, which runs with IRQs masked. */
void os_nsi_counters_power_off(unsigned bank)
{
    if (bank >= A733_NSI_COUNTER_BANKS) return;
    if (take()) {
        if (owner.opened && !(atomic_load(&gated) >> bank & 1u))
            (void)a733_nsi_counters_restore(&owner, bank);
        atomic_fetch_or(&gated, 1u << bank);
        give();
        return;
    }
    atomic_fetch_or(&gated, 1u << bank);
    while (atomic_load(&inflight) >> bank & 1u) {}
}
/* After bank's master domain has finished a power-on: its words are back at their hardware reset. */
void os_nsi_counters_power_on(unsigned bank)
{
    if (bank < A733_NSI_COUNTER_BANKS) atomic_fetch_and(&gated, ~(1u << bank));
}
/* A failed session's GPU quiesce (apps/os/gpu.c): restore and close, unless a call is in progress (the board's reset
 * then restores the words). */
void os_nsi_counters_quiesce(void)
{
    if (!take()) return;
    (void)a733_nsi_counters_close(&owner);
    give();
}
