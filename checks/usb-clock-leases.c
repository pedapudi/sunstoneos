#include "a733_clock_leases.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int now(void *cookie,uint64_t *out)
{*out=*(const uint64_t *)cookie;return 1;}
static void untouched(struct a733_clock_token *out,const struct a733_clock_token *saved)
{assert(!memcmp(out,saved,sizeof(*out)));}
int main(void)
{
    uint64_t time=1;struct a733_clock_leases r={0};
    assert(a733_clock_leases_init(&r,now,&time));
    struct a733_clock_token usb0,usb1,out={.id=999},saved=out;
    const a733_clock_mask_t parents=A733_CLOCK_USB01_REF|A733_CLOCK_REF|A733_CLOCK_OSC;
    assert(a733_clock_acquire(&r,1,A733_CLOCK_FABRIC,A733_CLOCK_USB0,20,&usb0)==A733_CLOCK_OK);
    assert(usb0.shared==(parents|A733_CLOCK_FABRIC) && usb0.exclusive==A733_CLOCK_USB0);
    assert(a733_clock_acquire(&r,2,A733_CLOCK_FABRIC,A733_CLOCK_USB1,20,&usb1)==A733_CLOCK_OK);
    assert(usb1.shared==(parents|A733_CLOCK_FABRIC) && usb1.exclusive==A733_CLOCK_USB1);
    const a733_clock_mask_t writers[]={A733_CLOCK_USB0,A733_CLOCK_USB1,A733_CLOCK_USB01_REF,
        A733_CLOCK_REF,A733_CLOCK_OSC,A733_CLOCK_FABRIC};
    for(unsigned i=0;i<sizeof(writers)/sizeof(*writers);++i) {
        assert(a733_clock_acquire(&r,3,0,writers[i],20,&out)==A733_CLOCK_BUSY);untouched(&out,&saved);
    }
    for(unsigned i=0;i<2;++i) {
        assert(a733_clock_acquire(&r,3,i ? A733_CLOCK_USB1 : A733_CLOCK_USB0,0,20,&out)==A733_CLOCK_INVALID);
        untouched(&out,&saved);
    }
    assert(a733_clock_release(&r,&usb0,20)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&r,3,0,A733_CLOCK_USB01_REF,20,&out)==A733_CLOCK_BUSY);
    assert(a733_clock_release(&r,&usb1,20)==A733_CLOCK_OK);
    /* The cold provider separately proves hardware stability before metadata
     * publication. One port remains exclusive while the other can join. */
    struct a733_clock_token old,next={.id=999};
    assert(a733_clock_acquire(&r,7,A733_CLOCK_FABRIC|A733_CLOCK_RC16M,
        A733_CLOCK_USB01_REF|A733_CLOCK_USB0|A733_CLOCK_CE,20,&old)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&r,2,0,A733_CLOCK_USB1,20,&out)==A733_CLOCK_BUSY);
    assert(a733_clock_publish_stable(&r,&old,A733_CLOCK_USB01_REF,1,&next)==A733_CLOCK_EXPIRED);
    untouched(&next,&saved);assert(a733_clock_token_status(&r,&old)==A733_CLOCK_OK);
    assert(a733_clock_publish_stable(&r,&old,A733_CLOCK_USB01_REF,20,&old)==A733_CLOCK_INVALID);
    assert(a733_clock_publish_stable(&r,&old,A733_CLOCK_USB0,20,&next)==A733_CLOCK_INVALID);
    untouched(&next,&saved);
    struct a733_clock_token changed=old;changed.exclusive^=A733_CLOCK_USB1;
    assert(a733_clock_publish_stable(&r,&changed,A733_CLOCK_USB01_REF,20,&next)==A733_CLOCK_INVALID);
    untouched(&next,&saved);
    assert(a733_clock_publish_stable(&r,&old,A733_CLOCK_USB01_REF,20,&next)==A733_CLOCK_OK);
    assert(next.id!=old.id && next.owner==old.owner && next.shared==(old.shared|A733_CLOCK_USB01_REF));
    assert(next.exclusive==(A733_CLOCK_USB0|A733_CLOCK_CE));
    assert(a733_clock_token_status(&r,&old)==A733_CLOCK_INVALID);
    assert(a733_clock_release(&r,&old,20)==A733_CLOCK_INVALID);
    assert(a733_clock_token_status(&r,&next)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&r,2,A733_CLOCK_FABRIC,A733_CLOCK_USB1,20,&usb1)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&r,2,0,A733_CLOCK_USB0,20,&out)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&r,2,0,A733_CLOCK_USB01_REF,20,&out)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&r,2,0,A733_CLOCK_CE,20,&out)==A733_CLOCK_BUSY);
    assert(a733_clock_release(&r,&next,20)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&r,2,0,A733_CLOCK_USB01_REF,20,&out)==A733_CLOCK_BUSY);
    assert(a733_clock_release(&r,&usb1,20)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&r,8,0,A733_CLOCK_USB01_REF,20,&out)==A733_CLOCK_OK);
    assert(out.shared==(A733_CLOCK_REF|A733_CLOCK_OSC));
    puts("PASS: USB port separation, stable parent exclusion and atomic reference publication");
}
