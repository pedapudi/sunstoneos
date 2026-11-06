/* The OS image's sealed session (docs/designs/one-os-image.md, "Lifetime word and sealed session"). It takes the place
 * of apps/board/session.c in the OS image, with the same interface (apps/board/session.h) and the same checks, and
 * keeps a copy of the lifetime that EL3 admission verified from the contract magic.
 *
 * The OS link script (apps/os/sealed-link.patch) places .bss.os_sealed in a NOLOAD section last in the CODE block,
 * which EL1 and EL0 map read-only. Only EL3 writes it, here, with translation and caches off: those stores are
 * Device-nGnRnE and allocate no cache line. No loader zeroes a NOLOAD section, and DRAM keeps the previous boot's
 * bytes across a watchdog reset, so the section is zeroed before the copy and ready is written last:
 *   1. DC CIVAC over the section, so that no Secure line of it can later be written back over the new bytes;
 *   2. zero, copy and check the session, write the lifetime copy, then ready;
 *   3. DC CIVAC over the section again (DC CVAC, the publication, would leave a clean line valid);
 *   4. every core flushes the Nonsecure alias at EL1 entry with translation off (platform/a733-smp-cached/continue.S).
 * EL3 never reads the lifetime copy back: its own decisions read a733_smp_lifetime in monitor memory. */
#include "session.h"
#include "boot.h"
#include "a733_cpu_coherence.h"
#include "a733_storage_digest.h"
#include <string.h>
#ifndef __aarch64__
/* Host fixture supplies the cache-maintenance boundary. */
int a733_cpu_coherence_flush(uint64_t,uint64_t,uint64_t);
#endif
static struct {
    union { uint8_t bytes[4096];struct a733_board_session prefix; } data;
    unsigned ready,lifetime;
} sealed __attribute__((section(".bss.os_sealed"),aligned(64)));
_Static_assert(sizeof(sealed)%8==0,"the sealed session is zeroed in 64-bit words");
/* EL3 state: a second session in the same boot is refused. In monitor memory, which EL1 does not map, and zeroed with
 * the image's other monitor state. */
static unsigned attempted __attribute__((section(".bss.a733_smp_monitor")));
int board_session_prepare(const struct a733_smp_contract *c)
{
    if(attempted)return 0;
    attempted=1;
    if(!c||c->reserved[0]!=A733_BOARD_SESSION_ADDRESS||
       c->reserved[1]<sizeof(struct a733_board_session)||
       c->reserved[1]>sizeof(sealed.data.bytes))return 0;
    if(a733_cpu_coherence_flush((uintptr_t)&sealed,sizeof(sealed),c->startup_deadline)!=1)return 0;
    /* Aligned 64-bit stores: with translation off the section is Device memory, which takes no unaligned access. */
    volatile uint64_t *word=(volatile uint64_t *)(uintptr_t)&sealed;
    for(size_t i=0;i<sizeof(sealed)/8;++i)word[i]=0;
    const volatile uint8_t *source=(const void *)A733_BOARD_SESSION_ADDRESS;
    for(size_t i=0;i<c->reserved[1];++i)sealed.data.bytes[i]=source[i];
    uint8_t hash[32];storage_digest_bytes(sealed.data.bytes,c->reserved[1],hash);
    const struct a733_board_session *s=&sealed.data.prefix;
    if(memcmp(hash,&c->reserved[2],32)||!a733_board_session_valid(s,a733_smp_counter())||
       s->epoch!=c->epoch||s->generation!=c->generation||s->lease!=c->lease||
       memcmp(s->image,c->build_id,32)||!a733_smp_lifetime)return 0;
    *(volatile unsigned *)&sealed.lifetime=a733_smp_lifetime;
#ifdef __aarch64__
    __asm__ volatile("dsb sy":::"memory");
#endif
    *(volatile unsigned *)&sealed.ready=1;
    if(a733_cpu_coherence_flush((uintptr_t)&sealed,sizeof(sealed),c->startup_deadline)==1)return 1;
    *(volatile unsigned *)&sealed.ready=0;return 0;
}
const struct a733_board_session *board_session_snapshot(void)
{return sealed.ready?&sealed.data.prefix:NULL;}
const void *board_session_sealed(void)
{return sealed.ready?sealed.data.bytes:NULL;}
unsigned board_session_lifetime(void)
{return sealed.ready?sealed.lifetime:0;}
