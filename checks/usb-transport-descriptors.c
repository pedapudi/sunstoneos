/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the adapter's actual descriptor admission, without a controller. */
#include "../drivers/usb/a733_xhci_tinyusb.c"
#include <assert.h>
#include <stdio.h>
/* Any lifetime failure in these metadata-only cases is unexpected. */
void a733_xhci_lost(struct a733_xhci *x){(void)x;assert(0);}
void a733_usb2_platform_lost(struct a733_usb2_platform *p){(void)p;assert(0);}
static const uint8_t receiver[]={
    9,2,59,0,2,1,0,0xa0,50,
    9,4,0,0,1,3,1,1,0,
    9,0x21,0x11,1,0,1,0x22,63,0,
    7,5,0x81,3,8,0,10,
    9,4,1,0,1,3,1,2,0,
    9,0x21,0x11,1,0,1,0x22,52,0,
    7,5,0x82,3,8,0,10
};
static void address_reuse(void)
{
    static struct a733_usb2_native native;
    host=&native;host->generic_host=1;
    /* A completed application lease keeps the disabled child's record while
     * TinyUSB reuses its address for a different record. Test either order. */
    for(unsigned old=1;old<=2;++old){
        unsigned active=3-old;
        memset(host->core.devices,0,sizeof(host->core.devices));
        memset(host->descriptors,0,sizeof(host->descriptors));
        host->core.devices[old].address=2;host->core.devices[old].detach=3;
        host->core.devices[active].address=2;host->core.devices[active].port_ready=3;
        host->descriptors[active].generic.ready=1;
        assert(descriptor_index(2)==active);
        assert(generic_at(2)==&host->descriptors[active].generic);
        host->core.devices[active].detach=1;
        assert(descriptor_index(2)==A733_XHCI_DEVICES&&!generic_at(2));
    }
    host=NULL;
}
static void report_descriptor_length(void)
{
    static struct a733_usb2_native native;
    uint8_t report[44]={0};report[43]=0xc0; /* A stale END_COLLECTION. */
    host=&native;host->control_address=2;host->descriptors[0].generic.ready=1;
    const uint8_t setup[8]={0x81,6,0,0x22,0,0,sizeof(report),0};
    memcpy(host->control_setup,setup,sizeof(setup));
    struct a733_xhci_notice event={.address=2,.endpoint=128,.bytes=43};
    for(unsigned bytes=0;bytes<sizeof(report);++bytes){
        host->control_buffer=report;host->control_bytes=sizeof(report);event.bytes=bytes;
        assert(!control_response(&event));
        assert(host->control_buffer==report&&host->control_bytes==sizeof(report));
    }
    event.bytes=sizeof(report);assert(control_response(&event));
    assert(!host->control_buffer&&!host->control_bytes);
    /* UVC/bridge class responses retain their actual short-transfer contract. */
    host->control_setup[0]=0xa1;host->control_setup[1]=0x81;
    host->control_buffer=report;host->control_bytes=sizeof(report);event.bytes=43;
    assert(control_response(&event));
    host=NULL;
}
int main(void)
{
    address_reuse();
    report_descriptor_length();
    struct a733_usb2_generic g;
    assert(generic_configuration(&g,receiver,sizeof(receiver))==1);
    assert(g.ready&&g.interfaces==3&&g.endpoints==2&&g.reports[0]==63&&g.reports[1]==52);
    uint8_t bad[sizeof(receiver)];memcpy(bad,receiver,sizeof(bad));
    bad[54]=0x81;assert(!generic_configuration(&g,bad,sizeof(bad)));
    memcpy(bad,receiver,sizeof(bad));bad[9]=0;assert(!generic_configuration(&g,bad,sizeof(bad)));
    memcpy(bad,receiver,sizeof(bad));bad[38]=0;assert(!generic_configuration(&g,bad,sizeof(bad)));
    memcpy(bad,receiver,sizeof(bad));bad[4]=3;assert(!generic_configuration(&g,bad,sizeof(bad)));
    assert(!generic_configuration(&g,receiver,sizeof(receiver)-1));
    puts("USB transport descriptor admission passed");
    return 0;
}
