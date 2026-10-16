#ifndef SUNSTONE_AIC8800_SECURITY_H
#define SUNSTONE_AIC8800_SECURITY_H
#include "aic8800_wifi.h"
#define AIC8800_SECURITY_KEYS 5u
#define AIC8800_SECURITY_TIDS 17u
struct aic8800_supplicant;
enum aic8800_security_state { AIC8800_SECURITY_IDLE, AIC8800_SECURITY_REQUEST,
    AIC8800_SECURITY_WAITING, AIC8800_SECURITY_DONE, AIC8800_SECURITY_FAULT };
enum aic8800_security_operation { AIC8800_SECURITY_ADD, AIC8800_SECURITY_DELETE, AIC8800_SECURITY_PORT };
struct aic8800_security_key {
    uint64_t generation, pn[AIC8800_SECURITY_TIDS];
    uint8_t material[16], valid, pairwise, key_id, hardware_index;
};
struct aic8800_security {
    const void *owner;
    struct aic8800_wifi *wifi;
    struct aic8800_supplicant *supplicant;
    struct aic8800_security_key keys[AIC8800_SECURITY_KEYS], pending_key;
    enum aic8800_security_state state;
    enum aic8800_security_operation operation;
    uint64_t generation, last_now, deadline, completion_deadline;
    uint8_t initialized, slot, requested_open, authorized, retiring, retired, port_confirmed;
    uint8_t request[512], last_cfm[64];
    uint16_t expected_cfm, last_cfm_bytes;
};
/* Privileged transport/RSN adapter only. All key material remains private to
 * these objects; no diagnostic dump or generic application key-write API.
 * Fresh zero storage, one association generation, no reentry. Fault retains
 * installed and uncertain keys and commands until external recovery proves
 * quiescence. Normal retirement closes the firmware port and deletes each key
 * before secret disposal/reconnection. No key reinstallation resets RX PN. */
int aic8800_security_init(struct aic8800_security *);
int aic8800_security_claim(struct aic8800_security *,const void *,struct aic8800_wifi *,struct aic8800_supplicant *,uint64_t);
int aic8800_security_add(struct aic8800_security *,const void *,unsigned pairwise,unsigned key_id,
    const uint8_t key[16],const uint8_t rsc[6],uint64_t now,uint64_t deadline);
int aic8800_security_delete(struct aic8800_security *,const void *,unsigned slot,uint64_t,uint64_t);
int aic8800_security_port(struct aic8800_security *,const void *,int opened,uint64_t,uint64_t);
void aic8800_security_observe(struct aic8800_security *,const void *,uint64_t);
int aic8800_security_sent(struct aic8800_security *,const void *);
enum aic8800_codec_result aic8800_security_response(struct aic8800_security *,const void *,
    const struct aic8800_sdio_response *,uint64_t);
void aic8800_security_stop(struct aic8800_security *,const void *);
int aic8800_security_open(const struct aic8800_security *);
/* Verify source-defined decrypted CCMP header and strictly increasing PN.
 * Caller commits PN only after the complete Ethernet frame is retained. */
int aic8800_security_rx_key(struct aic8800_security *,unsigned group,unsigned key_id,
    unsigned tid,uint64_t pn,unsigned *slot);
void aic8800_security_rx_commit(struct aic8800_security *,unsigned slot,unsigned tid,uint64_t pn);
void aic8800_security_zero(void *,size_t);
#endif
