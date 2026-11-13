#ifndef SUNSTONE_BLUETOOTH_WORK_STATE_H
#define SUNSTONE_BLUETOOTH_WORK_STATE_H
#include <stdint.h>
#include "bt_sco.h"
#include "bt_sdp_server.h"
/* Exact owner-private progress metadata. No clocks, payload, keys, addresses,
 * pointers or replay histories. Queue bounds are the existing protocol bounds.
 * Zero-filled uint64_t fields make byte comparison exact, including inactive
 * slots. Allocate snapshots from the owner's ledger, never as a stack pair. */
#define BT_WORK_APPLICATION(F) F(stage) F(stop_phase) F(stop_fault) F(result) F(stop_release) F(stop_peer)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_application {BT_WORK_APPLICATION(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_HCI(F) F(state) F(command) F(stage) F(credits) F(opcode) F(command_tag) F(command_accepted) F(external_command) F(configuration_stage) F(configured) F(synchronous_ready) F(security_ack_pending) F(scan_requested) F(scan_complete) F(stop_requested) F(return_command)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_hci {BT_WORK_HCI(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_H4(F) F(state) F(count) F(assembling) F(staged) F(stage_offset) F(tx_header) F(tx_length) F(tx_offset) F(next_serial)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_h4 {BT_WORK_H4(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_RECORD(F) F(owner) F(serial) F(length) F(expected) F(header) F(type)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_record {BT_WORK_RECORD(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_HEADSET(F) F(stage) F(stop) F(idled) F(recognition) F(active) F(bvra) F(voice_off) F(skip) F(idle_close) F(partial_offset) F(segment_bytes) F(segment_records) F(hashed) F(silence_bytes) F(silence_offset) F(silence_ready) F(wideband) F(auth_started) F(configured) F(position) F(partial_serial)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_headset {BT_WORK_HEADSET(BT_WORK_FIELD)
    uint64_t segment_head,segment_tail,segment_stop,segment_flags,segment_disconnected,segment_request,terminal;};
