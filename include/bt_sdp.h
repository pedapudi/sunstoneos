#ifndef SUNSTONE_BT_SDP_H
#define SUNSTONE_BT_SDP_H
#include "bt_l2cap.h"
#define BT_SDP_BYTES 4096u
#define BT_SDP_SEGMENTS 16u
#define BT_SDP_RECORDS 8u
#define BT_SDP_ATTRIBUTES 64u
#define BT_SDP_DEPTH 8u
#define BT_SDP_ELEMENTS 512u
#define BT_SDP_ENDPOINTS 4u
enum bt_sdp_phase { BT_SDP_OFF, BT_SDP_OPENING, BT_SDP_RUNNING,
    BT_SDP_CLOSING, BT_SDP_CLOSED, BT_SDP_FAULT };
enum bt_sdp_status { BT_SDP_NONE, BT_SDP_PENDING, BT_SDP_COMPLETE,
    BT_SDP_REMOTE_ERROR, BT_SDP_EXPIRED, BT_SDP_LIMIT, BT_SDP_INVALID,
    BT_SDP_LINK_LOST, BT_SDP_CANCELLED };
/* Offsets include the value's data-element header in the retained bytes[]. */
struct bt_sdp_attribute { uint16_t id, offset, length; };
struct bt_sdp_endpoint {
    uint16_t l2cap_psm; /* Zero means the descriptor omitted this parameter. */
    uint8_t rfcomm_channel; /* Advertised byte; the RFCOMM owner must admit it. */
};
struct bt_sdp_record {
    uint32_t handle;
    uint16_t first_attribute, attributes;
    unsigned matches_class, endpoints;
    struct bt_sdp_endpoint endpoint[BT_SDP_ENDPOINTS];
};
struct bt_sdp {
    struct bt_l2cap *l2cap;
    uint64_t channel, link, deadline, close_deadline, tx_serial, request_after, held_rx;
    uint16_t service_uuid, transaction, maximum_bytes, remote_error;
    unsigned waiting, sent, continuation_length, segments, length, records, attributes;
    uint8_t continuation[16], bytes[BT_SDP_BYTES];
    struct bt_sdp_record record[BT_SDP_RECORDS];
    struct bt_sdp_attribute attribute[BT_SDP_ATTRIBUTES];
    enum bt_sdp_phase phase;
    enum bt_sdp_status status;
};
/* Zero-init; one task owns this client and drives the lower stack exclusively.
 * Opens a dedicated PSM1 channel for one ServiceSearchAttribute query, selecting
 * a 16-bit service UUID and all attributes. The existing L2CAP security policy
 * must authorize it. Objects and lower H4 buffers must be disjoint. All backing
 * stays alive until CLOSED, including after query expiry or malformed replies.
 * Deadline is absolute microseconds; it bounds the entire query/continuations.
 * Exact1 accepted,0 lower capacity,-1 refusal,-2 lower identity exhaustion.
 * Public fields are read-only. No heap, automatic retry or security admission. */
int bt_sdp_begin(struct bt_sdp *,struct bt_l2cap *,uint64_t link,uint16_t service_uuid,uint64_t deadline);
/* One lower step and at most one owned SDP response. It also retires this
 * client's lower TX results after query completion/expiry. Other channels and
 * unrelated HCI records remain owned by their respective callers. Return1 for
 * successful worker progress (inspect status),0 CLOSED,-1 retained lower fault. */
int bt_sdp_step(struct bt_sdp *,unsigned uart_budget,uint64_t deadline);
/* Explicitly cancel any pending query and request channel close. The close
 * deadline is independent. Only confirmed close plus released RX/TX permits
 * CLOSED. A quarantined opening needs lower link closure/recovery. */
int bt_sdp_close(struct bt_sdp *,uint64_t deadline);
/* Result views exist only for COMPLETE, and remain valid through CLOSED until
 * release. They report remote advertisements, not authentication or a usable
 * headset connection. Unknown attributes retain their complete encoded values. */
const struct bt_sdp_record *bt_sdp_record(const struct bt_sdp *,unsigned index);
const struct bt_sdp_attribute *bt_sdp_attribute(const struct bt_sdp *,unsigned record,uint16_t id);
int bt_sdp_release(struct bt_sdp *);
#endif
