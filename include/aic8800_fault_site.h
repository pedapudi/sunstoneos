#ifndef SUNSTONE_AIC8800_FAULT_SITE_H
#define SUNSTONE_AIC8800_FAULT_SITE_H
#include <stdint.h>
/* Where the security stage of an association failed, for the radio record (head flags bit 12). Codes, states and
 * counts only: no address, SSID, frequency, RSSI, key or request byte.
 * transport_site, stop_site: the first transport fault and the first supplicant stop, each as AIC8800_SITE (file code
 * << 12 | source line; include/aic8800_security.h names the files). transport_error: enum aic8800_transport_error.
 * data: bits 3:0 enum aic8800_data_state, 7:4 enum aic8800_data_error. security: bits 3:0 the security worker's state
 * when it first stopped (its state now when it has not), bit 4 it stopped, bits 7:5 enum aic8800_security_operation.
 * slot: bits 2:0 the key slot of the last add or delete (0 pairwise, 1 + key ID group), 7:4 why the security worker
 * first stopped (enum aic8800_security_stop_reason: 0 another worker's fault, 1 its command deadline, 2 a completed
 * operation left unconsumed, 3 the clock ran backwards, 4 the link left security-pending or the Wi-Fi worker left
 * ready while a command was outstanding, 5 a new association, 6 the matched confirmation was refused). expected_cfm: the confirmation it waits
 * for or last waited for (37 MM_KEY_ADD_CFM, 39 MM_KEY_DEL_CFM, 5125 ME_SET_CONTROL_PORT_CFM). cfm_bytes and
 * cfm_status: the parameter length and first byte of the confirmation that matched the outstanding request, before
 * it was judged (zero until one matched). supplicant: bits 3:0 enum aic8800_supplicant_state, 7:4 queued actions.
 * actions: bit 0 the head action installs a key (clear: it sends EAPOL), 1 the head action is in flight, 2 the port
 * open is pending, 3 the four-way handshake completed, 4 a received EAPOL frame is deferred. ms: the clock in
 * milliseconds, modulo 65536, when the association stage first saw the failure. */
struct aic8800_fault_site {
    uint16_t transport_site;
    uint8_t transport_error, data, security, slot;
    uint16_t expected_cfm;
    uint8_t cfm_bytes, cfm_status, supplicant, actions;
    uint16_t stop_site, ms;
};
_Static_assert(sizeof(struct aic8800_fault_site)==16,"fault site section");
#ifdef SUNSTONE_AIC8800_FAULT_SITE_CAPTURE
#include "aic8800_sdio_transport.h"
#include "aic8800_data.h"
#include "aic8800_security.h"
#include "aic8800_supplicant.h"
static inline void aic8800_fault_site_capture(struct aic8800_fault_site *f,const struct aic8800_sdio_transport *t,
    const struct aic8800_supplicant *s,unsigned ms)
{
    *f=(struct aic8800_fault_site){.ms=(uint16_t)ms};
    if(t->state==AIC8800_TRANSPORT_FAULT){f->transport_site=t->fail_site;f->transport_error=(uint8_t)t->error;}
    const struct aic8800_data *d=t->data;
    if(d)f->data=(uint8_t)(((unsigned)d->state&15u)|((unsigned)d->error&15u)<<4);
    const struct aic8800_security *c=s&&s->security?s->security:t->security;
    if(c){
        unsigned state=c->stopped?c->stop_from:(unsigned)c->state;
        f->security=(uint8_t)((state&15u)|(c->stopped?16u:0)|((unsigned)c->operation&7u)<<5);
        f->slot=(uint8_t)((c->slot&7u)|(c->stopped?((unsigned)c->stop_reason&15u)<<4:0));f->expected_cfm=c->expected_cfm;
        f->cfm_bytes=(uint8_t)(c->seen_cfm_bytes>255u?255u:c->seen_cfm_bytes);f->cfm_status=c->seen_cfm_status;
    }
    if(s&&s->initialized){
        unsigned count=s->count>15u?15u:s->count;
        f->supplicant=(uint8_t)(((unsigned)s->state&15u)|count<<4);
        f->actions=(uint8_t)((s->count&&s->actions[s->head].kind==AIC8800_SUPPLICANT_KEY?1u:0)|(s->action_active?2u:0)|
            (s->port_pending?4u:0)|(s->completed?8u:0)|(s->deferred_bytes?16u:0));
        f->stop_site=s->stop_site;
    }
}
#endif
#endif
