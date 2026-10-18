#ifndef SUNSTONE_MIMUS_NATIVE_SESSION_H
#define SUNSTONE_MIMUS_NATIVE_SESSION_H
/* Reuse the immutable session page and scalar wire readers. Mimus has its own
 * admission identity and never carries a storage range or write permission. */
#include "../../platform/a733-storage-maintenance/session.h"
#undef A733_STORAGE_SESSION_MAGIC
#undef A733_STORAGE_CONTRACT_MAGIC
#define A733_STORAGE_SESSION_MAGIC UINT64_C(0x415337334d494d31)
#define A733_STORAGE_CONTRACT_MAGIC UINT64_C(0x415337334d494331)
#define A733_MIMUS_REQUEST_MAGIC UINT64_C(0x415337334d494c31)
/* 160 app version, 168 paced flag, 176 asset bytes, 184 nonce[32],
 * 216 asset SHA256[32], 248 zero, 256 deadline us; 264..415 zero.
 * Common providers retain offsets 416..559,632..637. MMC offsets are zero. */
static inline int a733_mimus_wire_valid(const uint8_t *s)
{
    unsigned nonce=0,hash=0;
    for(unsigned i=0;i<32;++i){nonce|=s[184+i];hash|=s[216+i];}
    if(a733_storage_wire64(s+160)!=1||a733_storage_wire64(s+168)>1||
       a733_storage_wire64(s+176)<128||a733_storage_wire64(s+176)>33554432||
       !nonce||!hash||a733_storage_wire64(s+248))return 0;
    for(unsigned i=264;i<416;++i)if(s[i])return 0;
    for(unsigned i=560;i<632;++i)if(s[i])return 0;
    for(unsigned i=638;i<640;++i)if(s[i])return 0;
    for(unsigned i=656;i<704;++i)if(s[i])return 0;
    return 1;
}
#endif
