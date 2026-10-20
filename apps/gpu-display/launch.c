/* The host validates this fixed, pointer-free ABI against the actual image.
 * Full session hashing and provider checks remain in native EL3 admission. */
#include "usb-authority.h"
#define a733_smp_launch_prepare gpu_display_base_prepare
#include "gpu-display-launch-base.c"
#undef a733_smp_launch_prepare
int a733_smp_launch_prepare(void)
{
    volatile const uint64_t *s=(const void *)UINT32_C(0x41f00000);
    volatile const uint64_t *hdmi=(const void *)UINT32_C(0x41f00018);
    volatile const struct gpu_display_usb_authority *usb=(const void *)UINT32_C(0x41f002c0);
    volatile const uint32_t *input=(const void *)UINT32_C(0x41f00348);
    ADMIT(s[0]==UINT64_C(0x4153373347444131)&&s[1]==2&&
        s[2]>=984&&s[2]<=1048576&&s[87]&&s[87]==CONTRACT->reserved[1],14);
    ADMIT(hdmi[0]==UINT64_C(0x4153373348444131)&&hdmi[1]==1&&hdmi[2]==544&&
        hdmi[3]==REQUEST[3]&&hdmi[4]==REQUEST[4]&&hdmi[5]==REQUEST[6]&&hdmi[26],14);
    ADMIT(*(volatile const uint64_t *)input==UINT64_C(0x4153373347444931)&&
        input[2]==1&&input[3]==s[2]-840,14);
    ADMIT(usb->owner&&usb->handoff&&usb->owner!=hdmi[27]&&usb->owner!=hdmi[38]&&
        (usb->owner<9||usb->owner>11)&&usb->cold_reference==2&&!usb->reserved&&
        usb->supply_uv[0]==3300000&&usb->supply_uv[1]==3300000&&
        usb->supply_uv[2]==1200000&&usb->supply_uv[3]==900000,14);
    volatile const uint8_t *entry=(const void *)((uintptr_t)CONTRACT+256);
    volatile const uint8_t *evidence=(const void *)UINT32_C(0x41f00068);
    for(unsigned i=0;i<32;++i)ADMIT(entry[i]==evidence[i],14);
    return gpu_display_base_prepare();
}
