#ifndef SUNSTONE_A733_SMP_BOOT_H
#define SUNSTONE_A733_SMP_BOOT_H
#include <stdint.h>
#ifndef A733_SMP_CONTRACT_MAGIC
#define A733_SMP_CONTRACT_MAGIC UINT64_C(0x4153373343414331)
#endif
#ifndef A733_SMP_RESULT_MAGIC
#define A733_SMP_RESULT_MAGIC UINT64_C(0x4153373343525331)
#endif
#define A733_SMP_COUNTER_HZ UINT64_C(24000000)
#define A733_SMP_CONTRACT_ADDRESS UINT64_C(0x42004000)
#define A733_SMP_RESULT_ADDRESS UINT64_C(0x42000000)
/* Host publishes magic last, after the immutable contract and image. The image
 * copies this record before mutation; no field authorizes arbitrary MMIO/code.
 * Proof of watchdog/FEL qualification and exclusive ownership stays external. */
struct a733_smp_contract {
    uint64_t magic, version, bytes, epoch, generation, inverse_generation;
    uint8_t build_id[32];
    uint64_t armed_at, startup_deadline, diagnostic_deadline, expires_at;
    uint64_t lease, watchdog_mode, watchdog_configuration, watchdog_output;
    uint64_t fel_flag, redirect_flag, redirect_entry, alternate_flag, reserved[6];
};
struct a733_smp_result {
    uint64_t magic, epoch, generation, stage, status, detail, start_counter;
    uint64_t completed_mask, end_counter, reserved[7];
};
struct a733_smp_core_result {
    uint64_t task_rounds, background_progress, sgis, failure, ticks, reserved[11];
};
extern struct a733_smp_core_result a733_smp_core_results[8];
extern const unsigned char a733_smp_build_id[32];
int a733_smp_contract_valid(const struct a733_smp_contract *, uint64_t now);
int a733_smp_power_address(unsigned core, uintptr_t address, int write);
uint64_t a733_smp_counter(void);
_Noreturn void a733_smp_stop(unsigned status, uint64_t detail);
void a733_smp_primary_el1(void);
#endif
