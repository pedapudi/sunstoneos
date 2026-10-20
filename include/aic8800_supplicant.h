#ifndef SUNSTONE_AIC8800_SUPPLICANT_H
#define SUNSTONE_AIC8800_SUPPLICANT_H
#include "aic8800_security.h"
#include "aic8800_data.h"
#define AIC8800_SUPPLICANT_ACTIONS 8u
#define AIC8800_SUPPLICANT_TIMERS 8u
#define AIC8800_SUPPLICANT_SMALL_SLOTS 32u
#define AIC8800_SUPPLICANT_LARGE_SLOTS 4u
struct aic8800_sdio_transport;
struct aic8800_supplicant_config {
    /* Production provider returns exactly 1 after filling all bytes from a
     * cryptographically suitable entropy source; no identifier/time fallback.
     * Context lifetime/extent is caller-owned and excludes all owned objects. */
    int (*entropy)(void *,uint8_t *,size_t);
    void *entropy_context;
    uint64_t entropy_record, beacon_record, ticks_per_second, command_timeout;
    uint8_t pmk[32], ap_rsn[256];
    uint16_t ap_rsn_bytes;
};
enum aic8800_supplicant_state { AIC8800_SUPPLICANT_NEW, AIC8800_SUPPLICANT_HANDSHAKE,
    AIC8800_SUPPLICANT_AUTHORIZED, AIC8800_SUPPLICANT_RETIRING,
    AIC8800_SUPPLICANT_RETIRED, AIC8800_SUPPLICANT_FAULT };
enum aic8800_supplicant_action_kind { AIC8800_SUPPLICANT_EAPOL, AIC8800_SUPPLICANT_KEY };
struct aic8800_supplicant_action {
    enum aic8800_supplicant_action_kind kind;
    uint8_t bytes[AIC8800_DATA_ETHERNET_BYTES], key[16], rsc[6], pairwise, key_id;
    uint16_t length;
};
struct aic8800_supplicant_timer { void (*handler)(void *,void *);void *context,*argument;uint64_t due; };
struct aic8800_supplicant {
    struct aic8800_supplicant_config config;
    struct aic8800_security *security;
    struct aic8800_data *data;
    struct aic8800_wifi *wifi;
    struct aic8800_sdio_transport *owner;
    void *rsn;
    enum aic8800_supplicant_state state;
    int rsn_state;
    uint64_t generation,last_now,deadline;
    uint32_t dropped_eapol;
    struct aic8800_supplicant_action actions[AIC8800_SUPPLICANT_ACTIONS];
    struct aic8800_supplicant_timer timers[AIC8800_SUPPLICANT_TIMERS];
    uint16_t used[AIC8800_SUPPLICANT_SMALL_SLOTS+AIC8800_SUPPLICANT_LARGE_SLOTS];
    _Alignas(16) uint8_t small[AIC8800_SUPPLICANT_SMALL_SLOTS][2048];
    _Alignas(16) uint8_t large[AIC8800_SUPPLICANT_LARGE_SLOTS][16384];
    uint8_t deferred_rx[AIC8800_DATA_ETHERNET_BYTES], deferred_encrypted;
    uint16_t deferred_bytes;
    uint8_t initialized,entered,head,count,action_active,completed,port_pending,retire_closed;
};
/* Fresh zero storage, copied immutable config/PMK/observed AP RSN. Single native
 * RSN owner; all calls occur under transport serialization. No credentials are
 * read from host files. API success never asserts installed keys or RF success.
 * Existing firmware/board identity and caller RF authority remain prerequisites.
 * Fault preserves keys/actions until external recovery. Explicit retirement
 * closes port, removes known keys, then wipes arena and PMK before detachment. */
int aic8800_supplicant_init(struct aic8800_supplicant *,const struct aic8800_supplicant_config *);
int aic8800_supplicant_claim(struct aic8800_supplicant *,struct aic8800_sdio_transport *,
    struct aic8800_security *,uint64_t,uint64_t);
void aic8800_supplicant_observe(struct aic8800_supplicant *,uint64_t);
void aic8800_supplicant_step(struct aic8800_supplicant *,uint64_t);
int aic8800_supplicant_receive(struct aic8800_supplicant *,const struct aic8800_data_frame *,uint64_t);
/* Rearm only this successfully retired and detached pair. Never accepts a
 * fault or uncertain key; preserves the transport/data sequence lifetime. */
int aic8800_supplicant_rearm(struct aic8800_supplicant *,struct aic8800_security *,
    struct aic8800_sdio_transport *,const struct aic8800_supplicant_config *);
int aic8800_supplicant_config_valid(const struct aic8800_supplicant_config *);
int aic8800_supplicant_retire(struct aic8800_supplicant *,uint64_t,uint64_t);
void aic8800_supplicant_stop(struct aic8800_supplicant *);
/* Private external-library OS entry. No global entropy fallback is provided. */
int aic8800_supplicant_os_enter(struct aic8800_supplicant *);
void aic8800_supplicant_os_leave(struct aic8800_supplicant *);
int aic8800_supplicant_os_timer(struct aic8800_supplicant *);
#endif