#undef BT_WORK_FIELD
#define BT_WORK_SECURITY(F) F(first) F(count) F(allocated) F(pending) F(configuration_stage) F(configured) F(fatal) F(entered) F(stopping) F(tag) F(pairing_complete)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_security {BT_WORK_SECURITY(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_CONNECTION(F) F(encrypt) F(sec_level) F(required_sec_level) F(key_size) F(key_verified) F(references) F(attached) F(authenticated)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_connection {BT_WORK_CONNECTION(BT_WORK_FIELD) uint64_t flags;};
#undef BT_WORK_FIELD
#define BT_WORK_ACL(F) F(state) F(next_link) F(next_packet) F(seen_record) F(capacity) F(credits) F(mtu) F(active_tx) F(command_link) F(admission_closed) F(stopping)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_acl {BT_WORK_ACL(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_ACL_LINK(F) F(state) F(command_tag) F(acknowledgement) F(request_serial) F(flush_seen) F(incoming) F(handle_valid) F(cancel_event) F(encryption)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_acl_link {BT_WORK_ACL_LINK(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_ACL_PACKET(F) F(serial) F(length) F(uncertain) F(state)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_acl_packet {BT_WORK_ACL_PACKET(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_L2CAP(F) F(state) F(error) F(next_serial) F(next_channel) F(next_cid)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_l2cap {BT_WORK_L2CAP(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_CHANNEL(F) F(state) F(request_serial) F(request_after) F(output_serial) F(request_id) F(request_code) F(request_sent) F(input_done) F(output_done) F(config_length) F(config_retries) F(need_config) F(request_reused) F(timer) F(completion)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_channel {BT_WORK_CHANNEL(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_PDU(F) F(state) F(serial) F(first_record) F(length) F(cursor) F(accepted) F(leases) F(application) F(failed) F(request_id)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_pdu {BT_WORK_PDU(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_FRAGMENT(F) F(acl_serial) F(pdu_serial) F(length) F(accepted)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_fragment {BT_WORK_FRAGMENT(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_RFCOMM(F) F(phase) F(error) F(next_serial) F(request) F(request_after) F(held_rx) F(tx_credits) F(rx_credits) F(reserved_credits) F(next_action) F(waiting) F(request_sent) F(peer_msc) F(own_msc) F(dlc_open) F(wire_closed) F(closing_lower) F(incoming) F(mux_pending) F(mux_open) F(pn_pending) F(pn_ready) F(dlc_pending)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_rfcomm {BT_WORK_RFCOMM(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_RFCOMM_BUFFER(F) F(serial) F(lower_serial) F(length) F(action) F(accepted) F(grant) F(state)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_rfcomm_buffer {BT_WORK_RFCOMM_BUFFER(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_SDP(F) F(phase) F(status) F(tx_serial) F(request_after) F(held_rx) F(waiting) F(sent) F(continuation_length) F(segments) F(length)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_sdp {BT_WORK_SDP(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_SERVER(F) F(initialized) F(failed) F(lower_checked) F(next_query)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_server {BT_WORK_SERVER(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_SERVER_SESSION(F) F(tx_serial) F(held_rx) F(query) F(request_length) F(result_length) F(offset) F(pending_length) F(closing) F(close_started) F(error)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_server_session {BT_WORK_SERVER_SESSION(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_AG(F) F(phase) F(error) F(stage) F(pending_stage) F(transmit) F(receive) F(receive_cursor) F(line_length) F(response_length) F(response_cursor) F(chunk) F(response_accepted) F(discard) F(reporting) F(pending_reporting) F(active) F(pending_active) F(changed) F(notify_index) F(notify_value) F(cmer_mode) F(pending_mode) F(gain_valid) F(voice) F(voice_admit) F(recognition) F(pending_recognition) F(local_codecs) F(peer_codecs) F(pending_codecs) F(offered_codec) F(selected_codec) F(pending_codec)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_ag {BT_WORK_AG(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_SCO(F) F(next_link) F(next_packet) F(refuse) F(refuse_tag) F(refuse_acknowledgement) F(capacity) F(credits) F(mtu) F(command_link) F(active_tx) F(initialized) F(stop_close) F(error)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_sco {BT_WORK_SCO(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_SCO_LINK(F) F(phase) F(command_tag) F(acknowledgement) F(request_serial) F(closed_after) F(listening) F(requested) F(request_type) F(profile) F(handle_valid) F(codec) F(fallback_allowed) F(local_closed)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_sco_link {BT_WORK_SCO_LINK(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
#define BT_WORK_SCO_PACKET(F) F(serial) F(length) F(status) F(state)
#define BT_WORK_FIELD(name) uint64_t name;
struct bluetooth_work_sco_packet {BT_WORK_SCO_PACKET(BT_WORK_FIELD)};
#undef BT_WORK_FIELD
struct bluetooth_work_state {
    struct bluetooth_work_application application;
    struct bluetooth_work_hci hci;
    struct bluetooth_work_h4 h4;
    struct bluetooth_work_record records[BT_H4_SLOTS];
    struct bluetooth_work_headset headset;
    struct bluetooth_work_security security;
    struct bluetooth_work_connection connections[BT_ACL_LINKS];
    struct bluetooth_work_acl acl;
    struct bluetooth_work_acl_link links[BT_ACL_LINKS];
    struct bluetooth_work_acl_packet packets[BT_ACL_TX_SLOTS];
    struct bluetooth_work_l2cap l2cap;
    struct bluetooth_work_channel channels[BT_L2CAP_CHANNELS];
    struct bluetooth_work_pdu tx[BT_L2CAP_PDUS],rx[BT_L2CAP_PDUS];
    struct bluetooth_work_fragment fragments[BT_ACL_TX_SLOTS];
    uint64_t assembling[BT_ACL_LINKS],next_id[BT_ACL_LINKS],peer_sequence[BT_ACL_LINKS];
    struct bluetooth_work_rfcomm rfcomm;
    struct bluetooth_work_rfcomm_buffer rfcomm_tx[BT_RFCOMM_SLOTS],rfcomm_rx[BT_RFCOMM_DATA];
    uint64_t credit_after[BT_RFCOMM_DATA];
    struct bluetooth_work_sdp discovery;
    struct bluetooth_work_server server;
    struct bluetooth_work_server_session sessions[BT_L2CAP_CHANNELS];
    struct bluetooth_work_ag ag;
    struct bluetooth_work_sco audio;
    struct bluetooth_work_sco_link audio_links[BT_SCO_LINKS];
    struct bluetooth_work_sco_packet audio_tx[BT_SCO_PACKETS],audio_rx[BT_SCO_PACKETS];
    uint64_t held_rx[BT_H4_SLOTS];
    uint64_t stop_phase,snapshot_bytes,snapshot_records;
};
_Static_assert(sizeof(struct bluetooth_work_state)==7232,"Work snapshot ledger geometry");
struct bluetooth_application;
/* Pure, designated owner only. Exact 1 fills; 0 refuses invalid/failed state
 * or output overlapping private storage. Unconstructed layers remain zero. */
int bluetooth_application_work_state(const struct bluetooth_application *,struct bluetooth_work_state *);
/* ACTIVE H4 only: 1 pending acquisition/classification, 0 none, -1 invalid/failed.
 * OWNED records retain RX age until the actual ACL seen_record reaches them. */
int bluetooth_application_rx_unclassified(const struct bluetooth_application *);
#endif
