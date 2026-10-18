#ifndef SUNSTONE_MIMUS_NATIVE_SESSION_H
#define SUNSTONE_MIMUS_NATIVE_SESSION_H
/* Reuse the immutable session page and scalar wire readers. Mimus has its own
 * admission identity and never carries a storage range or write permission. */
#include "../../platform/a733-storage-maintenance/session.h"
#undef A733_STORAGE_SESSION_MAGIC
#undef A733_STORAGE_CONTRACT_MAGIC
#ifdef SUNSTONE_MIMUS_SUPERVISED
#define A733_STORAGE_SESSION_MAGIC UINT64_C(0x415337334d494d32)
#define MIMUS_SESSION_VERSION 2u
#else
#define A733_STORAGE_SESSION_MAGIC UINT64_C(0x415337334d494d31)
#define MIMUS_SESSION_VERSION 1u
#endif
#ifdef SUNSTONE_MIMUS_SUPERVISED
#define A733_STORAGE_CONTRACT_MAGIC UINT64_C(0x415337334d494333)
#else
#define A733_STORAGE_CONTRACT_MAGIC UINT64_C(0x415337334d494331)
#endif
#define A733_MIMUS_REQUEST_MAGIC UINT64_C(0x415337334d494c31)
/* 160 app version, 168 paced flag, 176 asset bytes, 184 nonce[32],
 * 216 asset SHA256[32], 248 zero, 256 deadline us; 264..415 zero.
 * Common providers retain offsets 416..559,632..637. MMC offsets are zero.
 * Explicit supervised version2 uses zero watchdog/deadline/reload fields and
 * linked build SHA256 at656..687. The full session digest binds this value to
 * the live version3 contract; default version1 keeps656..703 zero. */
static inline int a733_mimus_wire_valid(const uint8_t *s)
{
    unsigned nonce=0,hash=0;
    for(unsigned i=0;i<32;++i){nonce|=s[184+i];hash|=s[216+i];}
    if(a733_storage_wire64(s+160)!=MIMUS_SESSION_VERSION||a733_storage_wire64(s+168)>1||
       a733_storage_wire64(s+176)<128||a733_storage_wire64(s+176)>33554432||
       !nonce||!hash||a733_storage_wire64(s+248))return 0;
    for(unsigned i=264;i<416;++i)if(s[i])return 0;
    for(unsigned i=560;i<632;++i)if(s[i])return 0;
    for(unsigned i=638;i<640;++i)if(s[i])return 0;
#ifdef SUNSTONE_MIMUS_SUPERVISED
    for(unsigned i=688;i<704;++i)if(s[i])return 0;
#else
    for(unsigned i=656;i<704;++i)if(s[i])return 0;
#endif
    return 1;
}
#ifdef SUNSTONE_MIMUS_SUPERVISED
static inline int a733_mimus_supervised_wire_valid(const uint8_t *s)
{
 if(!a733_mimus_wire_valid(s)||a733_storage_wire64(s)!=A733_STORAGE_SESSION_MAGIC||
    a733_storage_wire64(s+8)!=2||a733_storage_wire64(s+16)!=704||
    !a733_storage_wire64(s+24)||!a733_storage_wire64(s+32)||!a733_storage_wire64(s+40)||
    !a733_storage_wire64(s+416)||!a733_storage_wire64(s+512)||!a733_storage_wire64(s+520)||
    a733_storage_wire64(s+512)==a733_storage_wire64(s+520))return 0;
 const unsigned ranges[][2]={{112,160},{256,416},{456,488},{504,512},{552,556},{560,632},{638,656},{688,704}};
 for(unsigned r=0;r<sizeof(ranges)/sizeof(ranges[0]);++r)
  for(unsigned i=ranges[r][0];i<ranges[r][1];++i)if(s[i])return 0;
 for(unsigned r=0;r<3;++r){unsigned start=r==2?424:48+32*r,bits=0;
  for(unsigned i=0;i<32;++i)bits|=s[start+i];if(!bits)return 0;}
 return 1;
}
#endif
#endif
