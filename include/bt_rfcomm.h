/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BT_RFCOMM_H
#define SUNSTONE_BT_RFCOMM_H
#include "bt_sdp.h"
#define BT_RFCOMM_MTU 512u
#define BT_RFCOMM_DATA 4u
#define BT_RFCOMM_CONTROL 4u
#define BT_RFCOMM_SLOTS (BT_RFCOMM_DATA+BT_RFCOMM_CONTROL)
struct bt_rfcomm;
struct bt_rfcomm_registry {
    struct bt_l2cap *l2cap;
    struct bt_rfcomm *session[BT_ACL_LINKS];
    int (*authorize_psm)(void *,uint64_t,uint16_t,unsigned);
    int (*authorize_service)(void *,uint64_t,uint16_t,uint8_t);
    void *context;
};
enum bt_rfcomm_phase { BT_RFCOMM_OFF, BT_RFCOMM_OPENING,
    BT_RFCOMM_NEGOTIATING, BT_RFCOMM_OPEN, BT_RFCOMM_CLOSING,
    BT_RFCOMM_CLOSED, BT_RFCOMM_FAULT, BT_RFCOMM_LISTENING };
enum bt_rfcomm_error { BT_RFCOMM_NONE, BT_RFCOMM_LOWER, BT_RFCOMM_PROTOCOL,
    BT_RFCOMM_EXPIRED, BT_RFCOMM_UNSUPPORTED, BT_RFCOMM_SECURITY,
    BT_RFCOMM_REFUSED, BT_RFCOMM_EXHAUSTED };
enum bt_rfcomm_buffer_state { BT_RFCOMM_FREE, BT_RFCOMM_QUEUED,
    BT_RFCOMM_SUBMITTED, BT_RFCOMM_RETIRED, BT_RFCOMM_CANCELLED,
    BT_RFCOMM_RECEIVED };
struct bt_rfcomm_buffer {
    uint64_t serial, lower_serial, deadline;
    size_t length;
    unsigned action, accepted, grant;
    enum bt_rfcomm_buffer_state state;
    uint8_t bytes[BT_RFCOMM_MTU+6];
};
struct bt_rfcomm {
    struct bt_rfcomm_registry *registry;
    struct bt_l2cap *l2cap;
    uint64_t link, channel, deadline, close_deadline, next_serial;
    uint64_t request, request_after, timer, held_rx;
    uint64_t credit_after[BT_RFCOMM_DATA];
    uint64_t admission, mux_after, pn_after, dlc_after;
    uint32_t tx_credits;
    unsigned rx_credits, reserved_credits, next_action, waiting, request_sent;
    unsigned peer_msc, own_msc, dlc_open, wire_closed, closing_lower;
    unsigned incoming, mux_pending, mux_open, pn_pending, pn_ready, dlc_pending;
    uint16_t service_uuid, mtu;
    uint8_t server, dlci, modem_status, line_status;
    enum bt_rfcomm_phase phase;
    enum bt_rfcomm_error error;
    struct bt_rfcomm_buffer tx[BT_RFCOMM_SLOTS],rx[BT_RFCOMM_DATA];
};
/* Initialize before L2CAP; pass bt_rfcomm_admit and this registry to L2CAP init.
 * Both callbacks are mandatory, bounded and non-reentrant. Exact1 authorizes;
 * service authorization must establish the actual profile security policy.
 * The registry, callbacks and context outlive every L2CAP callback and session.
 * All objects, lower backing and caller inputs/outputs must be disjoint.
 * One task owns the complete stack. Public fields are read-only. */
int bt_rfcomm_registry_init(struct bt_rfcomm_registry *,struct bt_l2cap *,
    int (*psm)(void *,uint64_t,uint16_t,unsigned),
    int (*service)(void *,uint64_t,uint16_t,uint8_t),void *context);
/* Delegates unrelated PSMs unchanged. PSM3 requires a reserved outbound
 * acquisition or listener, explicit policy success and no existing session. */
int bt_rfcomm_admit(void *,uint64_t,uint16_t,unsigned);
/* Uses an actual COMPLETE SDP record on this link, retaining a copy of its
 * advertised service/channel. This does not authenticate the peer. MTU23..512.
 * One outbound data DLC per session; credit mode required. Control timers20s,
 * data-DLC SABM timer60s. Caller deadline independently bounds total opening.
 * Exact1 accepted,0 capacity,-1 refusal,-2 exhausted lower identity. */
int bt_rfcomm_begin(struct bt_rfcomm *,struct bt_rfcomm_registry *,
    const struct bt_sdp *,unsigned record,unsigned endpoint,uint16_t mtu,uint64_t deadline);
/* Reserves one incoming server on an already connected link. The application
 * must publish a matching SDP record separately. Local service authorization is
 * mandatory before listener reservation and each incoming admission. Only the
 * exact L2CAP generation captured by this registry's admission can be adopted.
 * One credit-mode data DLC; server channel1..30, MTU23..512. Absolute deadline
 * bounds listening and complete setup. No pairing or incoming ACL setup occurs.
 * Return1 accepted,0 registry capacity,-1 refusal. Keep backing until release. */
int bt_rfcomm_listen(struct bt_rfcomm *,struct bt_rfcomm_registry *,uint64_t link,
    uint16_t local_service_uuid,uint8_t server_channel,uint16_t mtu,uint64_t deadline);
/* One lower step, one received frame and one newly submitted frame per call.
 * Continue after close to drain controller ownership. Fault retains backing;
 * explicit close can recover a protocol fault only while the lower stack and
 * security authority still permit transport. No resynchronization or retries. */
int bt_rfcomm_step(struct bt_rfcomm *,unsigned uart_budget,uint64_t deadline);
/* Copies one nonempty serial-data frame. Local deadline can cancel QUEUED
 * data; accepted lower copies may finish afterward and remain owned until
 * controller retirement. RETIRED is transport retirement, not peer delivery. */
int bt_rfcomm_send(struct bt_rfcomm *,const uint8_t *,size_t,uint64_t deadline,uint64_t *serial);
const struct bt_rfcomm_buffer *bt_rfcomm_receive(const struct bt_rfcomm *);
const struct bt_rfcomm_buffer *bt_rfcomm_result(const struct bt_rfcomm *);
int bt_rfcomm_release_receive(struct bt_rfcomm *,uint64_t serial);
int bt_rfcomm_release_result(struct bt_rfcomm *,uint64_t serial);
/* Confirmed RFCOMM/L2CAP close and lower retirement precede CLOSED. Application
 * RX and terminal TX records remain readable until explicit release. Registry
 * reservation survives faults and is released only by bt_rfcomm_release. */
int bt_rfcomm_close(struct bt_rfcomm *,uint64_t deadline);
int bt_rfcomm_release(struct bt_rfcomm *);
#endif
