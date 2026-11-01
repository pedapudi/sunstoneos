#include "a733_usb_memory.h"
#include "a733_cpu_coherence.h"
#include "a733_cpu_start.h"
#include "a733_device_memory_layout.h"
volatile unsigned fixture_admitted=1,fixture_decode=1;
int os_usb_memory_admitted(void){return fixture_admitted==1;}
int a733_cpu_index(uint64_t mpidr,unsigned *core)
{*core=(unsigned)(mpidr>>8)&7;return 1;}
int a733_dram_decode_check(const struct a733_dram_range *ranges,unsigned count)
{(void)ranges;(void)count;return fixture_decode==1;}
