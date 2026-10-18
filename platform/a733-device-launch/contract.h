#ifndef SUNSTONE_A733_DEVICE_LAUNCH_CONTRACT_H
#define SUNSTONE_A733_DEVICE_LAUNCH_CONTRACT_H
#include <stdint.h>
#include <stddef.h>
#define A733_SMP_CONTRACT_MAGIC UINT64_C(0x4153373343414331)
#define A733_SMP_COUNTER_HZ UINT64_C(24000000)
#define A733_SMP_CONTRACT_ADDRESS UINT64_C(0x42004000)
/* Independent wire declaration for the version-2 cached image. The six words
 * bind layout, admitted reservation identity, WB start/end and NC start/end.
 * Physical qualification and exclusive ownership are host prerequisites. */
struct a733_smp_contract {
    uint64_t magic, version, bytes, epoch, generation, inverse_generation;
    uint8_t build_id[32];
    uint64_t armed_at, startup_deadline, diagnostic_deadline, expires_at;
    uint64_t lease, watchdog_mode, watchdog_configuration, watchdog_output;
    uint64_t fel_flag, redirect_flag, redirect_entry, alternate_flag, reserved[6];
};
_Static_assert(sizeof(struct a733_smp_contract)==224, "contract wire size");
_Static_assert(offsetof(struct a733_smp_contract,reserved)==176, "layout wire offset");
#endif
