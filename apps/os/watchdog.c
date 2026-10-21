/* Resident WDT0 renewal. The stage-2 loader armed WDT0 in its four-second
 * mode with no ROM redirect and no RTC FEL flag. The shared reload in
 * platform/a733/maintenance_watchdog.c requires the FEL-launched redirect,
 * so it refuses this state; this unit checks the resident state instead and
 * keeps the same ordering: register mutex, armed-state readback, one CTRL
 * restart write, readback, and mutex retention on an uncertain result. */
#include "os.h"
#include "../board/application.h"
#include "a733_maintenance_watchdog_internal.h"
#include "a733_counter_clock.h"
#include "aarch64_device_memory.h"
static int mappings(void)
{
    static const uintptr_t addresses[]={0x0205000c,0x02050010,0x02050014,0x02050018,
        0x03000024,0x070501c0,0x070501c4,0x070501d4,0x07090108};
    for(unsigned i=0;i<sizeof(addresses)/sizeof(addresses[0]);++i){
        struct aarch64_translation t;
        if(!aarch64_device_translate(addresses[i],i==0,&t)||t.physical!=addresses[i]||t.attributes!=0)return 0;
    }
    return 1;
}
static uint32_t read32(uintptr_t a)
{
    __asm__ volatile("dsb sy":::"memory");
    uint32_t value=*(volatile const uint32_t *)a;
    __asm__ volatile("dsb sy":::"memory");return value;
}
/* Armed four-second mode, reset output, no ROM redirect or FEL flag. */
static int armed(void)
{
    return (read32(0x02050014)&0xffff)==0x41&&(read32(0x02050010)&0xffff)==1&&
        (read32(0x02050018)&0xffff)==2&&!(read32(0x03000024)&0x100)&&
        !read32(0x070501c0)&&!read32(0x070501c4)&&!read32(0x070501d4)&&!read32(0x07090108);
}
int os_watchdog_reload(void *supervisor)
{
    uint64_t now;
    /* The supervisor task holds no critical section; every wait is bounded. */
    if(!a733_maintenance_hw_task(supervisor)||!mappings()||
       a733_counter_clock_now(&providers.shared.counter,&now)!=1||
       a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,now+100000)!=1)return 0;
    if(!armed()){a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return 0;}
    __asm__ volatile("dsb sy":::"memory");
    *(volatile uint32_t *)(uintptr_t)0x0205000c=0x14af;
    __asm__ volatile("dsb sy":::"memory");
    /* CTRL has no restart acknowledgment; readback only rejects a changed mode. */
    if(!armed()){(void)a733_platform_lock_retain(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return 0;}
    a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);
    return 1;
}
