/* Exercise the OS CPU register adapter, including its actual MMIO accessors.
 * Unicorn supplies register storage; no CPU or PMIC hardware is accessed. */
#include "../apps/os/cpu.c"
uint32_t os_cpu_adapter_value;
int os_cpu_adapter_check(unsigned mode,uintptr_t address,uint32_t value)
{
    if(mode==0)return clock_read(&owner,address,&os_cpu_adapter_value);
    if(mode==1)return clock_write(&owner,address,value);
    if(mode==2)return clock_read(NULL,address,&os_cpu_adapter_value);
    if(mode==3)return clock_write(NULL,address,value);
    if(mode==4)return os_cpu_admit();
    return 0;
}
