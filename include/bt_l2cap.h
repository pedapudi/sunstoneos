#ifndef SUNSTONE_BT_L2CAP_H
#define SUNSTONE_BT_L2CAP_H
#include "bt_acl.h"
#define BT_L2CAP_CHANNELS 4u
#define BT_L2CAP_PDUS 8u
#define BT_L2CAP_MTU 1024u
#define BT_L2CAP_SIGNAL_MTU 64u
#define BT_L2CAP_IDENTIFIERS 255u
#define BT_L2CAP_REQUESTS (BT_ACL_LINKS*BT_L2CAP_IDENTIFIERS)
#define BT_L2CAP_EXHAUSTED (-2)
enum bt_l2cap_state { BT_L2CAP_OFF, BT_L2CAP_ACTIVE, BT_L2CAP_FAULT };
enum bt_l2cap_error { BT_L2CAP_NONE, BT_L2CAP_LOWER, BT_L2CAP_PROTOCOL,
    BT_L2CAP_TIMEOUT, BT_L2CAP_CAPACITY, BT_L2CAP_OWNERSHIP };
enum bt_l2cap_channel_state { BT_L2CAP_UNUSED, BT_L2CAP_CONNECTING,
    BT_L2CAP_CONFIGURING, BT_L2CAP_OPEN, BT_L2CAP_DISCONNECTING, BT_L2CAP_CLOSED, BT_L2CAP_QUARANTINED };
enum bt_l2cap_timer { BT_L2CAP_TIMER_NONE, BT_L2CAP_RTX, BT_L2CAP_ERTX };
enum bt_l2cap_channel_result { BT_L2CAP_CHANNEL_NONE, BT_L2CAP_CONFIRMED,
    BT_L2CAP_CALLER_EXPIRED, BT_L2CAP_RTX_EXPIRED, BT_L2CAP_ERTX_EXPIRED, BT_L2CAP_LINK_UNCERTAIN };
struct bt_l2cap_channel {
    uint64_t generation, link, deadline, request_serial, request_after, output_serial;
    uint64_t timer_deadline;
    uint16_t local_cid, remote_cid, psm, local_mtu, remote_mtu, result;
    unsigned request_id, request_code, request_sent, input_done, output_done;
    unsigned config_length, config_retries, need_config, request_reused;
    uint8_t used_ids[32];
    enum bt_l2cap_timer timer;
    enum bt_l2cap_channel_result completion;
    uint8_t config[BT_L2CAP_SIGNAL_MTU];
    enum bt_l2cap_channel_state state;
};
enum bt_l2cap_buffer_state { BT_L2CAP_FREE, BT_L2CAP_PARTIAL,
    BT_L2CAP_SIGNAL, BT_L2CAP_RECEIVED, BT_L2CAP_TRANSMITTING,
    BT_L2CAP_COMPLETED, BT_L2CAP_FAILED };
