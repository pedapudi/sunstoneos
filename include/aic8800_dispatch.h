#ifndef SUNSTONE_AIC8800_DISPATCH_H
#define SUNSTONE_AIC8800_DISPATCH_H
#include "aic8800_sdio_transport.h"
#include "aic8800_wifi.h"
#include "aic8800_data.h"
#define AIC8800_DISPATCH_EVENTS 4u
#define AIC8800_DISPATCH_RECORDS 4u
/* Diagnostic copies keep at most this many leading bytes of each record. */
#define AIC8800_DISPATCH_RECORD_BYTES 4096u
enum aic8800_dispatch_event_kind { AIC8800_DISPATCH_SCAN=1,AIC8800_DISPATCH_STATION,AIC8800_DISPATCH_EAPOL };
enum aic8800_dispatch_record_kind { AIC8800_DISPATCH_UNCLAIMED=1,AIC8800_DISPATCH_PRINT,
    AIC8800_DISPATCH_UNSUPPORTED_DATA,AIC8800_DISPATCH_MALFORMED_UNCLAIMED };
struct aic8800_dispatch_event {
    uint64_t sequence,generation;
    enum aic8800_dispatch_event_kind kind;
    uint16_t bytes;
    /* Pointer members in these copied metadata records are always NULL;
     * frame/indication bytes are in payload and have independent ownership. */
    union {struct aic8800_wifi_scan_result scan;struct aic8800_wifi_station_event station;} metadata;
    uint8_t payload[AIC8800_DATA_ETHERNET_BYTES];
};
struct aic8800_dispatch_record {
    uint64_t sequence,generation;
    enum aic8800_dispatch_record_kind kind;
    uint16_t bytes,message_id; /* bytes counts the copied payload. */
    uint8_t transport_type,has_message,truncated;
    uint8_t payload[AIC8800_DISPATCH_RECORD_BYTES];
};
struct aic8800_dispatch {
    struct aic8800_sdio_transport *transport;
    const uint8_t *terminal_event_record;
    struct aic8800_dispatch_event events[AIC8800_DISPATCH_EVENTS];
    struct aic8800_dispatch_record records[AIC8800_DISPATCH_RECORDS];
    uint64_t next_sequence,retired_records,discarded_diagnostics;
    unsigned event_head,event_count,record_head,record_count;
    uint8_t initialized,entered,faulted;
};
/* Sole privileged receive owner. Fresh zero storage remains pinned for the
 * transport lifetime. No callbacks, allocation, reinitialization or release.
 * init claims an idle runtime; the same worker admits management/security TX.
 * Poll advances at most one transport step or retires one outer record.
 * Matching confirmations and EAPOL execute actual lower protocol consumers.
 * Supported events retain copied bytes; a full event queue applies backpressure.
 * Unclaimed/unsupported records keep the first four diagnostic copies; later
 * diagnostics retire with a saturating count. They never complete a command.
 * A copy holds at most the record's first AIC8800_DISPATCH_RECORD_BYTES bytes
 * and marks a longer record as truncated.
 * Diagnostic bytes are private and may contain sensitive radio payloads.
 */
int aic8800_dispatch_init(struct aic8800_dispatch *,struct aic8800_sdio_transport *);
void aic8800_dispatch_poll(struct aic8800_dispatch *);
int aic8800_dispatch_take_event(struct aic8800_dispatch *,struct aic8800_dispatch_event *);
int aic8800_dispatch_take_record(struct aic8800_dispatch *,struct aic8800_dispatch_record *);
/* Ordinary validated Ethernet remains borrowed until this exact owner consumes
 * it. The network adapter copies it into its fixed IP pool before consumption. */
int aic8800_dispatch_peek_data(const struct aic8800_dispatch *,struct aic8800_data_frame *);
int aic8800_dispatch_consume_data(struct aic8800_dispatch *);
#endif
