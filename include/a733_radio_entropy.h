#ifndef SUNSTONE_A733_RADIO_ENTROPY_H
#define SUNSTONE_A733_RADIO_ENTROPY_H
#include "a733_ce_entropy.h"
#include "a733_counter_clock.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#define A733_RADIO_ENTROPY_CAPACITY 512u
struct a733_radio_entropy_config {
    struct a733_ce_entropy *source;
    TaskHandle_t owner;
    SemaphoreHandle_t pool_mutex;
    struct a733_counter_clock *clock; /* Actual counter shared with CE's registry. */
    uint64_t qualification_record, request_timeout_us;
    /* Complete backing of CE callback state, including the counter and locks.
     * The cookie must lie within this immutable platform reservation. */
    const void *provider_state;
    size_t provider_bytes;
    /* Independent authority for this exact session, source and enable epoch.
     * Require assessed raw entropy and successful source-backed startup tests.
     * CE health programming or conditioned-output counts cannot establish this.
     * Success grants immutable qualification for the retained lifetime. */
    int (*qualified)(void *, const struct a733_ce_session *, uint64_t);
    void *qualification_context;
    size_t qualification_bytes;
};
enum a733_radio_entropy_result { A733_RADIO_ENTROPY_PENDING,
    A733_RADIO_ENTROPY_BUSY, A733_RADIO_ENTROPY_READY,
    A733_RADIO_ENTROPY_RETAINED, A733_RADIO_ENTROPY_INVALID };
/* Private platform-lifetime storage, zero once; never copy or reinitialize.
 * Only owner drives source and staging. Consumers touch the copied reservoir
 * under its short atomic lock. Permanent exact CE token and provider lifetimes
 * exclude release, profile replacement and reset, including by other clients. */
struct a733_radio_entropy {
    struct a733_radio_entropy *self;
    struct a733_radio_entropy_config config;
    struct a733_ce_session *engine;
    atomic_uint busy, reservoir_busy, fault;
    uint64_t generation, deadline, last_now;
    unsigned active, staged, retained;
    size_t first, count;
    uint8_t reservoir[A733_RADIO_ENTROPY_CAPACITY];
    uint8_t staging[A733_CE_ENTROPY_MAX];
};
/* Serialized boot, after actual CE entropy initialization and before scheduling.
 * pool_mutex is the existing common pool/service mutex used by EVERY CE client
 * and allocator transition. No new engine, pool, token or mutex is constructed.
 * Current A733 entropy is unqualified: no successful production qualification
 * callback is supplied by this adapter. All objects and callback backing stay
 * independent of the adapter, its configuration and consumer outputs. */
int a733_radio_entropy_init(struct a733_radio_entropy *,const struct a733_radio_entropy_config *);
/* One bounded owner-task iteration. Takes pool_mutex without waiting, advances
 * at most one actual CE request/poll, then publishes a complete 256-byte refill.
 * Call before radio polling; prefill before security/network activation.
 * Registry contention after completion retries metadata, never repeats output.
 * Failure retains CE resources; keep polling for pending metadata retention. */
enum a733_radio_entropy_result a733_radio_entropy_poll(struct a733_radio_entropy *);
/* Callback signatures match WPA2 and network configuration. Only the recorded
 * owner or the actual FreeRTOS IP task can consume. All-or-nothing, 1..256 bytes;
 * no waiting, allocation, CE access or device callback. Exhaustion/contention
 * returns 0 with unchanged output. Network callers already fail closed on 0.
 * No PRNG, seed fallback or replay; consumed reservoir bytes are zeroed. */
int a733_radio_entropy_bytes(void *,uint8_t *,size_t);
int a733_radio_entropy_random32(void *,uint32_t *);
int a733_radio_entropy_sequence(void *,uint32_t,uint16_t,uint32_t,uint16_t,uint32_t *);
#endif
