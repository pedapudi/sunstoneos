#ifndef SUNSTONE_BT_ACL_H
#define SUNSTONE_BT_ACL_H
#include "bt_hci.h"
#define BT_ACL_LINKS 4u
#define BT_ACL_TX_SLOTS 16u
/* Connection attempts consume a slot for this controller lifetime. Handles
 * and peer addresses are never reused, including after failure/disconnect. New capacity requires
 * externally proven controller recovery and a new HCI/UART generation. */
enum bt_acl_link_state { BT_ACL_UNUSED, BT_ACL_CONNECT_QUEUED,
    BT_ACL_CONNECT_STATUS, BT_ACL_CONNECTING, BT_ACL_CONNECTED,
    BT_ACL_DISCONNECT_QUEUED, BT_ACL_DISCONNECT_STATUS, BT_ACL_DISCONNECTING,
    BT_ACL_CLOSED };
enum bt_acl_tx_state { BT_ACL_TX_FREE, BT_ACL_QUEUED, BT_ACL_PARTIAL,
    BT_ACL_SENT, BT_ACL_CREDIT_RETIRED, BT_ACL_FLUSHED, BT_ACL_NOT_SENT };
struct bt_acl_peer { uint8_t address[6], page_scan_mode, role_switch; uint16_t clock_offset; };
struct bt_acl_link {
    struct bt_acl_peer peer;
    uint64_t generation, deadline, command_tag, acknowledgement;
    uint16_t handle;
    uint8_t status, reason, encryption;
    unsigned flush_seen;
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
struct bt_acl {
    struct bt_hci *hci;
    struct bt_acl_link links[BT_ACL_LINKS];
    struct bt_acl_packet packets[BT_ACL_TX_SLOTS];
    uint64_t next_link, next_packet, seen_record;
    unsigned capacity, credits, mtu, active_tx, command_link;
    enum bt_acl_state state;
};
/* One serialized task exclusively owns this object and the IDENTIFIED HCI
 * worker. Init requires a fresh bootstrap with all RX records released, no
 * earlier external command/ACL owner and packet-based controller flow control.
 * Objects and immutable borrowed payloads must be disjoint from UART, HCI, RX
 * storage and each other. Keep all backing alive on FAULT. Public structure
 * fields are read-only to callers. Exact1 success,0 capacity/busy,-1 refusal. */
int bt_acl_init(struct bt_acl *, struct bt_hci *);
/* Nonzero Bluetooth peer address; known page-scan mode0..2 and clock offset
 * supplied by a trusted discovery/configuration owner. Requests DM1 only.
 * Output pointers must be disjoint from every owned object and buffer. */
int bt_acl_connect(struct bt_acl *, const struct bt_acl_peer *, uint64_t deadline,
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
 * match its handle against links[] to find the immutable generation. Unknown,
 * SCO, ISO and security events are retained for later protocol owners. */
const struct bt_h4_record *bt_acl_receive(const struct bt_acl *);
int bt_acl_receive_release(struct bt_acl *, uint64_t serial);
#endif
