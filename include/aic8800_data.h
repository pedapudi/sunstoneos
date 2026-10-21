#ifndef SUNSTONE_AIC8800_DATA_H
#define SUNSTONE_AIC8800_DATA_H
#include "aic8800_wifi.h"
struct aic8800_security;
#define AIC8800_DATA_ETHERNET_BYTES 1514u
#define AIC8800_DATA_PACKET_BYTES 1536u
enum aic8800_data_state { AIC8800_DATA_IDLE, AIC8800_DATA_REQUEST,
    AIC8800_DATA_WAITING, AIC8800_DATA_DONE, AIC8800_DATA_FAULT };
enum aic8800_data_error { AIC8800_DATA_NO_ERROR, AIC8800_DATA_TIME_ERROR,
    AIC8800_DATA_LINK_ERROR, AIC8800_DATA_TRANSPORT_ERROR };
struct aic8800_data_result {
    uint64_t generation;
    uint32_t sequence, raw_status, raw_index;
    uint8_t eapol, bus_transferred, firmware_status_valid;
    uint8_t tx_done, retry_required, software_retry_required, acknowledged;
};
struct aic8800_data_frame {
    uint64_t generation;
    const uint8_t *ethernet; /* Borrowed until generic transport consume. */
    uint16_t bytes;
    uint8_t eapol, vif, ap_index, encrypted;
};
struct aic8800_data {
    const void *owner;
    const struct aic8800_wifi *wifi;
    struct aic8800_security *security;
    enum aic8800_data_state state;
    enum aic8800_data_error error;
    uint64_t last_now, deadline, completion_deadline, open_authority, open_generation;
    uint32_t next_sequence;
    uint8_t initialized, held;
    size_t request_bytes;
    const uint8_t *held_record;
    const uint8_t *amsdu; /* Held A-MSDU MPDU in the transport receive buffer. */
    size_t amsdu_next,amsdu_end;
    struct aic8800_data_result result;
    struct aic8800_data_frame frame;
    uint8_t request[AIC8800_DATA_PACKET_BYTES], ethernet[AIC8800_DATA_ETHERNET_BYTES];
};
/* Pure initialization of fresh zero storage. Single privileged transport owner,
 * no reentry; all objects and copied packets survive FAULT until external
 * quiescence/disposal. These worker functions are transport-internal contracts.
 * Inputs/outputs must not overlap any owned object or immutable firmware blob;
 * public transport wrappers enforce this across the complete ownership graph.
 * RX profile: unfragmented station FromDS, RFC1042 SNAP, with or without CCMP.
 * Reorder-flagged MPDUs arrive in order of receipt. An A-MSDU is validated
 * whole, then each subframe is a separate view of the same held record; consume
 * advances to the next subframe and releases the record after the last.
 * Unsupported records remain in the raw transport queue.
 * There is no WPA controlled-port authorization: only EAPOL is admitted while
 * SECURITY_PENDING. Open-network authorization is explicit and generation-bound.
 */
int aic8800_data_init(struct aic8800_data *);
int aic8800_data_claim(struct aic8800_data *, const void *, const struct aic8800_wifi *, uint64_t);
int aic8800_data_authorize_open(struct aic8800_data *, const void *, uint64_t record);
int aic8800_data_send(struct aic8800_data *, const void *, const uint8_t *, size_t, uint64_t, uint64_t);
void aic8800_data_observe(struct aic8800_data *, const void *, uint64_t);
int aic8800_data_sent(struct aic8800_data *, const void *);
enum aic8800_codec_result aic8800_data_response(struct aic8800_data *, const void *,
    const struct aic8800_sdio_response *, uint64_t);
/* Releases the record, or replaces the view with the next A-MSDU subframe. */
void aic8800_data_consumed(struct aic8800_data *, const void *, const uint8_t *);
void aic8800_data_stop(struct aic8800_data *, const void *, enum aic8800_data_error);
#endif
