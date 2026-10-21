#ifndef SUNSTONE_BT_ACL_H
#define SUNSTONE_BT_ACL_H
#include "bt_hci.h"
#define BT_ACL_LINKS 4u
#define BT_ACL_TX_SLOTS 16u
/* Outbound and admitted attempts keep their slot for this controller lifetime.
 * Handles and connected peer addresses are never reused, including after
 * disconnect. Only the same peer may retake a slot whose attempt closed before
 * any handle existed, under a fresh generation. A rejected unknown peer's slot
 * is freed once its failed Connection Complete is matched. New capacity
 * requires externally proven controller recovery and a new HCI/UART generation. */
enum bt_acl_link_state { BT_ACL_UNUSED, BT_ACL_CONNECT_QUEUED,
    BT_ACL_CONNECT_STATUS, BT_ACL_CONNECTING, BT_ACL_CONNECTED,
    BT_ACL_DISCONNECT_QUEUED, BT_ACL_DISCONNECT_STATUS, BT_ACL_DISCONNECTING,
    BT_ACL_CLOSED, BT_ACL_WAIT_REQUEST, BT_ACL_ACCEPT_QUEUED,
    BT_ACL_ACCEPT_STATUS, BT_ACL_ACCEPTING, BT_ACL_REJECT_QUEUED,
    BT_ACL_REJECT_STATUS, BT_ACL_REJECTING };
enum bt_acl_tx_state { BT_ACL_TX_FREE, BT_ACL_QUEUED, BT_ACL_PARTIAL,
    BT_ACL_SENT, BT_ACL_CREDIT_RETIRED, BT_ACL_FLUSHED, BT_ACL_NOT_SENT };
struct bt_acl_peer { uint8_t address[6], page_scan_mode, role_switch; uint16_t clock_offset; };
struct bt_acl_link {
    struct bt_acl_peer peer;
    uint64_t generation, deadline, command_tag, acknowledgement, request_serial;
    uint16_t handle;
    uint8_t status, reason, encryption, class_of_device[3];
    unsigned flush_seen, incoming, handle_valid;
    enum bt_acl_link_state state;
};
struct bt_acl_packet {
    const uint8_t *bytes;
    size_t length;
    uint64_t serial, link_generation, deadline;
    uint16_t handle_flags;
    unsigned uncertain;
    enum bt_acl_tx_state state;
};
enum bt_acl_state { BT_ACL_OFF, BT_ACL_ACTIVE, BT_ACL_FAULT };
/* One static synchronous protocol owner shares unmaskable controller events.
 * Every callback is bounded, non-reentrant and runs on the stack owner task.
 * prepare observes final UART acceptance before event/credit dispatch.
 * owns includes retired handles. validate_credits is read-only; commit_credits
 * cannot fail and changes only this owner's entries in the already validated
 * whole packet. event returns0 retained/unrelated,1 consumed,-1 fault.
 * disconnect validates first; commit cannot fail or invoke other stack steps.
 * Immutable table/context/backing outlive the controller generation. */
struct bt_acl_synchronous_ops {
    unsigned max_handles;
    int (*prepare)(void *);
    int (*acknowledgement_pending)(void *);
    int (*owns)(void *,uint16_t);
    int (*validate_credits)(void *,const uint8_t *entries,unsigned count);
    void (*commit_credits)(void *,const uint8_t *entries,unsigned count);
    int (*validate_disconnect)(void *,const struct bt_h4_record *);
    void (*commit_disconnect)(void *,const struct bt_h4_record *);
    int (*event)(void *,const struct bt_h4_record *);
};
/* Security events share this same serialized HCI owner. Callbacks cannot
 * advance lower transport or submit a command; they copy/validate records.
 * Exact0 leaves an unrelated event for existing ACL/SCO classification. */
