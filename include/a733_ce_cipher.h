#ifndef SUNSTONE_A733_CE_CIPHER_H
#define SUNSTONE_A733_CE_CIPHER_H
#include "a733_ce_session.h"
#define A733_CE_CIPHER_MAX_BYTES 2048u
#define A733_CE_GCM_AAD_MAX 512u
#define A733_CE_GCM_IV_MAX 64u
/* AUTH reports a GCM tag mismatch: no output, and the session stays usable. */
enum a733_ce_cipher_result { A733_CE_CIPHER_PENDING,A733_CE_CIPHER_BUSY,
    A733_CE_CIPHER_OUTPUT,A733_CE_CIPHER_CONSUMED,A733_CE_CIPHER_RETAINED,A733_CE_CIPHER_INVALID,
    A733_CE_CIPHER_AUTH };
enum a733_ce_cipher_mode { A733_CE_CIPHER_ECB,A733_CE_CIPHER_CBC,A733_CE_CIPHER_CTR,A733_CE_CIPHER_GCM };
/* One request. key_bytes is 16, 24 or 32. ECB takes no IV; CBC and CTR take
 * a 16-byte IV or initial counter block; GCM takes a 1..64-byte IV, at most
 * 512 AAD bytes and a 12..16-byte tag. aad may be null only when empty. */
struct a733_ce_cipher_params {
    const uint8_t *key,*iv,*aad;
    size_t key_bytes,iv_bytes,aad_bytes,tag_bytes;
    enum a733_ce_cipher_mode mode;
    unsigned decrypt;
};
struct a733_ce_cipher {
    uintptr_t self;
    struct a733_ce_session *engine,*bound_engine;
    atomic_uint busy;
    uint64_t generation,next_generation,session_generation,deadline,last_now;
    size_t bytes,completed,inflight,length,aad_bytes,tag_bytes,key_bytes,iv_bytes;
    unsigned initialized,active,phase,decrypt,fault,terminal,mode,subkey,rejected;
    /* Key, chaining IV, GCM hash subkey/J0/tag and AAD are private secrets
     * or request data; all staging is zeroed after publication or failure. */
    uint8_t key[32],iv[16],subkey_block[16],hash_subkey[16],j0[16],tag[16];
    uint8_t nonce[A733_CE_GCM_IV_MAX],aad[A733_CE_GCM_AAD_MAX];
    uint8_t input[A733_CE_CIPHER_MAX_BYTES+16],output[A733_CE_CIPHER_MAX_BYTES+16];
};
/* Zero once and bind the actual retained session with AES capability. Native
 * construction uses a733_ce_session_native_init and an appropriate profile.
 * SHA, entropy and cipher clients share this exact engine, page and token.
 * All calls hold the existing pool service lock. The request owns the client
 * slot until complete output publication; other clients return BUSY.
 * Native admission covers the complete page, actual clocks/security/IRQ access,
 * remote exclusion, independent prior drain and recovery. AES-only requires no
 * TRNG health assertion. Physical AES output remains unqualified until tested. */
int a733_ce_cipher_bind(struct a733_ce_cipher *,struct a733_ce_session *);
/* AES-128 ECB: a733_ce_cipher_start with a 16-byte key and no IV.
 * Encrypt with decrypt=0 or decrypt with decrypt=1.
 * Accept 16..2048 bytes in complete 16-byte blocks; copy key/input before return.
 * No IV, counter, padding, authentication, key ladder or CCMP implementation.
 * Each hardware task handles at most 1024 bytes under the same request deadline.
 * generation changes only on PENDING. All supplied buffers are outside private
 * service/session/pool/registry storage; generation cannot overlap key or input. */
enum a733_ce_cipher_result a733_ce_cipher_begin(struct a733_ce_cipher *,const uint8_t key[16],
    const uint8_t *,size_t,unsigned decrypt,uint64_t deadline_us,uint64_t *generation);
/* Begin any mode. ECB and CBC take 16..2048 bytes in complete blocks. CTR
 * takes 1..2048 bytes and treats both directions alike. GCM encryption takes
 * 0..2048 plaintext bytes and outputs ciphertext followed by the tag; GCM
 * decryption takes ciphertext followed by the tag and outputs plaintext only
 * after constant-time tag verification, otherwise AUTH. Every AES block
 * operation runs on the engine in tasks of at most 1024 bytes; software chains
 * CBC/CTR IVs between tasks and computes GHASH (SP 800-38D) for GCM, whose
 * hash subkey is one engine ECB task. All inputs are copied before return. */
enum a733_ce_cipher_result a733_ce_cipher_start(struct a733_ce_cipher *,const struct a733_ce_cipher_params *,
    const uint8_t *,size_t,uint64_t deadline_us,uint64_t *generation);
/* Output changes only on OUTPUT, exactly once, after all tasks and software key
 * line cleanup succeed. Output size must match the request: the input size,
 * plus the tag for GCM encryption, minus the tag for GCM decryption. Caller
 * input/key may have been discarded; output may reuse their storage.
 * Successful cleanup zeros the DMA key, IV, counter, input and cipher output
 * and cleans them.
 * Internal CE key-register erasure is not established. On failure, clear CPU
 * staging and retain the real session/page/token. DMA key/input/output remain
 * quarantined after uncertainty. No cancellation, reset or release is provided. */
enum a733_ce_cipher_result a733_ce_cipher_poll(struct a733_ce_cipher *,uint64_t generation,uint8_t *,size_t);
#endif
