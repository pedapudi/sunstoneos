/* Only the ISP0 image's selected cold USB profile; no ROM takeover cases. */
#define main previous_usb_platform_cases
#include "a733-usb-platform.c"
#undef main
int main(void)
{
 struct fixture f;init(&f,A733_USB_COLD_REFERENCE);
 /* Union of unchanged USB and VI/IOMMU required global gates. */
 *reg(&f,C+0x5c0)=0x110103ff;*reg(&f,C+0x5e0)=0xc1055803;
 assert(run(&f)==A733_USB_PLATFORM_PREPARED);
 assert((*reg(&f,C+0x1340)&0x87000000)==0x81000000);
 assert((*reg(&f,C+0x1300)&0xc0000000)==0x40000000);
 assert((*reg(&f,C+0x1304)&0x01110111)==0x01000100);
 assert(*reg(&f,C+0x5c0)==0x110103ff&&*reg(&f,C+0x5e0)==0xc1055803);
 assert(!f.retains&&!f.locked);puts("ISP0 cold USB mode1 selected profile passed");return 0;
}
