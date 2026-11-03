/* The services that the CPU clock owner reaches once it works: clock and rail leases, the PMIC, the counter, the thermal
 * policy and the provider table. checks/os-cpu-clock-native.py runs an ungranted session through the real owner, which
 * must reach none of them, so each one stops the machine (BRK). */
#include <stdint.h>
#define TRAP(name) void name(void){__asm__ volatile("brk #0");for(;;){}}
TRAP(a733_clock_acquire) TRAP(a733_clock_release) TRAP(a733_clock_retain) TRAP(a733_counter_clock_now)
TRAP(a733_pmic_decode) TRAP(a733_pmic_reap) TRAP(a733_pmic_result) TRAP(a733_pmic_submit) TRAP(a733_pmic_submit_write)
TRAP(a733_rail_acquire) TRAP(a733_rail_floor) TRAP(a733_rail_poll) TRAP(a733_rail_release) TRAP(a733_rail_retain)
TRAP(a733_thermal_policy_level) TRAP(a733_thermal_policy_seen)
void board_fail_note(uint32_t site,uint32_t value,const void *caller){(void)site;(void)value;(void)caller;__asm__ volatile("brk #0");}
char providers[262144] __attribute__((aligned(64)));
