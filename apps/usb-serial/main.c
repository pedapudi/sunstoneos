/* Echo one retained receive buffer without copying its payload. */
#include "usb_serial-app.h"
void usb_serial_main(const struct ss_grant *grant)
{
    (void)grant;struct ss_usb_serial_buffer buffer;
    const struct ss_usb_serial_line line={1000000,8,0,0,0};
    for(;;){int r=ss_usb_serial_configure(1,&line);if(r<0)ss_fail(5);if(r==1)break;ss_progress();ss_usb_wait(20000);}
    for(;;){int r=ss_usb_serial_config_result(1);if(r<0)ss_fail(6);if(r==1)break;ss_progress();ss_usb_wait(20000);}
    for(;;){int r=ss_usb_serial_acquire(&buffer);if(r<0)ss_fail(1);if(r==1)break;ss_progress();ss_usb_wait(20000);}
    uint64_t sequence=1;
    for(;;){int r=ss_usb_serial_write(sequence,buffer.data,buffer.bytes);if(r<0)ss_fail(2);if(r==1)break;ss_progress();ss_usb_wait(20000);}
    size_t actual;
    for(;;){int r=ss_usb_serial_write_result(sequence,&actual);if(r<0)ss_fail(3);if(r==1)break;ss_progress();ss_usb_wait(20000);}
    if(ss_usb_serial_release(buffer.token)!=1)ss_fail(4);
    uint64_t result[2]={buffer.bytes,actual};ss_finish(result,sizeof(result));
}