struct bt_acl_security_ops {
    int (*prepare)(void *);
    int (*event)(void *,const struct bt_h4_record *);
};
struct bt_acl {
    struct bt_hci *hci;
    const void *l2cap_owner;
    const struct bt_acl_synchronous_ops *synchronous;
    const struct bt_acl_security_ops *security;
    void *security_context;
    void *synchronous_context;
    struct bt_acl_link links[BT_ACL_LINKS];
    struct bt_acl_packet packets[BT_ACL_TX_SLOTS];
    uint64_t next_link, next_packet, seen_record, incoming_timeout_us;
    unsigned capacity, credits, mtu, active_tx, command_link;
    enum bt_acl_state state;
};
/* One serialized task exclusively owns this object and the IDENTIFIED HCI
 * worker. Init requires a fresh bootstrap or completed incoming configuration,
 * no earlier external command/ACL owner and packet-based controller flow control.
 * The configured path retains staged, partial and owned incoming RX through
 * handoff. The ordinary bootstrap path requires all RX storage free. HCI keeps
 * the exact ACL owner address; a duplicate or copied owner is refused.
 * Objects and immutable borrowed payloads must be disjoint from UART, HCI, RX
 * storage and each other. Keep all backing alive on FAULT. Public structure
 * fields are read-only to callers. Exact1 success,0 capacity/busy,-1 refusal. */
int bt_acl_init(struct bt_acl *, struct bt_hci *);
/* Install once after configured audio support, before any synchronous handle.
 * Existing ACL links may remain. Exact owner identity is retained on fault. */
int bt_acl_security_register(struct bt_acl *,const struct bt_acl_security_ops *,void *context);
int bt_acl_synchronous_register(struct bt_acl *,const struct bt_acl_synchronous_ops *,void *context);
/* Nonzero Bluetooth peer address; known page-scan mode0..2 and clock offset
 * supplied by a trusted discovery/configuration owner. Requests DM1 only.
 * A peer whose earlier attempt closed without a handle retakes that slot.
 * Output pointers must be disjoint from every owned object and buffer. */
int bt_acl_connect(struct bt_acl *, const struct bt_acl_peer *, uint64_t deadline,
                   uint64_t *link_generation);
/* Enable incoming ACL handling once, before any attempt, after successful
 * bt_hci_configure_incoming. Its exact read-back controller timeout must exceed
 * authenticated receive latency plus response_timeout_us. The scheduling
 * authority is checked again here and on every HCI step. Preserved incoming
 * records are allowed: enable and reserve permitted peers before first ACL poll.
 * This scalar bounds classification through terminal completion; it is not a
 * controller setting. Unknown peers are rejected with reason0x0e; with no free
 * slot they get no reply and the controller's accept timeout refuses them. A
 * request from a retained address and uncertain command outcomes retain a fault. */
int bt_acl_incoming(struct bt_acl *, uint64_t response_timeout_us);
/* Reserve one copied peer address and a generation for incoming connection
 * admission. This permits baseband connection only: the address and received
 * class are unauthenticated. Real pairing/key/encryption and PSM/service policy
 * remain mandatory at their owners. The absolute bound includes waiting for
 * Request and terminal completion. No automatic expiry/reuse or cancellation;
 * the caller may reserve the peer again after an attempt closed without a
 * handle. A matched request sends Accept with role1 (remain peripheral). */
int bt_acl_listen(struct bt_acl *, const uint8_t address[6], uint64_t deadline,
                  uint64_t *link_generation);
/* Only reason0x13 (local user termination) is currently emitted. */
int bt_acl_disconnect(struct bt_acl *, uint64_t link_generation, uint64_t deadline);
/* One already-fragmented unicast ACL payload, PB1 continuation or PB2 start.
 * No L2CAP framing/security is supplied. Caller must retain immutable bytes
 * until an explicit terminal-result release, even after UART acceptance. */
int bt_acl_submit(struct bt_acl *, uint64_t link_generation, unsigned pb,
                  const uint8_t *, size_t, uint64_t deadline, uint64_t *serial);
/* One HCI step, at most one retained record processed, one new command/frame
 * admitted. budget1..64 bounds each UART direction and parser. Deadlines are
 * absolute microseconds in UART clock. Timeout/fault retains every owner. */
int bt_acl_step(struct bt_acl *, unsigned budget, uint64_t deadline);
const struct bt_acl_packet *bt_acl_result(const struct bt_acl *);
int bt_acl_release(struct bt_acl *, uint64_t serial);
/* Only already-classified records are exposed. ACL RX remains a raw HCI packet;
 * match its handle against links[] to find the immutable generation. Validated
 * informational and vendor events, unmatched command events, unclaimed remote
 * features, failed completions and unanswered requests are released. Unknown,
 * SCO, ISO and security events are retained for later protocol owners. */
const struct bt_h4_record *bt_acl_receive(const struct bt_acl *);
int bt_acl_receive_release(struct bt_acl *, uint64_t serial);
#endif
