#include "session.h"
#include "boot.h"
#include "a733_cpu_coherence.h"
#include "a733_storage_digest.h"
#include <string.h>
#ifndef __aarch64__
/* Host fixture supplies the cache-publication boundary. */
int a733_cpu_coherence_publish(uint64_t,uint64_t,uint64_t);
#endif
/* Private session page: current consumers declare592,824 or856 bytes.
 * Retain the complete authenticated copy; only its board prefix is exported. */
static struct {
    union { uint8_t bytes[4096];struct a733_board_session prefix; } data;
    unsigned attempted,ready;
} sealed;
int board_session_prepare(const struct a733_smp_contract *c)
{
    if(sealed.attempted)return 0;
    sealed.attempted=1;
    if(!c||c->reserved[0]!=A733_BOARD_SESSION_ADDRESS||
       c->reserved[1]<sizeof(struct a733_board_session)||
       c->reserved[1]>sizeof(sealed.data.bytes))return 0;
    const volatile uint8_t *source=(const void *)A733_BOARD_SESSION_ADDRESS;
    for(size_t i=0;i<c->reserved[1];++i)sealed.data.bytes[i]=source[i];
    uint8_t hash[32];storage_digest_bytes(sealed.data.bytes,c->reserved[1],hash);
    const struct a733_board_session *s=&sealed.data.prefix;
    if(memcmp(hash,&c->reserved[2],32)||!a733_board_session_valid(s,a733_smp_counter())||
       s->epoch!=c->epoch||s->generation!=c->generation||s->lease!=c->lease||
       memcmp(s->image,c->build_id,32))return 0;
    sealed.ready=1;
    if(a733_cpu_coherence_publish((uintptr_t)&sealed,sizeof(sealed),c->startup_deadline)==1)return 1;
    sealed.ready=0;return 0;
}
const struct a733_board_session *board_session_snapshot(void)
{return sealed.ready?&sealed.data.prefix:NULL;}
