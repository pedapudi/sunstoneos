/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BT_HFP_AG_H
#define SUNSTONE_BT_HFP_AG_H
#include "bt_rfcomm.h"
#define BT_HFP_AG_LINE 128u
#define BT_HFP_AG_RESPONSE 256u
#define BT_HFP_AG_FEATURES 0u
#define BT_HFP_AG_INDICATORS 7u
#define BT_HFP_AG_NUMBER 32u
/* AG BRSF bits for call rejection, enhanced call status and extended errors. */
#define BT_HFP_AG_CALL_FEATURES 352u
enum bt_hfp_ag_phase { BT_HFP_AG_OFF, BT_HFP_AG_INITIALIZING,
    BT_HFP_AG_READY, BT_HFP_AG_CLOSING, BT_HFP_AG_CLOSED, BT_HFP_AG_FAULT };
enum bt_hfp_ag_error { BT_HFP_AG_NONE, BT_HFP_AG_LOWER, BT_HFP_AG_IDENTITY,
    BT_HFP_AG_EXPIRED };
enum bt_hfp_ag_call_event { BT_HFP_AG_CALL_INCOMING=1, BT_HFP_AG_CALL_ALERTING,
    BT_HFP_AG_CALL_ACTIVE, BT_HFP_AG_CALL_END };
/* One call at most. call/setup equal the call and callsetup indicators;
 * direction0 outgoing,1 incoming. A zero length means no known number. */
struct bt_hfp_ag_call {
    uint8_t number[BT_HFP_AG_NUMBER];
    unsigned length, type, direction, call, setup;
};
/* HF-selected reporting state plus the last dialed number. DTMF digits and
 * HF audio requests are counted for the application, which owns their effect. */
struct bt_hfp_ag_control {
    struct bt_hfp_ag_call call, last;
    unsigned cmee, clip, ccwa, format, dtmf, dtmf_count, audio_requests;
};
struct bt_hfp_ag {
    struct bt_rfcomm *rfcomm;
    struct bt_rfcomm_registry *registry;
    uint64_t link, channel, setup_deadline, timeout_us, now, line_deadline;
    uint64_t response_deadline, transmit, receive;
    size_t receive_cursor, line_length, response_length, response_cursor, chunk;
    uint32_t hf_features, pending_features;
    unsigned local_codecs,peer_codecs,pending_codecs,offered_codec,selected_codec,pending_codec;
    uint64_t codec_deadline;
    unsigned stage, pending_stage, response_accepted, discard, reporting, pending_reporting;
    unsigned active, pending_active, changed, notify_index, notify_value;
    unsigned cmer_mode, pending_mode;
    unsigned gain_valid, microphone_gain, speaker_gain;
    unsigned calls;
    uint64_t ring_us, next_ring;
    struct bt_hfp_ag_control control, pending_control;
    uint8_t indicators[BT_HFP_AG_INDICATORS], notified[BT_HFP_AG_INDICATORS];
    uint8_t line[BT_HFP_AG_LINE], response[BT_HFP_AG_RESPONSE];
    enum bt_hfp_ag_phase phase;
    enum bt_hfp_ag_error error;
};
/* HFP1.9 AG control. The legacy initializer advertises zero features;
 * init_codecs adds only codec negotiation for the concrete wideband consumer.
 * The caller must configure matching local SDP features and retain actual
 * service security admission. This is a control subset, not HFP qualification
 * or microphone readiness. AG role is independent of RFCOMM direction.
 * Bind one OPEN session: incoming local UUID0x111f or outgoing peer UUID0x111e.
 * Exclusive application RX/TX ownership is required from init through release;
 * other serialized stack workers may advance lower transport. Public fields are
 * read-only. All backing/inputs must be disjoint and remain alive on fault.
 * Initial object must be zero. No existing application TX is admitted.
 * Queued RX is adopted from this exact session; no previous application reader
 * or writer may remain.
 * Indicators: service0..1, call0..1, callsetup0..3, callheld0..2, signal0..5,
 * roam0..1, battery0..5. Values must describe actual application/device state.
 * Setup deadline and per-line/response timeout use absolute UART microseconds
 * and duration microseconds respectively; timeout1..60000000. Exact1/-1. */
/* codecs=0 preserves the legacy subset;6 enables CVSD(1)/mSBC(2).
 * Selection commits only after matching BCS and complete response acceptance. */
int bt_hfp_ag_init_codecs(struct bt_hfp_ag *,struct bt_rfcomm *,const uint8_t[7],uint64_t,uint64_t,unsigned codecs);
int bt_hfp_ag_select_codec(struct bt_hfp_ag *,unsigned codec);
int bt_hfp_ag_init(struct bt_hfp_ag *,struct bt_rfcomm *,const uint8_t indicators[7],
    uint64_t setup_deadline,uint64_t timeout_us);
/* ring_us 1..60000000 also enables call control and advertises
 * BT_HFP_AG_CALL_FEATURES; the SDP Network attribute must then be1. The AG
 * then owns the call, callsetup and callheld indicators; callheld stays0
 * because no second call exists. Initial call indicators must be zero.
 * Commands: ATA, AT+CHUP, ATD<dial>;, ATD>n; (no memory: ERROR), AT+BLDN,
 * AT+VTS, AT+CLCC, AT+CLIP, AT+CCWA, AT+CMEE, AT+COPS (no operator), AT+CNUM
 * (no subscriber number) and, with codec negotiation, AT+BCC. Dialing needs
 * service1. Functional refusals use +CME ERROR after AT+CMEE=1. */
int bt_hfp_ag_init_calls(struct bt_hfp_ag *,struct bt_rfcomm *,const uint8_t indicators[7],
    uint64_t setup_deadline,uint64_t timeout_us,unsigned codecs,uint64_t ring_us);
/* Application call events after READY: INCOMING (number NULL or1..32 of
 * 0-9*#+A-D, type128..175) starts RING alerts every ring_us after the callsetup
 * report, with +CLIP after each RING when enabled. ALERTING follows a dialed
 * call; ACTIVE answers or connects; END ends any call or setup. Exact1 applied,
 * 0 while a response owns the serial stream, -1 invalid event or state. */
int bt_hfp_ag_call(struct bt_hfp_ag *,unsigned event,const char *number,unsigned type);
/* One lower step, at most byte_budget input bytes (1..512), one complete AT
 * command and one new response fragment. uart_budget follows the lower1..64
 * bound. READY requires complete CMER OK UART acceptance; controller retirement
 * remains independently owned and does not prove peer/application delivery.
 * Return1 active,0 closed,-1 retained fault/refusal. */
int bt_hfp_ag_step(struct bt_hfp_ag *,unsigned uart_budget,unsigned byte_budget,uint64_t deadline);
/* Copies truthful indicator state atomically. With call control the call,
 * callsetup and callheld values must equal the AG's own state. Reports changed enabled values
 * with CIEV after initialization; call precedes callsetup. Intermediate updates
 * may coalesce while a response owns the serial stream. No call/audio action
 * is performed. Exact1/-1. */
int bt_hfp_ag_indicators(struct bt_hfp_ag *,const uint8_t indicators[7]);
/* Close preserves copied and lower-owned bytes until confirmed RFCOMM close.
 * A bounded failure retains the object. Release detaches only after CLOSED;
 * the caller then owns the separate RFCOMM release. */
int bt_hfp_ag_close(struct bt_hfp_ag *,uint64_t deadline);
int bt_hfp_ag_release(struct bt_hfp_ag *);
#endif
