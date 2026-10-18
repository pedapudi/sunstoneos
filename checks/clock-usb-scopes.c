#include "a733_clock_leases.h"
#include <assert.h>
#include <stdio.h>

static int now(void *cookie,uint64_t *out)
{(void)cookie;*out=1;return 1;}

int main(void)
{
    struct a733_clock_leases registry={0};
    struct a733_clock_token pmic,typec,usb0,usb2,camera,out={0};
    assert(a733_clock_leases_init(&registry,now,NULL));
    /* PMIC, Type-C, camera and the existing FEL-facing port can coexist. */
    assert(a733_clock_acquire(&registry,1,0,A733_CLOCK_S_TWI0,10,&pmic)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&registry,2,0,A733_CLOCK_S_TWI1,10,&typec)==A733_CLOCK_OK);
    assert(typec.exclusive==A733_CLOCK_S_TWI1);
    assert(typec.shared==(A733_CLOCK_APBS1|A733_CLOCK_REF|A733_CLOCK_OSC));
    assert(a733_clock_acquire(&registry,3,0,A733_CLOCK_TWI11,10,&camera)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&registry,4,0,A733_CLOCK_USB0,10,&usb0)==A733_CLOCK_OK);
    const a733_clock_mask_t parents=A733_CLOCK_PERI0|A733_CLOCK_FABRIC|
        A733_CLOCK_SERDES_COMMON|A733_CLOCK_USB_PCIE_DOMAIN;
    assert(a733_clock_acquire(&registry,5,parents,A733_CLOCK_USB2,10,&usb2)==A733_CLOCK_OK);
    assert(usb2.exclusive==A733_CLOCK_USB2);
    assert(usb2.shared==(parents|A733_CLOCK_REF|A733_CLOCK_OSC));
    /* Independent local ownership must still exclude retuning shared parents. */
    const a733_clock_mask_t conflicts[]={A733_CLOCK_S_TWI1,A733_CLOCK_USB2,
        A733_CLOCK_APBS1,A733_CLOCK_REF,A733_CLOCK_OSC,A733_CLOCK_PERI0,
        A733_CLOCK_FABRIC,A733_CLOCK_SERDES_COMMON,A733_CLOCK_USB_PCIE_DOMAIN};
    for(unsigned i=0;i<sizeof(conflicts)/sizeof(conflicts[0]);++i) {
        assert(a733_clock_acquire(&registry,6,0,conflicts[i],10,&out)==A733_CLOCK_BUSY);
        assert(!out.id);
    }
    assert(a733_clock_acquire(&registry,6,A733_CLOCK_S_TWI1,0,10,&out)==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&registry,6,A733_CLOCK_USB2,0,10,&out)==A733_CLOCK_INVALID);
    struct a733_clock_token forged=typec;
    forged.exclusive=A733_CLOCK_USB2;
    assert(a733_clock_token_status(&registry,&forged)==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&registry,6,0,(A733_CLOCK_ALL+1),10,&out)==A733_CLOCK_INVALID);
    assert(a733_clock_release(&registry,&usb2,10)==A733_CLOCK_OK);
    assert(a733_clock_release(&registry,&typec,10)==A733_CLOCK_OK);
    assert(a733_clock_token_status(&registry,&pmic)==A733_CLOCK_OK);
    assert(a733_clock_token_status(&registry,&usb0)==A733_CLOCK_OK);
    assert(a733_clock_token_status(&registry,&camera)==A733_CLOCK_OK);
    /* Releasing S_TWI1 does not release the PMIC's shared APBS1 reservation. */
    assert(a733_clock_acquire(&registry,6,0,A733_CLOCK_APBS1,10,&out)==A733_CLOCK_BUSY);
    puts("PASS: independent Type-C and USB2 scopes with shared-parent exclusion");
}
