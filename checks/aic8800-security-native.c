/* Link-only entry retains the real native dependency closure. Not a board image. */
#include "aic8800_sdio_transport.h"
#include "aic8800_supplicant.h"
void (*volatile security_poll_entry)(struct aic8800_sdio_transport *)=aic8800_transport_poll;
int (*volatile security_init_entry)(struct aic8800_supplicant *,const struct aic8800_supplicant_config *)=aic8800_supplicant_init;
int (*volatile security_start_entry)(struct aic8800_sdio_transport *,struct aic8800_security *,struct aic8800_supplicant *,uint64_t)=aic8800_transport_start_security;
int (*volatile security_retire_entry)(struct aic8800_sdio_transport *,uint64_t)=aic8800_transport_retire_security;
void security_link_entry(void)
{
    (void)security_poll_entry;(void)security_init_entry;(void)security_start_entry;(void)security_retire_entry;
    for(;;)__asm__ volatile("wfe");
}
