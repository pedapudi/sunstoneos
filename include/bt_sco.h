/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BT_SCO_H
#define SUNSTONE_BT_SCO_H
#include "bt_hfp_ag.h"
#define BT_SCO_LINKS 4u
#define BT_SCO_PACKETS 8u
#define BT_SCO_BYTES 255u
struct bt_sco_security {uint64_t generation;unsigned secure_connections;};
struct bt_sco_binding {
    uint64_t scheduling_generation,service_latency_us,response_timeout_us;
    /* Exact1 proves real current audio task/buffer/RTS-CTS service bounds for
     * this firmware/UART generation. A chosen scalar is not such proof. */
    int (*current)(void *,const struct bt_h4_binding *,uint64_t,uint64_t);
    /* Exact1 establishes actual profile security and whether AES-CCM Secure
     * Connections is in use. No default policy; output is copied on admission
     * and must match on every subsequent operation. No controller I/O here. */
    int (*security)(void *,const struct bt_hfp_ag *,struct bt_sco_security *);
    void *context;
};
enum bt_sco_phase {BT_SCO_UNUSED,BT_SCO_FEATURE_QUEUED,BT_SCO_FEATURE_STATUS,
    BT_SCO_FEATURE_WAIT,BT_SCO_LISTENING,BT_SCO_SETUP_QUEUED,BT_SCO_SETUP_STATUS,
    BT_SCO_SETUP_WAIT,BT_SCO_OPEN,BT_SCO_DISCONNECT_QUEUED,BT_SCO_DISCONNECT_STATUS,
    BT_SCO_DISCONNECT_WAIT,BT_SCO_REJECT_QUEUED,BT_SCO_REJECT_STATUS,
    BT_SCO_REJECT_WAIT,BT_SCO_CLOSED,BT_SCO_RETAINED};
enum bt_sco_packet_state {BT_SCO_FREE,BT_SCO_QUEUED,BT_SCO_PARTIAL,BT_SCO_SENT,
    BT_SCO_RETIRED,BT_SCO_FLUSHED,BT_SCO_NOT_SENT,BT_SCO_RECEIVED};
enum bt_sco_error {BT_SCO_OK,BT_SCO_LOWER,BT_SCO_IDENTITY,BT_SCO_PROTOCOL,
    BT_SCO_EXPIRED,BT_SCO_SECURITY,BT_SCO_UNSUPPORTED,BT_SCO_REFUSED};
struct bt_sco_link {
    const struct bt_hfp_ag *ag;
    struct bt_sco_security security;
    uint64_t generation,acl_generation,channel,deadline,command_tag,acknowledgement;
    uint64_t request_serial,request_deadline,closed_after;
    uint16_t handle,acl_handle,rx_packet_length,tx_packet_length;
    uint8_t address[6],features[8],status,reason,type,interval,retransmission;
    unsigned listening,requested,request_type,profile,handle_valid,codec,fallback_allowed,local_closed;
    enum bt_sco_phase phase;
};
struct bt_sco_packet {
    uint64_t serial,link,deadline,acquired;
    size_t length;
    unsigned status;
    enum bt_sco_packet_state state;
    uint8_t bytes[BT_SCO_BYTES];
};
struct bt_sco {
    struct bt_acl *acl;
    const struct bt_sco *self;
    struct bt_sco_binding binding;
    struct bt_sco_link links[BT_SCO_LINKS];
    struct bt_sco_packet tx[BT_SCO_PACKETS],rx[BT_SCO_PACKETS];
    uint64_t next_link,next_packet,now,held_rx[BT_H4_SLOTS];
    /* One Reject Synchronous Connection Request for a request no link expects:
     * 0 idle,1 queued,2 awaiting status,3 awaiting the failed completion. */
    uint64_t refuse_deadline,refuse_tag,refuse_acknowledgement;
    uint8_t refuse_address[6];
    unsigned refuse;
    unsigned capacity,credits,mtu,command_link,active_tx,initialized;
    enum bt_sco_error error;
};
/* One serialized task owns the entire stack. Init requires optional pre-link
 * HCI audio configuration/readback; no guessed firmware/PCM routing. HCI path0,
 * CVSD uses signed16-bit8kHz PCM. A completed AG mSBC selection admits
 * transparent HCI bytes over T1/T2 eSCO; the application owns H2 and codec data.
 * All objects, callbacks, context and lower backing are disjoint and survive
 * faults. Register once per controller generation, no unregister/reinit.
 * Four peer/audio identities and eight copied packets per direction; closed
 * handles and peers stay quarantined except for explicit completed local
 * close/reopen of the same authenticated service connection. */
int bt_sco_init(struct bt_sco *,struct bt_acl *,const struct bt_sco_binding *);
/* Bind actual AG READY and live ACL. listening1 awaits an incoming synchronous
 * request after querying remote features;0 initiates. A synchronous request no
 * link awaits is refused with reason0x0d, one at a time; while a refusal is
 * outstanding, the controller's accept timeout refuses the next. Callback-proven security
 * determines S1 or S4 eSCO safe settings and whether SCO fallback is legal.
 * No active call is fabricated. Exact1 admitted,0 capacity,-1 refusal. */
int bt_sco_open(struct bt_sco *,const struct bt_hfp_ag *,unsigned listening,
                uint64_t deadline,uint64_t *generation);
/* Reopen the exact slot after matched successful local disconnect and release
 * of all its copied packets. Same live AG/ACL/security, drained H4 receive and
 * command/TX boundary, fresh nonzero generation. 0 waits for drain; -1 refuses;
 * neither changes the output or old link identity. No I/O or automatic retry. */
int bt_sco_reopen(struct bt_sco *,uint64_t old_generation,uint64_t deadline,uint64_t *generation);
/* One ACL/HCI step, bounded link scans and at most one new command or data
 * submission. Source-defined fallback follows confirmed terminal failures only;
 * uncertain timeouts retain the complete controller/audio lifetime. */
/* Rebind a never-opened mSBC attempt only after a classified remote-feature
 * refusal or matched terminal setup failure. The same AG must have completed
 * fresh CVSD selection. Returns a fresh audio generation; uncertain faults deny. */
int bt_sco_retry_cvsd(struct bt_sco *,uint64_t,uint64_t,uint64_t *);
int bt_sco_step(struct bt_sco *,unsigned uart_budget,uint64_t deadline);
/* Copy complete signed16-bit PCM samples in little-endian byte order; even
 * length2..min(controller MTU,254) for CVSD. Transparent mSBC accepts
 * arbitrary byte fragments1..min(controller MTU,255). Caller supplies actual playback/silence
 * samples. No implicit speech or silence generation. Controller credits remain
 * separate from UART acceptance and peer rendering. */
int bt_sco_send(struct bt_sco *,uint64_t link,const uint8_t *,size_t,uint64_t deadline,uint64_t *serial);
const struct bt_sco_packet *bt_sco_result(const struct bt_sco *);
const struct bt_sco_packet *bt_sco_receive(const struct bt_sco *);
/* RX preserves HCI byte segmentation and status0..3 (good, possibly invalid,
 * lost, partly lost). Odd packet lengths may split a PCM sample; callers must
 * carry bytes across packet boundaries and preserve loss metadata. Acquired is
 * the H4 record serial, not an RF timestamp. No fabricated replacement samples. */
int bt_sco_release_result(struct bt_sco *,uint64_t serial);
int bt_sco_release_receive(struct bt_sco *,uint64_t serial);
int bt_sco_close(struct bt_sco *,uint64_t link,uint64_t deadline);
#endif