struct bt_l2cap_pdu {
    uint8_t bytes[BT_L2CAP_MTU+4];
    uint64_t serial, link, deadline, first_record, channel;
    /* Acquisition boundary captured when accepted reaches length. Valid only
     * then; immutable until explicit release/reuse, including terminal TX. */
    uint64_t accepted_after, accepted_at;
    size_t length, cursor, accepted;
    unsigned leases, application, failed, request_id;
    uint16_t cid;
    enum bt_l2cap_buffer_state state;
};
struct bt_l2cap_fragment { uint64_t acl_serial, pdu_serial; size_t length; unsigned accepted; };
struct bt_l2cap_request {
    uint64_t link, reply_serial, sequence, channel;
    unsigned length, reply_length;
    uint8_t request[BT_L2CAP_SIGNAL_MTU], reply[BT_L2CAP_SIGNAL_MTU];
};
struct bt_l2cap {
    struct bt_acl *acl;
    struct bt_l2cap_channel channels[BT_L2CAP_CHANNELS];
    struct bt_l2cap_pdu tx[BT_L2CAP_PDUS], rx[BT_L2CAP_PDUS];
    struct bt_l2cap_fragment fragments[BT_ACL_TX_SLOTS];
    struct bt_l2cap_request requests[BT_L2CAP_REQUESTS];
    unsigned next_id[BT_ACL_LINKS], assembling[BT_ACL_LINKS];
    unsigned rotation_broken[BT_ACL_LINKS];
    uint64_t next_serial, timeout_us, ertx_us, next_channel;
    uint64_t peer_sequence[BT_ACL_LINKS];
    unsigned next_cid;
    uint8_t local_used[BT_ACL_LINKS][32];
    uint16_t local_mtu;
    /* Exact1 authorizes this PSM on the current link, including its actual
     * security state; 0 unsupported, any negative value security refusal.
     * Bounded/non-reentrant. It must not call this worker or lower workers. */
    int (*authorize)(void *,uint64_t link,uint16_t psm,unsigned incoming);
    void *context;
    enum bt_l2cap_state state;
    enum bt_l2cap_error error;
};
/* Zero-init once; one task exclusively owns the ACL/HCI/UART workers afterward.
 * Lower link establishment is completed before init. No existing ACL TX owner
 * or received ACL bytes may exist. Unrelated HCI records remain accessible below.
 * q, ACL/HCI/UART objects and H4 RX buffers must be disjoint. Callbacks and all
 * backing remain alive on fault. Public fields are read-only to callers.
 * RTX is1..60 seconds and ERTX60..300 seconds. The admitted infinite-flush
 * profile performs zero retransmissions. Caller deadlines remain independent
 * of protocol timers and lower ownership. Partial reassembly and each emitted
 * packet use separate bounds: RTX duration for reassembly, ERTX duration for
 * signaling transport ownership. Expiry does not authorize reclamation.
 * MTU48..1024; 672 is the protocol default. Exact1 success,0 busy,-1 invalid or
 * fault,-2 exhausted lifetime identity/cache/channel capacity. */
int bt_l2cap_init(struct bt_l2cap *,struct bt_acl *,uint16_t mtu,uint64_t rtx_us,uint64_t ertx_us,
                  int (*authorize)(void *,uint64_t,uint16_t,unsigned),void *context);
/* The initial native configured handoff may retain H4 bytes acquired before
 * upper construction. Claim this ACL once using its actual current binding;
 * no staging/partial framing or received bytes are discarded or relabeled.
 * All existing constructor checks remain. This proves ownership only; normal
 * link, handle, PSM and security validation still precede data admission. */
int bt_l2cap_init_bootstrap(struct bt_l2cap *,struct bt_acl *,uint16_t mtu,uint64_t rtx_us,uint64_t ertx_us,
    int (*authorize)(void *,uint64_t,uint16_t,unsigned),void *context,const struct bt_h4_binding *);
int bt_l2cap_open(struct bt_l2cap *,uint64_t link,uint16_t psm,uint64_t deadline,uint64_t *channel);
int bt_l2cap_close(struct bt_l2cap *,uint64_t channel,uint64_t deadline);
/* Release a confirmed closed channel only after all its RX/TX backing and
 * partial reassembly are released. Expired/uncertain channels remain quarantined.
 * Reclaimed slots receive fresh wire CIDs and opaque generations. */
int bt_l2cap_release_channel(struct bt_l2cap *,uint64_t channel);
/* Copies one basic-mode SDU. Payload length may be zero. Output serial and
 * payload must be disjoint from every worker, retained buffer and each other.
 * Completion means all controller credits retired; it does not prove delivery. */
int bt_l2cap_send(struct bt_l2cap *,uint64_t channel,const uint8_t *,size_t,uint64_t deadline,uint64_t *serial);
int bt_l2cap_step(struct bt_l2cap *,unsigned uart_budget,uint64_t deadline);
const struct bt_l2cap_pdu *bt_l2cap_receive(const struct bt_l2cap *);
const struct bt_l2cap_pdu *bt_l2cap_result(const struct bt_l2cap *);
int bt_l2cap_release_receive(struct bt_l2cap *,uint64_t serial);
int bt_l2cap_release_result(struct bt_l2cap *,uint64_t serial);
/* Retains security/audio/other non-ACL records without preventing ACL progress. */
const struct bt_h4_record *bt_l2cap_other(const struct bt_l2cap *);
int bt_l2cap_release_other(struct bt_l2cap *,uint64_t serial);
#endif
