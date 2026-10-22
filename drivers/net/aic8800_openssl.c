/* Allocation and entropy for the pinned OpenSSL library. The library has no
 * operating-system seed source in this build, so a small built-in provider
 * named sunstone-seed supplies the seed and nonce for its DRBG hierarchy. */
#include "aic8800_openssl.h"
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/params.h>
#include <openssl/provider.h>
#include <openssl/rand.h>
#include <stdint.h>
#include <string.h>
void *pvPortMalloc(size_t);
void vPortFree(void *);
int os_get_random(unsigned char *,size_t);
/* Every block carries its size in a 16-byte prefix for realloc and wiping. */
#define PREFIX 16u
static size_t held,peak;
static int state;
static void *heap_malloc(size_t n,const char *file,int line)
{
    (void)file;(void)line;
    if(!n||n>SIZE_MAX-PREFIX)return 0;
    uint8_t *p=pvPortMalloc(PREFIX+n);if(!p)return 0;
    memcpy(p,&n,sizeof(n));held+=n;if(held>peak)peak=held;return p+PREFIX;
}
static void heap_free(void *a,const char *file,int line)
{
    (void)file;(void)line;
    if(!a)return;
    uint8_t *p=(uint8_t *)a-PREFIX;size_t n;memcpy(&n,p,sizeof(n));
    OPENSSL_cleanse(p,PREFIX+n);held-=n;vPortFree(p);
}
static void *heap_realloc(void *a,size_t n,const char *file,int line)
{
    if(!a)return heap_malloc(n,file,line);
    if(!n){heap_free(a,file,line);return 0;}
    size_t old;memcpy(&old,(uint8_t *)a-PREFIX,sizeof(old));
    void *next=heap_malloc(n,file,line);
    if(next){memcpy(next,a,old<n?old:n);heap_free(a,file,line);}
    return next;
}
size_t aic8800_openssl_heap_bytes(size_t *maximum){if(maximum)*maximum=peak;return held;}
/* Seed source: each request is filled from os_get_random, which draws on the
 * entered supplicant's hardware entropy callback in pieces of 256 bytes. */
