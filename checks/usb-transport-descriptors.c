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
int main(void)
{
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
