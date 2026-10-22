#ifndef SUNSTONE_A733_CE_HASH_H
#define SUNSTONE_A733_CE_HASH_H
#include "a733_ce_session.h"
#define A733_CE_HASH_MAX_BYTES (UINT64_MAX/8-128)
/* HMAC keys longer than one block are first hashed by the same stream. */
#define A733_CE_HMAC_KEY_MAX 1024u
enum a733_ce_hash_result { A733_CE_HASH_READY,A733_CE_HASH_PENDING,A733_CE_HASH_BUSY,
    A733_CE_HASH_DIGEST,A733_CE_HASH_CONSUMED,A733_CE_HASH_RETAINED,A733_CE_HASH_INVALID };
struct a733_ce_hash {
    uintptr_t self;
    struct a733_ce_session *engine,*bound_engine;
    atomic_uint busy;
    uint64_t generation,next_generation,session_generation,deadline,last_now;
    uint64_t message_bytes,processed_bytes;
    size_t queued,inflight;
    unsigned initialized,active,phase,finalizing,fault,terminal;
    unsigned method,hmac,pass;
    uint8_t digest[64];
    uint8_t key[128]; /* HMAC block-sized key; zeroed with the stream. */
    uint8_t buffer[A733_CE_HASH_INPUT_BYTES+128];
};
/* Zero once; bind the actual shared session with SHA capability. A hash-only
 * service constructs it with a733_ce_session_native_init and SHA256_ONLY.
 * Coexistence binds the exact entropy client's engine pointer. All calls hold
 * the existing pool service lock. A whole stream owns the client slot; entropy
 * requests return BUSY until its digest is consumed. No copied engine authority.
 * Native admission covers actual complete page mappings, clocks, IRQ/security
 * access, remote exclusion, prior drain and recovery. No TRNG profile is required
 * for SHA-only capability. Physical SHA output remains unqualified until tested. */
int a733_ce_hash_bind(struct a733_ce_hash *,struct a733_ce_session *);
/* A SHA-256 stream; equivalent to a733_ce_hash_start(h,A733_CE_SHA256,0,0,...). */
enum a733_ce_hash_result a733_ce_hash_begin(struct a733_ce_hash *,uint64_t deadline_us,uint64_t *generation);
/* Begin a SHA-1, SHA-224, SHA-256, SHA-384 or SHA-512 stream. A nonnull key
 * selects HMAC (RFC 2104) with 0..A733_CE_HMAC_KEY_MAX key bytes, copied
 * before return. The engine runs every compression; software forms the
 * padded key blocks and the outer hash input. READY accepts input at once.
 * A key longer than one block returns PENDING: poll hashes it, then returns
 * READY. The key and its pad blocks are zeroed after the stream completes or
 * fails; the engine's input line holding a pad block is scrubbed per task. */
enum a733_ce_hash_result a733_ce_hash_start(struct a733_ce_hash *,enum a733_ce_hash_method,
    const uint8_t *key,size_t key_bytes,uint64_t deadline_us,uint64_t *generation);
/* Accept at most the available part of a 2048-byte chunk; accepted is written
 * only on READY/PENDING. Call again for its remainder after poll returns READY.
 * Input is copied before return and need not survive polling. No new input while
 * a task is pending. Length overflow and alias checks precede consumption. */
enum a733_ce_hash_result a733_ce_hash_update(struct a733_ce_hash *,uint64_t,const uint8_t *,size_t,size_t *accepted);
/* Final adds standard SHA software padding, including the original message
 * bit length (64-bit field for 64-byte blocks, 128-bit for 128-byte blocks).
 * Empty messages and exact block multiples are supported. HMAC then runs the
 * outer hash before DIGEST. */
enum a733_ce_hash_result a733_ce_hash_final(struct a733_ce_hash *,uint64_t);
/* Output changes only on DIGEST, once per stream; bytes must equal
 * a733_ce_hash_digest(method). READY requests more input or
 * finalization. Each successful task zeros and cleans the DMA output, IV and
 * input after copying. Failure clears private CPU staging and retains the actual page,
 * token and active client slot; DMA memory is not cleared after uncertainty.
 * No cancellation, reset or resource reclamation is provided. */
enum a733_ce_hash_result a733_ce_hash_poll_digest(struct a733_ce_hash *,uint64_t,uint8_t *digest,size_t bytes);
/* a733_ce_hash_poll_digest with 32 bytes: SHA-256 and HMAC-SHA-256 only. */
enum a733_ce_hash_result a733_ce_hash_poll(struct a733_ce_hash *,uint64_t,uint8_t digest[32]);
#endif
