/* SHA-256 with the ARMv8 Cryptographic Extension, for hashing at EL1 or EL0 only.
 *
 * storage_digest.c stays free of SIMD so that EL3 code, which runs with no FP/SIMD trap configuration and saves no vector
 * state, can hash through it (checks/elf-memory-hazards.py follows every direct call from the EL3 roots). This file holds
 * the only reference to storage_sha256_ce (storage_digest_ce.S); an image links both or neither. */
#include "a733_storage_digest.h"
#include <stdint.h>
#include <string.h>

extern void storage_sha256_ce(uint32_t h[8],const uint8_t *p,size_t blocks);
static int use_ce;
int storage_digest_use_extensions(int on)
{
    use_ce=on!=0;
    return use_ce;
}
int storage_digest_add_extensions(struct storage_digest *d,const void *data,size_t n)
{
    const uint8_t *p=data;
    if(!use_ce||!d||(!p&&n)||n>UINT64_MAX/8-d->bytes)return storage_digest_add(d,data,n);
    if(d->used){
        /* Complete the buffered block first so the rest starts on a block boundary. */
        size_t k=64-d->used;if(k>n)k=n;
        if(!storage_digest_add(d,p,k))return 0;
        p+=k;n-=k;
    }
    if(!d->used&&n>=64){
        size_t b=n/64;
        d->bytes+=64*b;
        storage_sha256_ce(d->h,p,b);
        p+=64*b;n-=64*b;
    }
    return n?storage_digest_add(d,p,n):1;
}
