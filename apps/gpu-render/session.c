#include "session.h"
#include "a733_cpu_coherence.h"
#include "a733_storage_digest.h"
#include <stddef.h>
static unsigned session_sealed;
#if defined(SUNSTONE_GPU_RUNTIME_TEXTURE)
static struct gpu_texture_session texture_session __attribute__((section(".gpu_texture_session_retained")));
#define sealed_session texture_session.base
#define session_storage texture_session
unsigned gpu_application_texture_profile(void)
{ return session_sealed?(unsigned)texture_session.version+1u:0; }
int gpu_application_texture_geometry(uint32_t *width,uint32_t *height)
{
    if(!session_sealed || !width || !height)return 0;
    *width=(uint32_t)texture_session.width;*height=(uint32_t)texture_session.height;return 1;
}
#elif defined(SUNSTONE_GPU_PROGRAM_APPLICATION)
static struct gpu_program_session program_session __attribute__((section(".gpu_program_session_retained")));
#define sealed_session program_session.base
#define session_storage program_session
const struct pvr_compute_package *gpu_application_package(void)
{return session_sealed?&program_session.package:NULL;}
#else
static struct a733_board_session sealed_session;
#define session_storage sealed_session
#endif
const struct a733_board_session *gpu_application_session(void)
{return session_sealed?&sealed_session:NULL;}
int gpu_application_seal_session(const struct a733_smp_contract *c)
{
    uint64_t el;__asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    if(el!=12||session_sealed||!a733_smp_contract_valid(c,a733_smp_counter()))return 0;
    const volatile uint8_t *source=(const void *)A733_BOARD_SESSION_ADDRESS;
    for(unsigned i=0;i<GPU_APPLICATION_SESSION_BYTES;++i)((uint8_t *)&session_storage)[i]=source[i];
    uint8_t digest[32];storage_digest_bytes(&session_storage,GPU_APPLICATION_SESSION_BYTES,digest);
    const uint8_t *bound=gpu_application_contract_digest(c);
    if(!bound)return 0;
    for(unsigned i=0;i<32;++i)if(digest[i]!=bound[i])return 0;
#ifdef SUNSTONE_GPU_RUNTIME_TEXTURE
    if(texture_session.magic!=UINT64_C(0x4153373347545831) || !texture_session.version || texture_session.version>3 ||
        texture_session.bytes!=sizeof(texture_session) || !texture_session.width || !texture_session.height ||
        texture_session.width>PVR_RENDER_TEXTURE_MAX_WIDTH || texture_session.height>PVR_RENDER_TEXTURE_MAX_HEIGHT)return 0;
#endif
#ifdef SUNSTONE_GPU_PROGRAM_APPLICATION
    if(program_session.magic!=UINT64_C(0x4153373347505031)||program_session.version!=1||
       program_session.bytes!=sizeof(program_session)||program_session.package_bytes!=sizeof(program_session.package)||
       !pvr_compute_package_valid(&program_session.package))return 0;
    storage_digest_bytes(&program_session.package,sizeof(program_session.package),digest);
    for(unsigned i=0;i<32;++i)if(digest[i]!=program_session.package_digest[i])return 0;
#endif
    session_sealed=1;
    return a733_cpu_coherence_publish((uintptr_t)&session_storage,GPU_APPLICATION_SESSION_BYTES,c->startup_deadline)==1&&
        a733_cpu_coherence_publish((uintptr_t)&session_sealed,sizeof(session_sealed),c->startup_deadline)==1;
}
