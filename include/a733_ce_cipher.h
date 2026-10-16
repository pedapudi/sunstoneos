#ifndef SUNSTONE_A733_CE_CIPHER_H
#define SUNSTONE_A733_CE_CIPHER_H
#include "a733_ce_session.h"
#define A733_CE_CIPHER_MAX_BYTES 2048u
enum a733_ce_cipher_result { A733_CE_CIPHER_PENDING,A733_CE_CIPHER_BUSY,
    A733_CE_CIPHER_OUTPUT,A733_CE_CIPHER_CONSUMED,A733_CE_CIPHER_RETAINED,A733_CE_CIPHER_INVALID };
struct a733_ce_cipher {
    uintptr_t self;
    struct a733_ce_session *engine,*bound_engine;
    atomic_uint busy;
    uint64_t generation,next_generation,session_generation,deadline,last_now;
    size_t bytes,completed,inflight;
    unsigned initialized,active,phase,decrypt,fault,terminal;
    uint8_t key[16],input[A733_CE_CIPHER_MAX_BYTES],output[A733_CE_CIPHER_MAX_BYTES];
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
/* AES-128 ECB only: encrypt with decrypt=0 or decrypt with decrypt=1.
 * Accept 16..2048 bytes in complete 16-byte blocks; copy key/input before return.
 * No IV, counter, padding, authentication, key ladder or CCMP implementation.
 * Each hardware task handles at most 1024 bytes under the same request deadline.
 * generation changes only on PENDING. All supplied buffers are outside private
 * service/session/pool/registry storage; generation cannot overlap key or input. */
enum a733_ce_cipher_result a733_ce_cipher_begin(struct a733_ce_cipher *,const uint8_t key[16],
    const uint8_t *,size_t,unsigned decrypt,uint64_t deadline_us,uint64_t *generation);
/* Output changes only on OUTPUT, exactly once, after all tasks and software key
 * line cleanup succeed. Output size must match the admitted input size. Caller
 * input/key may have been discarded; output may reuse their storage.
 * Successful cleanup zeros only software key backing and cleans its cache line.
 * Internal CE key-register erasure is not established. On failure, clear CPU
 * staging and retain the real session/page/token. DMA key/input/output remain
 * quarantined after uncertainty. No cancellation, reset or release is provided. */
enum a733_ce_cipher_result a733_ce_cipher_poll(struct a733_ce_cipher *,uint64_t generation,uint8_t *,size_t);
#endif
