#ifndef SUNSTONE_A733_CE_SESSION_H
#define SUNSTONE_A733_CE_SESSION_H
#include "a733_ce.h"
#include "device_memory_pool.h"
enum a733_ce_client { A733_CE_CLIENT_ENTROPY=1,A733_CE_CLIENT_SHA256=2,A733_CE_CLIENT_AES128=3 };
struct a733_ce_session {
    uintptr_t self;
    struct {
        struct device_memory_pool *pool;
        struct device_memory_lease lease;
        struct device_memory_extent extent;
        struct a733_clock_leases *registry;
        struct a733_clock_token token;
        struct a733_ce_config config;
        struct a733_ce_ops ops;
        void *cookie;
    } binding;
    struct a733_ce controller;
    const void *client;
    size_t client_bytes;
    uint64_t generation,next_generation;
    unsigned initialized,kind;
    atomic_uint fault; /* Acquire-load permits copied consumers to observe retention. */
    unsigned terminal;
};
/* Zero once. One actual session owns one DEVICE page and one retained CE token.
 * The existing pool service lock serializes EVERY operation and all consumer
 * calls. Portable binding requires that no other controller/session borrows
 * this CE token or the physical engine. All clients must use this session, never copied tokens/controllers or
 * direct embedded-controller calls. Complete capability and mapping authority
 * is authenticated by the existing platform authorize operation before MMIO.
 * Private cooperative metadata is not protection from arbitrary privileged writes. */
int a733_ce_session_bind(struct a733_ce_session *,struct device_memory_pool *,struct device_memory_lease,
    const struct a733_ce_config *,const struct a733_ce_ops *,void *,struct a733_clock_leases *,const struct a733_clock_token *);
int a733_ce_session_native_init(struct a733_ce_session *,const struct a733_ce_config *,const struct a733_ce_ops *,
    void *,struct a733_clock_leases *,const struct a733_clock_token *);
/* Boot constructor uses the existing singleton pool before concurrent users and
 * enforces one native session for the physical engine, even across tokens.
 * An initialized native owner is permanent. Existing clients share its address.
 * These internal constructor helpers implement that actual reservation; abort
 * can undo only an uninitialized construction, never a live/retained session. */
int a733_ce_session_native_claim(struct a733_ce_session *);
void a733_ce_session_native_abort(struct a733_ce_session *);
int a733_ce_session_bound(struct a733_ce_session *);
int a733_ce_session_outside(const struct a733_ce_session *,const void *,size_t);
enum a733_ce_result a733_ce_session_claim(struct a733_ce_session *,const void *client,size_t,
    enum a733_ce_client,uint64_t *generation);
int a733_ce_session_client(struct a733_ce_session *,const void *,enum a733_ce_client,uint64_t);
/* Release only the software client slot at a consumed-task boundary. The page,
 * token and platform authority remain retained; no global drain is claimed. */
int a733_ce_session_done(struct a733_ce_session *,const void *,enum a733_ce_client,uint64_t);
enum a733_ce_result a733_ce_session_retain(struct a733_ce_session *);
#endif
