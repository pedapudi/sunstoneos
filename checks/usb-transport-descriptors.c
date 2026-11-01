/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the adapter's actual descriptor admission, without a controller. */
#include "../drivers/usb/a733_xhci_tinyusb.c"
#include <assert.h>
#include <stdio.h>
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
int main(void)
{
    address_reuse();
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