struct seed { int state; };
static int fill(unsigned char *out,size_t n)
{
    for(size_t done=0;done<n;){
        size_t part=n-done>256?256:n-done;
        if(os_get_random(out+done,part)){OPENSSL_cleanse(out,n);return 0;}
        done+=part;
    }
    return 1;
}
static void *seed_new(void *provctx,void *parent,const OSSL_DISPATCH *calls)
{
    (void)provctx;(void)calls;
    if(parent)return 0;
    struct seed *s=OPENSSL_zalloc(sizeof(*s));if(s)s->state=EVP_RAND_STATE_UNINITIALISED;return s;
}
static void seed_free(void *s){OPENSSL_free(s);}
static int seed_instantiate(void *s,unsigned strength,int resistance,const unsigned char *p,size_t n,const OSSL_PARAM params[])
{(void)strength;(void)resistance;(void)p;(void)n;(void)params;((struct seed *)s)->state=EVP_RAND_STATE_READY;return 1;}
static int seed_uninstantiate(void *s){((struct seed *)s)->state=EVP_RAND_STATE_UNINITIALISED;return 1;}
static int seed_generate(void *s,unsigned char *out,size_t n,unsigned strength,int resistance,const unsigned char *a,size_t an)
{
    (void)strength;(void)resistance;(void)a;(void)an;
    /* A refusal fails only this request; the entered supplicant has already
     * faulted, and a later owner may seed again. */
    return ((struct seed *)s)->state==EVP_RAND_STATE_READY&&fill(out,n);
}
static int seed_reseed(void *s,int resistance,const unsigned char *e,size_t en,const unsigned char *a,size_t an)
{(void)resistance;(void)e;(void)en;(void)a;(void)an;return ((struct seed *)s)->state==EVP_RAND_STATE_READY;}
static size_t seed_nonce(void *s,unsigned char *out,unsigned strength,size_t min,size_t max)
{
    (void)strength;(void)max;
    if(!out)return min;
    return seed_generate(s,out,min,0,0,0,0)?min:0;
}
static size_t seed_get(void *s,unsigned char **out,int entropy,size_t min,size_t max,int resistance,const unsigned char *a,size_t an)
{
    (void)resistance;(void)a;(void)an;
    size_t n=min>(size_t)(entropy+7)/8?min:(size_t)(entropy+7)/8;
    unsigned char *p=n&&n<=max?OPENSSL_malloc(n):0;
    if(!p||!seed_generate(s,p,n,0,0,0,0)){OPENSSL_free(p);return 0;}
    *out=p;return n;
}
static void seed_clear(void *s,unsigned char *p,size_t n){(void)s;OPENSSL_clear_free(p,n);}
static int seed_lock_enable(void *s){(void)s;return 1;}
static int seed_lock(void *s){(void)s;return 1;}
static void seed_unlock(void *s){(void)s;}
static int seed_get_params(void *s,OSSL_PARAM params[])
{
    OSSL_PARAM *p=OSSL_PARAM_locate(params,OSSL_RAND_PARAM_STATE);
    if(p&&!OSSL_PARAM_set_int(p,((struct seed *)s)->state))return 0;
    /* The caller-supplied source is defined to be full entropy. */
    p=OSSL_PARAM_locate(params,OSSL_RAND_PARAM_STRENGTH);
    if(p&&!OSSL_PARAM_set_uint(p,256))return 0;
    p=OSSL_PARAM_locate(params,OSSL_RAND_PARAM_MAX_REQUEST);
    return !p||OSSL_PARAM_set_size_t(p,256);
}
static const OSSL_PARAM *seed_gettable(void *s,void *provctx)
{
    (void)s;(void)provctx;
    static const OSSL_PARAM known[]={OSSL_PARAM_int(OSSL_RAND_PARAM_STATE,0),
        OSSL_PARAM_uint(OSSL_RAND_PARAM_STRENGTH,0),OSSL_PARAM_size_t(OSSL_RAND_PARAM_MAX_REQUEST,0),OSSL_PARAM_END};
    return known;
}
static int seed_zeroized(void *s){(void)s;return 1;}
static const OSSL_DISPATCH seed_functions[]={
    {OSSL_FUNC_RAND_NEWCTX,(void (*)(void))seed_new},{OSSL_FUNC_RAND_FREECTX,(void (*)(void))seed_free},
    {OSSL_FUNC_RAND_INSTANTIATE,(void (*)(void))seed_instantiate},
    {OSSL_FUNC_RAND_UNINSTANTIATE,(void (*)(void))seed_uninstantiate},
    {OSSL_FUNC_RAND_GENERATE,(void (*)(void))seed_generate},{OSSL_FUNC_RAND_RESEED,(void (*)(void))seed_reseed},
    {OSSL_FUNC_RAND_NONCE,(void (*)(void))seed_nonce},
    {OSSL_FUNC_RAND_ENABLE_LOCKING,(void (*)(void))seed_lock_enable},
    {OSSL_FUNC_RAND_LOCK,(void (*)(void))seed_lock},{OSSL_FUNC_RAND_UNLOCK,(void (*)(void))seed_unlock},
    {OSSL_FUNC_RAND_GETTABLE_CTX_PARAMS,(void (*)(void))seed_gettable},
    {OSSL_FUNC_RAND_GET_CTX_PARAMS,(void (*)(void))seed_get_params},
    {OSSL_FUNC_RAND_VERIFY_ZEROIZATION,(void (*)(void))seed_zeroized},
    {OSSL_FUNC_RAND_GET_SEED,(void (*)(void))seed_get},{OSSL_FUNC_RAND_CLEAR_SEED,(void (*)(void))seed_clear},
    OSSL_DISPATCH_END};
static const OSSL_ALGORITHM seed_algorithms[]={
    {"SUNSTONE-SEED","provider=sunstone-seed",seed_functions,"Supplicant entropy callback"},{0,0,0,0}};
static const OSSL_ALGORITHM *query(void *provctx,int operation,int *no_cache)
{(void)provctx;*no_cache=0;return operation==OSSL_OP_RAND?seed_algorithms:0;}
static const OSSL_DISPATCH provider_functions[]={
    {OSSL_FUNC_PROVIDER_QUERY_OPERATION,(void (*)(void))query},OSSL_DISPATCH_END};
static int provider_init(const OSSL_CORE_HANDLE *handle,const OSSL_DISPATCH *in,const OSSL_DISPATCH **out,void **provctx)
{(void)in;*out=provider_functions;*provctx=(void *)handle;return 1;}
int aic8800_openssl_init(void)
{
    if(state)return state>0;
    state=-1;
    /* Memory functions must precede every other OpenSSL call. Loading a
     * provider explicitly disables the automatic default-provider load. */
    if(!CRYPTO_set_mem_functions(heap_malloc,heap_realloc,heap_free)||
        !OSSL_PROVIDER_add_builtin(0,"sunstone-seed",provider_init)||
        !RAND_set_seed_source_type(0,"SUNSTONE-SEED","provider=sunstone-seed")||
        !OSSL_PROVIDER_load(0,"sunstone-seed")||!OSSL_PROVIDER_load(0,"default"))return 0;
    state=1;return 1;
}
