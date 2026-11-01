/* Record the first input event delivered by the OS-owned USB host. */
#include "usb_input-app.h"
void usb_input_main(const struct ss_grant *grant)
{
    (void)grant;struct ss_input_event event;
    for(;;){int r=ss_input_event(&event);if(r<0)ss_fail(1);
        if(r==1)ss_finish(&event,sizeof(event));ss_progress();ss_usb_wait(20000);}
}
