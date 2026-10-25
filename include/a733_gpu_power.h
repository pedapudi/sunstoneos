#ifndef SUNSTONE_A733_GPU_POWER_H
#define SUNSTONE_A733_GPU_POWER_H
#include <stdint.h>
#include "a733_gpu_spin.h"
#include "a733_power_domain_row.h"
#include "a733_gpu_opp.h"

/* The PERI0 400 MHz output that the resource manager holds as a shared
 * parent lease. The GPU core runs from PLL_GPU0 at A733_GPU_CORE_HZ
 * (a733_gpu_opp.h). The firmware configuration of the GPU service still
 * states this rate until firmware start adopts A733_GPU_CORE_HZ. */
#define A733_GPU_INITIAL_HZ 400000000u
/* PWCR reset value of a PPU with one device Q-Channel (DEN0051E Table 5-10):
 * DEVQACTIVE 0 and the channel's handshake enabled. CORE6 reads this with
 * policy 8 and status 0 after power-up, before any software write. */
#define A733_GPU_CORE_RESET_PWCR UINT32_C(0x101)
/* Diagnostic CORE6 words, in step order: identification, the device and
 * PCSM accept/deny inputs before any write, CORE status after the PWCR write
 * and after the policy write, and the inputs again before the final status
 * poll. */
enum a733_gpu_core_note { A733_GPU_NOTE_IDR0, A733_GPU_NOTE_MISR,
    A733_GPU_NOTE_AFTER_PWCR, A733_GPU_NOTE_AFTER_POLICY,
    A733_GPU_NOTE_MISR_LATE, A733_GPU_CORE_NOTES };

/* GPU_TOP (PCK domain 5) remains behind the shared owner's row. CORE6 is
 * private to the GPU controller under its exclusive LOCAL lease. */
enum a733_gpu_power_bank { A733_GPU_CCU, A733_GPU_R_CCU, A733_GPU_CORE };
enum a733_gpu_power_phase { A733_GPU_POWER_NEW, A733_GPU_POWER_STARTING,
    A733_GPU_POWER_ON, A733_GPU_POWER_STOPPING, A733_GPU_POWER_OFF,
    A733_GPU_POWER_FAILED };
enum a733_gpu_power_result { A733_GPU_POWER_OK, A733_GPU_POWER_AGAIN,
    A733_GPU_POWER_INVALID, A733_GPU_POWER_FAULT };
/* PLL: a PLL_GPU0 write did not read back. LOCK: PLL_GPU0 did not lock within
 * 100 reads. UPDATE: a GPU clock update bit did not clear within 100 reads. */
enum a733_gpu_power_error { A733_GPU_POWER_NO_ERROR, A733_GPU_POWER_IO,
    A733_GPU_POWER_CLOCK, A733_GPU_POWER_TIMEOUT, A733_GPU_POWER_RESOURCES,
    A733_GPU_POWER_STATE, A733_GPU_POWER_CANCELLED, A733_GPU_POWER_DOMAIN,
    A733_GPU_POWER_PLL, A733_GPU_POWER_LOCK, A733_GPU_POWER_UPDATE };

struct a733_gpu_power_resources { uint32_t parent_hz, supply_uv; };
/* One privileged owner serializes this controller. All callbacks are bounded,
 * non-reentrant and return 1 for success. A failed callback may have side effects.
 * now uses monotonic microseconds without wrap during an operation; it may
 * round down to whole microseconds.
 * acquire owns the GPU-local clock, reset, PLL_GPU0 and GPU_TOP/CORE domains
 * exclusively. It also holds shared leases for stable pll-peri0-400m, DCDC4 at
 * A733_GPU_SUPPLY_UV, dcxo, the PCK clock, and GPU AHB/MBUS gates. Return the
 * established parent rate and rail setting.
 * These resources remain stable until release; failures retain any partial lease.
 * Neither callback changes GPU-local clock/reset or GPU power policies. The manager
 * must arbitrate shared clocks, rail changes and other devices' power requests.
 * read performs an ordered 32-bit read of CCU 0x02002000, R_CCU 0x07010000 or
 * CORE PPU 0x07066000 (policy/status/MISR/PWCR/IDR0 at 0/8/0x14/0x20/0xfb0
 * only; MISR and IDR0 are read-only diagnostics);
 * the GPU_TOP domain (PCK domain 5) belongs to the power-domain owner and is
 * reached only through the bound row. update atomically applies
 * (old & ~clear) | set under the platform register lock, including any write
 * key, preserving unrelated fields. update completes before returning; read
 * acquires preceding device writes. CCU updates preserve unrelated fields;
 * exclusive CORE policy/PWCR writes use clear=UINT32_MAX and only values 0/8.
 * release drops only this controller's shared-resource leases after power off.
 * It must preserve resources still used by other devices and the board
 * always-on DCDC4 rail.
 */
