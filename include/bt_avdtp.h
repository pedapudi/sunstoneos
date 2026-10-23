/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BT_AVDTP_H
#define SUNSTONE_BT_AVDTP_H
#include "bt_l2cap.h"
#define BT_AVDTP_PSM 0x19u
#define BT_AVDTP_SEID 1u
#define BT_AVDTP_RESPONSE 32u
#define BT_AVDTP_SLOTS 4u
#define BT_AVDTP_PAYLOAD 1024u
/* Stream endpoint states from AVDTP1.3 section 9 for the one SBC sink. */
enum bt_avdtp_stream { BT_AVDTP_IDLE, BT_AVDTP_CONFIGURED, BT_AVDTP_OPEN,
    BT_AVDTP_STREAMING, BT_AVDTP_CLOSING, BT_AVDTP_ABORTING };
enum bt_avdtp_error { BT_AVDTP_OK, BT_AVDTP_LOWER, BT_AVDTP_EXPIRED,
    BT_AVDTP_PROTOCOL, BT_AVDTP_IDENTITY, BT_AVDTP_SECURITY };
/* One copied media packet: whole SBC frames after the RTP and media payload
 * headers. gap counts packets the application never received before this
 * one, including invalid and dropped packets. pdu is the L2CAP serial. */
struct bt_avdtp_media {
    uint64_t serial, pdu;
    uint32_t timestamp;
    uint16_t sequence;
    unsigned frames, length, gap, published;
    uint8_t bytes[BT_AVDTP_PAYLOAD];
};
struct bt_avdtp {
    struct bt_l2cap *l2cap;
    int (*policy)(void *,uint64_t,uint16_t,unsigned);
    void *context;
    uint64_t link, signal, media, tx_serial, deadline, response_us, close_deadline;
    uint64_t next_serial, dropped, invalid, gaps;
    uint8_t capabilities[4], configuration[4], response[BT_AVDTP_RESPONSE];
    unsigned response_length, int_seid, closing, sequence_valid;
    unsigned initialized, lower_checked;
    uint16_t sequence;
    enum bt_avdtp_stream stream;
    enum bt_avdtp_error error;
    struct bt_avdtp_media slots[BT_AVDTP_SLOTS];
};
/* Zero-init before L2CAP. Acceptor-only A2DP1.3 sink over AVDTP1.3 with one
 * SBC endpoint, SEID1; this side sends no AVDTP commands. Capabilities use
 * the A2DP SBC information element and must include every sink-mandatory
 * value: 44.1 and 48 kHz, all channel modes, block lengths, subband counts
 * and allocation methods, with bitpool2<=min<=max<=250. Delay reporting,
 * content protection, reconfiguration and fragmented signaling are refused.
 * The caller publishes the matching SDP sink record. Policy establishes the
 * actual link security for PSM0x19 and delegates other PSMs; it is bounded
 * and non-reentrant. One task owns all workers; objects and backing are
 * disjoint and stay alive on fault. response_us is1..60000000. Exact1/-1. */
int bt_avdtp_init(struct bt_avdtp *,struct bt_l2cap *,const uint8_t capabilities[4],uint64_t response_us,
    int (*policy)(void *,uint64_t,uint16_t,unsigned),void *context);
/* Install beneath the RFCOMM registry like bt_sdp_server_admit. Incoming
 * PSM0x19 reserves the next channel generation as the signaling channel of
 * the only admitted link, then as its media channel after an accepted OPEN.
 * Outgoing PSM0x19 is refused; other PSMs delegate unchanged, also after a
 * sink fault, which refuses only PSM0x19. */
int bt_avdtp_admit(void *,uint64_t,uint16_t,unsigned);
/* One lower step, channel maintenance, at most one signaling command with
 * its response and one media packet. Media is copied only while STREAMING
 * and only when every SBC frame header matches the configuration; a full
 * slot set drops the packet. Return1 active,0 closed after local close,
 * -1 retained fault. A peer that closes signaling returns to IDLE. While a
 * channel is held and no local close is pending, each step first rechecks
 * the policy for PSM0x19; any result but1 is a retained SECURITY fault. */
int bt_avdtp_step(struct bt_avdtp *,unsigned uart_budget,uint64_t deadline);
/* Oldest published media; release it by serial to free the slot. */
const struct bt_avdtp_media *bt_avdtp_media(const struct bt_avdtp *);
int bt_avdtp_release_media(struct bt_avdtp *,uint64_t serial);
/* Local shutdown: stop responses, close the media then the signaling channel
 * after outstanding response retirement, and release both after confirmed
 * closure. Published media stays readable. Exact1/-1. */
int bt_avdtp_close(struct bt_avdtp *,uint64_t deadline);
#endif
