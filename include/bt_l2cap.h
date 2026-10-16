#ifndef SUNSTONE_BT_L2CAP_H
#define SUNSTONE_BT_L2CAP_H
#include "bt_acl.h"
#define BT_L2CAP_CHANNELS 4u
#define BT_L2CAP_PDUS 8u
#define BT_L2CAP_MTU 1024u
#define BT_L2CAP_SIGNAL_MTU 64u
#define BT_L2CAP_REQUESTS 16u
#define BT_L2CAP_EXHAUSTED (-2)
enum bt_l2cap_state { BT_L2CAP_OFF, BT_L2CAP_ACTIVE, BT_L2CAP_FAULT };
enum bt_l2cap_error { BT_L2CAP_NONE, BT_L2CAP_LOWER, BT_L2CAP_PROTOCOL,
    BT_L2CAP_TIMEOUT, BT_L2CAP_CAPACITY, BT_L2CAP_OWNERSHIP };
enum bt_l2cap_channel_state { BT_L2CAP_UNUSED, BT_L2CAP_CONNECTING,
    BT_L2CAP_CONFIGURING, BT_L2CAP_OPEN, BT_L2CAP_DISCONNECTING, BT_L2CAP_CLOSED };
struct bt_l2cap_channel {
    uint64_t link, deadline, request_serial, request_after, output_serial;
    uint16_t local_cid, remote_cid, psm, local_mtu, remote_mtu, result;
    unsigned request_id, request_code, request_sent, input_done, output_done;
    unsigned config_length, config_retries, need_config;
    uint8_t config[BT_L2CAP_SIGNAL_MTU];
    enum bt_l2cap_channel_state state;
};
enum bt_l2cap_buffer_state { BT_L2CAP_FREE, BT_L2CAP_PARTIAL,
    BT_L2CAP_SIGNAL, BT_L2CAP_RECEIVED, BT_L2CAP_TRANSMITTING,
    BT_L2CAP_COMPLETED, BT_L2CAP_FAILED };
struct bt_l2cap_pdu {
    uint8_t bytes[BT_L2CAP_MTU+4];
    uint64_t serial, link, deadline, first_record;
    size_t length, cursor, accepted;
    unsigned leases, application, failed;
    uint16_t cid;
    enum bt_l2cap_buffer_state state;
};
struct bt_l2cap_fragment { uint64_t acl_serial, pdu_serial; size_t length; unsigned accepted; };
struct bt_l2cap_request {
    uint64_t link, reply_serial;
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
    uint64_t next_serial, timeout_us;
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
 * Timeout1..60 seconds bounds peer operations and partial reassembly; explicit
 * operation deadlines may be shorter. No automatic signaling retransmission.
 * MTU48..1024; 672 is the protocol default. Exact1 success,0 busy,-1 invalid or
 * fault,-2 exhausted lifetime identity/cache/channel capacity. */
int bt_l2cap_init(struct bt_l2cap *,struct bt_acl *,uint16_t mtu,uint64_t timeout_us,
                  int (*authorize)(void *,uint64_t,uint16_t,unsigned),void *context);
int bt_l2cap_open(struct bt_l2cap *,uint64_t link,uint16_t psm,uint64_t deadline,uint16_t *cid);
int bt_l2cap_close(struct bt_l2cap *,uint16_t cid,uint64_t deadline);
/* Copies one basic-mode SDU. Payload length may be zero. Output serial and
 * payload must be disjoint from every worker, retained buffer and each other.
 * Completion means all controller credits retired; it does not prove delivery. */
int bt_l2cap_send(struct bt_l2cap *,uint16_t cid,const uint8_t *,size_t,uint64_t deadline,uint64_t *serial);
int bt_l2cap_step(struct bt_l2cap *,unsigned uart_budget,uint64_t deadline);
const struct bt_l2cap_pdu *bt_l2cap_receive(const struct bt_l2cap *);
const struct bt_l2cap_pdu *bt_l2cap_result(const struct bt_l2cap *);
int bt_l2cap_release_receive(struct bt_l2cap *,uint64_t serial);
int bt_l2cap_release_result(struct bt_l2cap *,uint64_t serial);
/* Retains security/audio/other non-ACL records without preventing ACL progress. */
const struct bt_h4_record *bt_l2cap_other(const struct bt_l2cap *);
int bt_l2cap_release_other(struct bt_l2cap *,uint64_t serial);
#endif
