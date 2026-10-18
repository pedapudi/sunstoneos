/* Only the newly assigned bus parent and atomic GPADC parent handoff. */
#include "a733_clock_leases.h"
#include <assert.h>
#include <stdio.h>
static int now(void *v,uint64_t *n){(void)v;*n=1;return 1;}
int main(void)
{
 struct a733_clock_leases m={0};struct a733_clock_token a={0},b={0},change={0},next={0};
 assert(a733_clock_leases_init(&m,now,NULL));
 assert(a733_clock_acquire(&m,1,A733_CLOCK_APBS0,0,100,&a)==A733_CLOCK_OK);
 assert(a.shared==(A733_CLOCK_APBS0|A733_CLOCK_REF|A733_CLOCK_OSC));
 assert(a733_clock_acquire(&m,2,A733_CLOCK_APBS0|A733_CLOCK_APBS1,0,100,&b)==A733_CLOCK_OK);
 assert(a733_clock_acquire(&m,3,0,A733_CLOCK_APBS0,100,&change)==A733_CLOCK_BUSY);
 assert(a733_clock_acquire(&m,3,0,A733_CLOCK_REF,100,&change)==A733_CLOCK_BUSY);
 assert(a733_clock_release(&m,&a,100)==A733_CLOCK_OK);
 assert(a733_clock_release(&m,&b,100)==A733_CLOCK_OK);
 assert(a733_clock_acquire(&m,3,0,A733_CLOCK_APBS0,100,&change)==A733_CLOCK_OK);
 assert(a733_clock_acquire(&m,4,A733_CLOCK_APBS0,0,100,&a)==A733_CLOCK_BUSY);
 assert(a733_clock_release(&m,&change,100)==A733_CLOCK_OK);
 assert(a733_clock_acquire(&m,3,0,A733_CLOCK_GPADC24M,100,&change)==A733_CLOCK_OK);
 assert(a733_clock_publish_stable(&m,&change,A733_CLOCK_GPADC24M,100,&next)==A733_CLOCK_OK);
 assert(a733_clock_token_status(&m,&change)==A733_CLOCK_INVALID);
 assert(next.shared==(A733_CLOCK_GPADC24M|A733_CLOCK_REF|A733_CLOCK_OSC)&&!next.exclusive);
 assert(a733_clock_acquire(&m,4,0,A733_CLOCK_THS,100,&a)==A733_CLOCK_OK);
 assert(a733_clock_acquire(&m,5,0,A733_CLOCK_GPADC,100,&b)==A733_CLOCK_OK);
 assert(a733_clock_acquire(&m,6,0,A733_CLOCK_GPADC24M,100,&change)==A733_CLOCK_BUSY);
 assert(a733_clock_acquire(&m,7,0,A733_CLOCK_ALL+1,100,&change)==A733_CLOCK_INVALID);
 puts("PASS: APBS0 shared parent stability/conflicts and atomic GPADC24M publication");
}
