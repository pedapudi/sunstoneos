/* Exercise the actual board-bundle CPU write adapter with modeled MMIO. */
#include "../apps/board-bundles-power/cpu-opp-step.c"
int os_cpu_bundle_adapter_check(unsigned mode,uintptr_t address,uint32_t value)
{return mode==1 ? cpu_write(NULL,address,value) : 0;}
