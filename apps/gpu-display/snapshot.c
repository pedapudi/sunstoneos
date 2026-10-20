#include "session.h"
#include "a733_cpu_coherence.h"
#include "a733_storage_digest.h"
#include <string.h>
/* This private copy is cleared at image entry, then published before secondary
 * startup. The uploaded envelope is outside BSS and never used by providers. */
static struct {
    _Alignas(64) uint8_t bytes[GPU_DISPLAY_ENVELOPE_LIMIT];
    uint8_t digest[32];
    unsigned attempted,ready;
} retained;
int gpu_display_session_prepare(const struct a733_smp_contract *c)
{
    if(!c||retained.attempted)return 0;
    retained.attempted=1;
    const struct gpu_display_boot_contract *record=gpu_display_boot_record(c);
    if(!record || !a733_smp_contract_valid(c,a733_smp_counter()))return 0;
    const volatile struct gpu_display_session *source=(const void *)A733_STORAGE_SESSION_ADDRESS;
    uint64_t bytes=source->bytes;
    if(bytes<sizeof(*source)||bytes>GPU_DISPLAY_ENVELOPE_LIMIT)return 0;
    for(uint64_t i=0;i<bytes;++i)retained.bytes[i]=((const volatile uint8_t *)source)[i];
    const struct gpu_display_session *copy=(const void *)retained.bytes;
    uint8_t digest[32];storage_digest_bytes(copy,bytes,digest);
    if(copy->bytes!=bytes || memcmp(digest,record->session_digest,32) ||
       !gpu_display_session_valid(copy))return 0;
    memcpy(retained.digest,digest,sizeof(retained.digest));
    retained.ready=1;
    if(a733_cpu_coherence_publish((uintptr_t)&retained,sizeof(retained),c->startup_deadline)==1)return 1;
    retained.ready=0;return 0;
}
const struct hdmi_session *gpu_display_hdmi_get(void)
{return retained.ready?&((const struct gpu_display_session *)(const void *)retained.bytes)->hdmi:NULL;}
const struct gpu_display_inputs *gpu_display_inputs_get(void)
{return retained.ready?&((const struct gpu_display_session *)(const void *)retained.bytes)->inputs:NULL;}
const struct gpu_display_usb_authority *gpu_display_usb_get(void)
{return retained.ready?&((const struct gpu_display_session *)(const void *)retained.bytes)->usb:NULL;}
const uint8_t *gpu_display_session_digest_get(void)
{return retained.ready?retained.digest:NULL;}
