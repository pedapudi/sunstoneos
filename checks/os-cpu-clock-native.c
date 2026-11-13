/* Exercise the OS CPU register adapter, including its actual MMIO accessors.
 * Unicorn supplies register storage; no CPU or PMIC hardware is accessed. */
#include "../apps/os/cpu.c"
/* The session and the application as the adapter modes below set them. */
static struct os_session adapter_session;
static struct ss_manifest adapter_manifest;
const struct os_session *os_session_admitted(void){return &adapter_session;}
int os_granted(unsigned mask){return mask&&(adapter_session.granted&mask)==mask;}
const struct ss_application ss_application={.manifest=&adapter_manifest};
/* The performance service is outside this adapter: a session without perf never reaches it. */
int os_perf_step(struct a733_cpu_clock *c,const struct a733_thermal_policy *p,int burst){(void)c;(void)p;(void)burst;return -1;}
/* The supervised lifetime: the returning units (apps/os/cpu-return*.c) are outside this adapter, and its branches are
 * not taken. */
unsigned board_session_lifetime(void){return A733_LIFETIME_SUPERVISED;}
int os_cpu_return_requested(void){return 0;}
int os_cpu_return_failing(void){return 0;}
/* A readiness publication wakes the application's main task (apps/os/application.c); no application runs here. */
void os_application_wake_main(void){}
uint32_t os_cpu_return_entry_refusal;
int os_cpu_return_entry_guard(const struct a733_cpu_clock *c,const struct a733_thermal_policy *p,uint64_t now,
    int (*read)(void *,uintptr_t,uint32_t *),void *context){(void)c;(void)p;(void)now;(void)read;(void)context;return -1;}
static struct a733_thermal_policy adapter_policy;
uint32_t os_cpu_adapter_value;
int os_cpu_adapter_check(unsigned mode,uintptr_t address,uint32_t value)
{
    if(mode==0)return clock_read(&owner,address,&os_cpu_adapter_value);
    if(mode==1)return clock_write(&owner,address,value);
    if(mode==2)return clock_read(NULL,address,&os_cpu_adapter_value);
    if(mode==3)return clock_write(NULL,address,value);
    if(mode==4)return os_cpu_admit();
    /* Mode 5: one owner step of a session that does not grant the CPU clock. It must return 0 and reach no register. */
    if(mode==5){adapter_session.granted=0;return os_cpu_clock_step(&adapter_policy);}
    /* Mode 6: the boot-point check is needed by a session with the CPU clock record (address 1) and by an A76 application
     * (value 1), and by no other. */
    if(mode==6){adapter_session.granted=address?OS_GRANT_CPU_CLOCK:0;adapter_manifest.cluster=(uint8_t)value;return os_cpu_boot_point_needed();}
    return 0;
}