struct a733_gpu_power_ops {
    int (*now)(void *, uint64_t *);
    int (*acquire)(void *, struct a733_gpu_power_resources *);
    int (*release)(void *);
    int (*read)(void *, enum a733_gpu_power_bank, uint32_t offset, uint32_t *);
    int (*update)(void *, enum a733_gpu_power_bank, uint32_t offset,
                  uint32_t clear, uint32_t set);
};
struct a733_gpu_power {
    struct a733_gpu_power_ops ops;
    void *context;
    uint64_t deadline, last_now;
    enum a733_gpu_power_phase phase;
    enum a733_gpu_power_error error;
    uint32_t index, last_value, lease_attempted, lease_held, requested;
    /* Entry observations for retained diagnostics, not release authority.
     * seen bits0/1/2 identify successful policy/status/PWCR reads, and bit
     * 3 + n a successful read of core_note[n] (enum a733_gpu_core_note).
     * core_power_up is set at admission when the entry words are the
     * power-up state. */
    uint32_t core_policy, core_status, core_config, core_seen;
    uint32_t core_note[A733_GPU_CORE_NOTES], core_power_up;
    /* PLL_GPU0 and GPU clock observations of the last ON or OFF: the reads of
     * each lock wait (enable, then 1008 MHz), the reads until each GPU clock
     * update bit cleared, and the last PLL_GPU0 word read. */
    uint32_t pll_lock_reads[2], clock_update_reads[2], pll_word;
    /* GPU_TOP: registered before the scheduler on the owner bound to the
     * PMIC provider, by a733_gpu_platform_bind_pmic in the images. */
    struct a733_power_domain_row row;
};
/* Zero storage before init. No I/O; refusal preserves state. Ops storage and
 * state must not overlap. Call only before first use. Callback context stays live.
 */
int a733_gpu_power_init(struct a733_gpu_power *, const struct a733_gpu_power_ops *, void *);
/* Start from NEW/OFF. Only a clock read occurs here. A row must be bound. All physical use requires
 * qualified independent watchdog recovery and exclusive device ownership.
 * Initial GPU core/bus clocks must be disabled and external reset asserted;
 * a live inherited GPU is refused before GPU-local or PCK writes.
 */
enum a733_gpu_power_result a733_gpu_power_on(struct a733_gpu_power *, uint64_t deadline);
/* Start from ON after firmware shutdown, closed job admission, and interrupt
 * masking. The caller must retain all device backing. OFF reports clock/reset
 * and power-domain state only: it does not prove memory transactions drained.
 * This controller MUST NOT implement pvr_gpu_stop_ops.isolate by itself.
 */
enum a733_gpu_power_result a733_gpu_power_off(struct a733_gpu_power *, uint64_t deadline);
/* Runs the resource and register steps back to back, with clock reads, in
 * the vendor's order (docs/designs/gpu-vendor-default.md). Power-on checks the
 * MBUS gates, the PPU clock, a closed GPU clock gate on PLL_GPU0 at divider
 * 1, an asserted reset and a disabled PLL_GPU0, then requests the row ON and
 * returns AGAIN until the service answers. It admits settled static CORE
 * policy/status, or the power-up state of policy 8, status 0 and
 * A733_GPU_CORE_RESET_PWCR, and writes PWCR 0. It enables PLL_GPU0, releases
 * the GPU reset, opens the bus and core clock gates, sets PLL_GPU0 to
 * A733_GPU_CORE_HZ, writes CORE policy 8 and polls CORE status until 8.
 * Every PLL_GPU0 write is read back (error PLL); each lock wait reads at most
 * 100 times 3 us apart (error LOCK) and each GPU clock update bit must clear
 * within 100 reads (error UPDATE); these waits run inside one poll. Status
 * observations after the PWCR and policy writes require 0 or 8.
 * Power-off commands and checks CORE OFF while the GPU is still clocked, then
 * closes the bus and core gates, disables PLL_GPU0, asserts reset and requests
 * the TOP row OFF. A refused row fails with STATE, a failed domain with
 * DOMAIN. A wait whose clock stops advancing for A733_GPU_SPIN_STALLED_READS
 * samples returns AGAIN without advancing the step. Every wait ends at the
 * absolute deadline. Failure retains diagnostics and disables further I/O; no
 * automatic rollback. lease_held records completed acquire/release
 * callbacks. Failed callbacks can leave partial ownership; a late successful
 * release leaves lease_held clear. ON establishes the configured
 * A733_GPU_CORE_HZ, not a frequency measurement.
 */
enum a733_gpu_power_result a733_gpu_power_poll(struct a733_gpu_power *);
enum a733_gpu_power_result a733_gpu_power_cancel(struct a733_gpu_power *);
#endif
