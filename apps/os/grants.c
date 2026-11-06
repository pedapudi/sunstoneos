/* The services a session grants: the wire size of the records it carries and the compositions that admission refuses.
 * Pure functions of the granted set, shared by the EL3 admission, the host pre-flight and checks/os-session-grants.py. */
#include "os.h"
#include <stdatomic.h>
/* The admitted session is sealed for the boot, so one successful validation holds: the pointer and the granted set are
 * kept, and every later call is a load. Before the first success the full validation runs on each call (the session is
 * not sealed yet, so nothing is cached). */
#define GRANTED_VALID UINT32_C(0x80000000)
static _Atomic(const struct os_session *) admitted;
static atomic_uint granted_word;
const struct os_session *os_session_admitted(void)
{
    const struct os_session *s=atomic_load_explicit(&admitted,memory_order_acquire);
    if(s)return s;
    s=os_session_validated();
    if(s){
        atomic_store_explicit(&granted_word,GRANTED_VALID|s->granted,memory_order_relaxed);
        atomic_store_explicit(&admitted,s,memory_order_release);
    }
    return s;
}
int os_granted(unsigned mask)
{
    unsigned word=atomic_load_explicit(&granted_word,memory_order_acquire);
    if(!(word&GRANTED_VALID)){
        if(!os_session_admitted())return 0;
        word=atomic_load_explicit(&granted_word,memory_order_acquire);
    }
    return mask&&(word&mask)==mask;
}
size_t os_session_wire_bytes(unsigned granted)
{
    /* The board prefix with the thermal record, and each declared record the session grants (apps/os/os-services.h). */
#define RECORD_BYTES(GRANT,extent,field,bytes,magic,version,need) +((granted&OS_GRANT_##GRANT)?(size_t)(bytes):0)
    return A733_OS_SESSION_BYTES OS_RECORD_ROWS(RECORD_BYTES);
#undef RECORD_BYTES
}
/* The cores of an application's workers. They fill the main task's cluster first, its pair before any other core. Cores 0
 * and 1 carry the shared interrupts, the PMIC task and the NPU interrupt, so they come last: a manifest takes them only by
 * asking for more workers than the other cores hold. An A55 manifest never reaches the A76 cores. A manifest that asks for
 * more workers than its order holds is refused, not placed on cores it did not name. The mask is ORed into *mask. */
int os_worker_cores(unsigned main_core,unsigned workers,uint32_t *mask)
{
    static const uint8_t order_a76[]={7,2,3,4,5,0,1},order_a55[]={3,4,5,0,1},order_queue[]={2,3,4,5,0};
    const uint8_t *order=main_core==6?order_a76:main_core==1?order_queue:order_a55;
    const unsigned count=main_core==6?sizeof(order_a76):main_core==1?sizeof(order_queue):sizeof(order_a55);
    if(workers>count)return 0;
    for(unsigned i=0;i<workers;++i)*mask|=1u<<order[i];
    return 1;
}
/* The compositions of services that admission refuses, with the reason of each. A row that is a convenience may be
 * removed once a measurement covers the combination; a row that names a shared hardware resource stays. The services
 * are the session's granted set, so the table is the only place that says which pairs are refused. A row applies in
 * every lifetime unless it names the lifetimes (LIFETIME bits of enum a733_lifetime) it applies in. */
#define LIFETIME(l) (1u<<(l))
static const struct {unsigned services;const char *reason;unsigned lifetimes;} refused_compositions[]={
    {OS_GRANT_NPU|OS_GRANT_AUDIO,"the NPU profile requires the recorded recognition composition, not live I2S capture"},
    {OS_GRANT_NPU|OS_GRANT_RADIO,"the NPU profile requires the recorded recognition composition, not the radio station"},
    {OS_GRANT_NPU|OS_GRANT_HEADSET,"the NPU profile requires the recorded recognition composition, not live headset capture"},
    {OS_GRANT_AUDIO|OS_GRANT_DMA,"one DMA0 owner and INTID 143: the adopted RAM-copy service excludes the I2S stream"},
#ifdef SUNSTONE_A733_OS_CPU_RETURN
    {OS_GRANT_AUDIO,"the returning lifetime has no qualified drain of the I2S stream"},
#ifndef SUNSTONE_A733_OS_RADIO_SCAN
    {OS_GRANT_RADIO,"the returning lifetime shuts down only the scanning radio station"},
#endif
    {OS_GRANT_HEADSET,"the returning lifetime has no qualified shutdown of the Bluetooth module"},
    {OS_GRANT_NPU|OS_GRANT_DMA,"the returning lifetime has no qualified retirement of the NPU beside the RAM-copy service"},
#ifdef SUNSTONE_A733_OS_GPU
    {OS_GRANT_NPU,"the returning lifetime has no qualified retirement of the NPU beside the GPU queue"},
#endif
#endif
#ifdef SUNSTONE_A733_OS_USB_JPEG
    {OS_GRANT_AUDIO,"the USB camera decoder owns run-time non-cacheable allocation; the I2S stream needs allocator serialization"},
    {OS_GRANT_RADIO,"the USB camera decoder owns run-time non-cacheable allocation; the radio station needs allocator serialization"},
    {OS_GRANT_HEADSET,"the USB camera decoder owns run-time non-cacheable allocation; the headset needs allocator serialization"},
    {OS_GRANT_NPU,"the USB camera decoder owns run-time non-cacheable allocation; the NPU service needs allocator serialization"},
#endif
#ifdef SUNSTONE_A733_OS_GPU
    {OS_GRANT_AUDIO,"the supervised GPU queue requires the recorded recognition composition, not live I2S capture",
        LIFETIME(A733_LIFETIME_SUPERVISED)|LIFETIME(A733_LIFETIME_RETURN)},
    {OS_GRANT_RADIO,"the supervised GPU queue requires the recorded recognition composition, not the radio station",
        LIFETIME(A733_LIFETIME_SUPERVISED)|LIFETIME(A733_LIFETIME_RETURN)},
    {OS_GRANT_HEADSET,"the supervised GPU queue requires the recorded recognition composition, not live headset capture",
        LIFETIME(A733_LIFETIME_SUPERVISED)|LIFETIME(A733_LIFETIME_RETURN)},
#endif
    {OS_GRANT_CPU_CLOCK|OS_GRANT_AUDIO,"resident CPU grants require the recorded-card session profile",
        LIFETIME(A733_LIFETIME_RESIDENT)},
    {OS_GRANT_CPU_CLOCK|OS_GRANT_RADIO,"resident CPU grants require the recorded-card session profile",
        LIFETIME(A733_LIFETIME_RESIDENT)},
};
unsigned os_composition_refused(unsigned granted,unsigned lifetime)
{
    for(unsigned i=0;i<sizeof(refused_compositions)/sizeof(refused_compositions[0]);++i){
        const unsigned only=refused_compositions[i].lifetimes;
        if((granted&refused_compositions[i].services)==refused_compositions[i].services&&
           (!only||(lifetime<32&&(only&LIFETIME(lifetime)))))return i+1;
    }
    return 0;
}
const char *os_composition_reason(unsigned row)
{return row&&row<=sizeof(refused_compositions)/sizeof(refused_compositions[0])?refused_compositions[row-1].reason:NULL;}
