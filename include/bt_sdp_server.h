/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BT_SDP_SERVER_H
#define SUNSTONE_BT_SDP_SERVER_H
#include "bt_l2cap.h"
#define BT_SDP_SERVER_RECORD_BYTES 256u
struct bt_sdp_ag_profile {
    uint32_t handle, brsf_features;
    uint16_t version, supported_features;
    uint8_t rfcomm_channel, network;
};
enum bt_sdp_server_error { BT_SDP_SERVER_OK, BT_SDP_SERVER_LOWER,
    BT_SDP_SERVER_EXPIRED, BT_SDP_SERVER_IDENTITY, BT_SDP_SERVER_SHORT };
struct bt_sdp_server_session {
    uint64_t link, channel, tx_serial, held_rx, deadline, query_deadline, query, close_deadline;
    unsigned request_length, result_length, offset, pending_length, closing, close_started;
    enum bt_sdp_server_error error;
    uint8_t request[BT_L2CAP_MTU], result[BT_SDP_SERVER_RECORD_BYTES];
    uint8_t pending[BT_L2CAP_MTU];
};
struct bt_sdp_server {
    struct bt_l2cap *l2cap;
    struct bt_sdp_ag_profile profile;
    int (*policy)(void *,uint64_t,uint16_t,unsigned);
    void *context;
    uint64_t response_us, query_us, next_query;
    unsigned rotation, initialized, failed, lower_checked;
    struct bt_sdp_server_session sessions[BT_L2CAP_CHANNELS];
};
/* Zero-init before L2CAP initialization. Profile is copied and immutable;
 * caller must establish that its actual AG service implements every advertised
 * capability and owns this RFCOMM channel. HFP1.9 encoding is supported; no
 * compliance, pairing, key or audio authority is inferred. Current profile
 * excludes wide/super-wide speech and enhanced voice recognition until their
 * native owners supply supported bindings. Network and bits0..4 agree with
 * BRSF; all other SDP feature bits are refused. Handle must be >=0x10000.
 * One serialized task owns all workers. Server, profile, policy context,
 * registry and lower objects/backing must be disjoint and stay alive on fault.
 * Callback is bounded/non-reentrant; exact1 authorizes,0 unsupported,<0 denied.
 * Response/query intervals are nonzero microseconds, query>=response.
 * Exact1 success,-1 refusal. Public fields are read-only. */
int bt_sdp_server_init(struct bt_sdp_server *,struct bt_l2cap *,
    const struct bt_sdp_ag_profile *,uint64_t response_us,uint64_t query_us,
    int (*policy)(void *,uint64_t,uint16_t,unsigned),void *context);
/* Chain below bt_rfcomm_registry.authorize_psm (an aggregate-context wrapper
 * may be needed). Other PSMs and outbound SDP delegate unchanged. Incoming
 * PSM1 reserves only the exact next channel generation after policy success.
 * Only install this callback on the configured L2CAP worker. */
int bt_sdp_server_admit(void *,uint64_t,uint16_t,unsigned);
/* One actual lower step, then bounded session maintenance and one request or
 * response action. At most one response per session waits for controller
 * retirement. Owned channel faults retain bytes and permit unrelated sessions
 * to progress. Global lower failure prevents all further backend access.
 * UART budget/per-call deadline retain their lower meanings. */
int bt_sdp_server_step(struct bt_sdp_server *,unsigned uart_budget,uint64_t deadline);
/* Explicit close may discard owned RX after confirmed closure; it does not
 * erase unresolved TX or expire ownership. Release requires confirmed CLOSED
 * and every lower RX/TX lease retired; faulted lower resources need recovery. */
int bt_sdp_server_close(struct bt_sdp_server *,uint64_t channel,uint64_t deadline);
int bt_sdp_server_release_channel(struct bt_sdp_server *,uint64_t channel);
#endif
