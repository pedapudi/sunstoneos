#include "session.h"
#include "a733_storage_update_native.h"
#include "a733_range.h"
#include <string.h>
static int present(const uint8_t *p,size_t n)
{unsigned v=0;for(size_t i=0;i<n;++i)v|=p[i];return v!=0;}
static int zero(const uint8_t *p,size_t n){return !present(p,n);}
static struct a733_pin_config pin(const uint8_t *p)
{return (struct a733_pin_config){p[0],p[1],p[2]};}
int a733_storage_session_decode(struct a733_storage_native_boot *out,
    struct a733_maintenance_transfer *handoff,const uint8_t raw[A733_STORAGE_SESSION_BYTES],uint64_t ticks)
{
    if(!out||!handoff||!raw)return 0;
    struct a733_storage_native_boot b={0};struct a733_maintenance_transfer t={0};
    if(a733_storage_wire64(raw)!=A733_STORAGE_SESSION_MAGIC ||
       a733_storage_wire64(raw+8)!=A733_STORAGE_SESSION_VERSION ||
       a733_storage_wire64(raw+16)!=A733_STORAGE_SESSION_BYTES ||
       !zero(raw+156,4)||!zero(raw+460,28)||!zero(raw+638,2)||!zero(raw+656,48) ||
       !a733_storage_grant_decode(&b.grant,raw+A733_STORAGE_GRANT_OFFSET,256))return 0;
    t.magic=A733_MAINTENANCE_TRANSFER_MAGIC;t.epoch=a733_storage_wire64(raw+24);
    t.generation=a733_storage_wire64(raw+32);t.lease=a733_storage_wire64(raw+40);
    memcpy(t.image,raw+48,32);memcpy(t.qualification,raw+80,32);
    t.minimum_reload_us=a733_storage_wire64(raw+112);t.maximum_recovery_us=a733_storage_wire64(raw+120);
    t.guard_us=a733_storage_wire64(raw+128);t.reset_by_us=a733_storage_wire64(raw+136);
    t.rtc_control=a733_storage_wire32(raw+144);t.rtc_divider=a733_storage_wire32(raw+148);
    t.rtc_source=a733_storage_wire32(raw+152);
    uint64_t before=a733_storage_wire64(raw+640),after=a733_storage_wire64(raw+648);
    if(!t.epoch||!t.generation||!t.lease||!present(t.image,32)||!present(t.qualification,32)||
       memcmp(t.qualification,b.grant.recovery,32)||after<before||after-before>24000||ticks<after||
       !t.guard_us||t.guard_us>UINT64_MAX-A733_MAINTENANCE_OPERATION_US||
       t.minimum_reload_us<=t.guard_us+A733_MAINTENANCE_OPERATION_US||
       t.maximum_recovery_us<t.minimum_reload_us||t.reset_by_us<=t.maximum_recovery_us||
       t.reset_by_us>UINT64_MAX/24||b.grant.deadline_us>t.reset_by_us-t.maximum_recovery_us||
       ticks>=b.grant.deadline_us*24)return 0;
    t.reload_before_us=before/24;t.reload_after_us=after/24;
    uint64_t window=t.minimum_reload_us-t.guard_us-A733_MAINTENANCE_OPERATION_US;
    uint64_t startup;
    if(!a733_deadline_after(t.reload_before_us,window,&startup))return 0;
    if(startup>b.grant.deadline_us)startup=b.grant.deadline_us;
    if(!startup||ticks/24>=startup)return 0;
    b.identity=t.generation;b.topology=a733_storage_wire64(raw+416);
    memcpy(b.topology_artifact,raw+424,32);memcpy(b.launch_artifact,t.image,32);
    if(!b.topology||!present(b.topology_artifact,32))return 0;
    for(unsigned i=0;i<4;++i)b.usb_supply_uv[i]=a733_storage_wire32(raw+488+i*4);
    b.mmc.owner=a733_storage_wire64(raw+504);b.pmic.owner=a733_storage_wire64(raw+512);
    b.usb.owner=a733_storage_wire64(raw+520);
    b.mmc.handoff=b.pmic.identity=b.usb.handoff=b.identity;b.mmc.topology=b.topology;
    b.mmc.deadline_us=b.pmic.startup_deadline=b.usb.deadline_us=startup;
    b.mmc.lifetime_deadline_us=b.grant.deadline_us;
    b.mmc.kind=(enum sunxi_mmc_kind)b.grant.kind;b.mmc.emmc_host=(enum a733_mmc_emmc_host)b.grant.host;
    /* Profile zero stays the default; the platform also refuses a profile
     * for eMMC and a pin route that does not match the profile. */
    uint32_t profile=a733_storage_wire32(raw+456);
    if(profile>SUNXI_MMC_SD_HIGH_SPEED4||(profile&&b.grant.kind!=SUNXI_MMC_SD))return 0;
    b.mmc.sd_profile=(enum sunxi_mmc_sd_profile)profile;
    for(unsigned i=0;i<3;++i) {
        b.mmc.fabric[i]=b.usb.fabric[i]=a733_storage_wire32(raw+528+i*4);
        b.mmc.fabric_hz[i]=b.usb.rate_hz[i]=a733_storage_wire32(raw+540+i*4);
    }
    b.mmc.voltage_mask=a733_storage_wire32(raw+552);b.usb.cold_reference=a733_storage_wire32(raw+556);
    for(unsigned i=0;i<12;++i){b.mmc.expected[i]=pin(raw+560+3*i);b.mmc.desired[i]=pin(raw+596+3*i);}
    for(unsigned i=0;i<2;++i)b.pmic.pins[i]=pin(raw+632+3*i);
    b.pins.authorized[A733_PIN_L]=3;
    if(b.grant.kind==SUNXI_MMC_SD)b.pins.authorized[A733_PIN_F]=0x7f;
    else if(b.grant.kind==SUNXI_MMC_EMMC)b.pins.authorized[A733_PIN_C]=0x1ef63;
    else return 0;
    /* Native provider constructors validate exact routes, fields and supplies.
     * The copied grant is the only card/range/write authority. */
    *out=b;t.storage=&out->grant;*handoff=t;return 1;
}
