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
#include "a733_mapping_check.h"
#include "a733_mmio.h"
static int mappings(void)
{
    static const uintptr_t addresses[]={0x0205000c,0x02050010,0x02050014,0x02050018,
        0x03000024,0x070501c0,0x070501c4,0x070501d4,0x07090108};
    /* CTRL is written; the other registers are only read. */
    static const struct a733_mapping_rule readable={A733_MAPPING_READ,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_ANY}},0,1},
        writable={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_ANY}},0,1};
    for(unsigned i=0;i<sizeof(addresses)/sizeof(addresses[0]);++i)
        if(!a733_mapping_check(aarch64_device_translate,addresses[i],addresses[i],i==0?&writable:&readable))return 0;
    return 1;
}
/* Armed four-second mode, reset output, no ROM redirect or FEL flag. */
static int armed(void)
{
    return (a733_mmio_read32_dsb_sy_both(0x02050014)&0xffff)==0x41&&
        (a733_mmio_read32_dsb_sy_both(0x02050010)&0xffff)==1&&
        (a733_mmio_read32_dsb_sy_both(0x02050018)&0xffff)==2&&
        !(a733_mmio_read32_dsb_sy_both(0x03000024)&0x100)&&
        !a733_mmio_read32_dsb_sy_both(0x070501c0)&&!a733_mmio_read32_dsb_sy_both(0x070501c4)&&
        !a733_mmio_read32_dsb_sy_both(0x070501d4)&&!a733_mmio_read32_dsb_sy_both(0x07090108);
}
int os_watchdog_reload(void *supervisor)
{
    uint64_t now;
    /* The supervisor task holds no critical section; every wait is bounded. */
    if(!a733_maintenance_hw_task(supervisor)||!mappings()||
       a733_counter_clock_now(&providers.shared.counter,&now)!=1||
       a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,now+100000)!=1)return 0;
    if(!armed()){a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return 0;}
    a733_mmio_write32_dsb_sy_both(0x0205000c,0x14af);
    /* CTRL has no restart acknowledgment; readback only rejects a changed mode. */
    if(!armed()){(void)a733_platform_lock_retain(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return 0;}
    a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);
    return 1;
}
