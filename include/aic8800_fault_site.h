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
/* What the bus did after the last command write, for the radio record (head flags bit 13, after the fault site).
 * Codes and counts only, saturating. tx_id: that command's message id. tx_flags: bit 0 it was submitted, 1 its CMD53
 * completed, 2 a data write followed it, 3 the transport waits for the function interrupt, 4 a receive record is
 * held, 5 the dispatcher faulted. credit: the flow-control value (function 1 register 3) that admitted that write.
 * tx_age_ms: from that write's completion to the transport's last clock read. Since that write: rx_records receive
 * records decoded or retired, rx_kind by transport type (command responses 0x11, data, data confirmations 0x12,
 * prints and others), rx_aggregates receive reads, status_reads function 1 status reads, rx_last_id the last command
 * response's message id (0 none), irq_wakes bits 7:4 the radio's GIC interrupts and 3:0 the owner's wakes from
 * waiting (each saturating at 15), both from the end of the pass that completed the write. phase: the transport's
 * phase. last_status: the last function 1 status value read. queued: bits 7:4 dispatcher events queued, 3:0
 * diagnostic records queued; queued_kinds: 2 bits per queued diagnostic record, oldest in bits 1:0, enum
 * aic8800_dispatch_record_kind minus 1. held_type: the held record's transport type (255 none). data_cfm: bits 7:0
 * of the last data confirmation's status (bit 0 sent, 1 retry required, 2 software retry required, 3 acknowledged).
 * irq: bit 0 the host saw the controller's SDIO interrupt latch set and has not acknowledged it, 1 the controller
 * was read at capture, 2 its SDIO interrupt latch (RINT bit 16) was then set, 3 that interrupt was unmasked (IMASK
 * bit 16), 4 the radio's GIC interrupt is masked. */
struct aic8800_transport_trace {
    uint16_t tx_id;
    uint8_t tx_flags, credit;
    uint16_t tx_age_ms, rx_records;
    uint8_t rx_kind[4], rx_aggregates, status_reads;
    uint16_t rx_last_id;
    uint8_t phase, last_status, queued, queued_kinds, held_type, data_cfm, irq_wakes, irq;
};
_Static_assert(sizeof(struct aic8800_transport_trace)==24,"transport trace section");
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
#include "aic8800_dispatch.h"
static inline unsigned aic8800_trace_nibble(uint64_t v){return v>15u?15u:(unsigned)v;}
/* controller: 1 when raw and mask were read from the SD controller at capture. */
static inline void aic8800_transport_trace_capture(struct aic8800_transport_trace *o,const struct aic8800_sdio_transport *t,
    const struct aic8800_dispatch *d,unsigned gic_masked,uint64_t interrupts,uint64_t wakes,
    unsigned controller,uint32_t raw,uint32_t mask)
{
    *o=(struct aic8800_transport_trace){.tx_id=t->trace_tx_id,.credit=t->trace_credit,.rx_records=t->trace_rx_records,
        .rx_aggregates=t->trace_rx_aggregates,.status_reads=t->trace_status_reads,.rx_last_id=t->trace_rx_last_id,
        .phase=t->phase,.last_status=t->raw_rx_status,.held_type=255u,
        .irq_wakes=(uint8_t)(aic8800_trace_nibble(interrupts)<<4|aic8800_trace_nibble(wakes))};
    for(unsigned i=0;i<4;++i)o->rx_kind[i]=t->trace_rx_kind[i];
    const struct sunxi_sdio *h=t->card?t->card->host:0;
    o->tx_flags=(uint8_t)((t->trace_tx_flags&7u)|(t->irq_wait?8u:0)|(t->held?16u:0)|(d&&d->faulted?32u:0));
    o->irq=(uint8_t)((h&&h->sdio_pending?1u:0)|(controller?2u:0)|(controller&&(raw&(1u<<16))?4u:0)|
        (controller&&(mask&(1u<<16))?8u:0)|(gic_masked?16u:0));
    if(t->trace_tx_flags&2u&&h&&h->last_now>=t->trace_tx_at&&t->config.ticks_per_ms){
        uint64_t ms=(h->last_now-t->trace_tx_at)/t->config.ticks_per_ms;o->tx_age_ms=(uint16_t)(ms>65535u?65535u:ms);}
    if(t->held)o->held_type=t->unrelated.transport_type;
    if(t->data)o->data_cfm=(uint8_t)t->data->result.raw_status;
    if(d&&d->initialized){
        o->queued=(uint8_t)((d->event_count>15u?15u:d->event_count)<<4|(d->record_count>15u?15u:d->record_count));
        for(unsigned i=0;i<d->record_count&&i<4;++i)
            o->queued_kinds|=(uint8_t)((((unsigned)d->records[(d->record_head+i)%AIC8800_DISPATCH_RECORDS].kind-1u)&3u)<<(2*i));
    }
}
#endif
#endif
