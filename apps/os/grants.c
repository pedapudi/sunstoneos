/* The services a session grants: the wire size of the records it carries and the compositions that admission refuses.
 * Pure functions of the granted set, shared by the EL3 admission, the host pre-flight and checks/os-session-grants.py. */
#include "os.h"
size_t os_session_wire_bytes(unsigned granted)
{
    return A733_OS_SESSION_BYTES+((granted&OS_GRANT_AUDIO)?OS_AUDIO_SESSION_BYTES:0)+
        ((granted&OS_GRANT_NPU)?OS_NPU_SESSION_BYTES:0)+((granted&OS_GRANT_CPU_CLOCK)?OS_CPU_CLOCK_SESSION_BYTES:0)+
        ((granted&OS_GRANT_DMA)?OS_DMA_SESSION_BYTES:0);
}
/* The compositions of services that admission refuses, with the reason of each. A row that is a convenience may be
 * removed once a measurement covers the combination; a row that names a shared hardware resource stays. The services
 * are the session's granted set, so the table is the only place that says which pairs are refused. */
static const struct {unsigned services;const char *reason;} refused_compositions[]={
    {OS_GRANT_NPU|OS_GRANT_AUDIO,"the NPU profile requires the recorded recognition composition, not live I2S capture"},
    {OS_GRANT_NPU|OS_GRANT_RADIO,"the NPU profile requires the recorded recognition composition, not the radio station"},
    {OS_GRANT_AUDIO|OS_GRANT_DMA,"one DMA0 owner and INTID 143: the adopted RAM-copy service excludes the I2S stream"},
#ifdef SUNSTONE_BOARD_RESIDENT
    {OS_GRANT_CPU_CLOCK|OS_GRANT_AUDIO,"resident CPU grants require the recorded-card session profile"},
    {OS_GRANT_CPU_CLOCK|OS_GRANT_RADIO,"resident CPU grants require the recorded-card session profile"},
#endif
#if !defined(SUNSTONE_A733_OS_PERF)&&!defined(SUNSTONE_BOARD_RESIDENT)&&!defined(SUNSTONE_A733_OS_GPU)&&!defined(SUNSTONE_A733_OS_CPU_RETURN)
    {OS_GRANT_NPU|OS_GRANT_CPU_CLOCK,"CPU grants compose with the NPU only in a recorded resident, GPU or returning image, or with performance modes"},
#endif
};
unsigned os_composition_refused(unsigned granted)
{
    for(unsigned i=0;i<sizeof(refused_compositions)/sizeof(refused_compositions[0]);++i)
        if((granted&refused_compositions[i].services)==refused_compositions[i].services)return i+1;
    return 0;
}
const char *os_composition_reason(unsigned row)
{return row&&row<=sizeof(refused_compositions)/sizeof(refused_compositions[0])?refused_compositions[row-1].reason:NULL;}
