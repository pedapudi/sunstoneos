/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BT_HFP_AG_H
#define SUNSTONE_BT_HFP_AG_H
#include "bt_rfcomm.h"
#define BT_HFP_AG_LINE 128u
#define BT_HFP_AG_RESPONSE 256u
#define BT_HFP_AG_FEATURES 0u
#define BT_HFP_AG_INDICATORS 7u
#define BT_HFP_AG_NUMBER 32u
/* AG and HF BRSF bits for voice recognition activation (HFP 1.9 4.25). */
#define BT_HFP_AG_VOICE 4u
#define BT_HFP_AG_HF_VOICE 8u
enum bt_hfp_ag_phase { BT_HFP_AG_OFF, BT_HFP_AG_INITIALIZING,
    BT_HFP_AG_READY, BT_HFP_AG_CLOSING, BT_HFP_AG_CLOSED, BT_HFP_AG_FAULT };
enum bt_hfp_ag_error { BT_HFP_AG_NONE, BT_HFP_AG_LOWER, BT_HFP_AG_IDENTITY,
    BT_HFP_AG_EXPIRED };
#ifdef SUNSTONE_BT_HFP_CALL_CONTROL
/* Call control exists for HFP conformance only and has no board use. */
/* AG BRSF bits for call rejection, enhanced call status and extended errors. */
#define BT_HFP_AG_CALL_FEATURES 352u
enum bt_hfp_ag_call_event { BT_HFP_AG_CALL_INCOMING=1, BT_HFP_AG_CALL_ALERTING,
    BT_HFP_AG_CALL_ACTIVE, BT_HFP_AG_CALL_END };
/* One call at most. call/setup equal the call and callsetup indicators;
 * direction0 outgoing,1 incoming. A zero length means no known number. */
struct bt_hfp_ag_call {
    uint8_t number[BT_HFP_AG_NUMBER];
    unsigned length, type, direction, call, setup;
};
#endif
/* HF-selected reporting state; with call control also the call, the last
 * dialed number and DTMF digits. HF audio requests are counted for the
 * application, which owns their effect. */
struct bt_hfp_ag_control {
    unsigned cmee, clip, ccwa, format, audio_requests;
#ifdef SUNSTONE_BT_HFP_CALL_CONTROL
    struct bt_hfp_ag_call call, last;
    unsigned dtmf, dtmf_count;
#endif
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
    unsigned voice, voice_admit, recognition, pending_recognition;
#ifdef SUNSTONE_BT_HFP_CALL_CONTROL
    unsigned calls;
    uint64_t ring_us, next_ring;
#endif
    struct bt_hfp_ag_control control, pending_control;
    uint8_t indicators[BT_HFP_AG_INDICATORS], notified[BT_HFP_AG_INDICATORS];
    uint8_t line[BT_HFP_AG_LINE], response[BT_HFP_AG_RESPONSE];
    enum bt_hfp_ag_phase phase;
    enum bt_hfp_ag_error error;
};
/* HFP1.9 AG control. The caller must configure matching local SDP features
 * and retain actual service security admission. This is a control subset, not
 * HFP qualification or microphone readiness. AG role is independent of
 * RFCOMM direction.
 * Bind one OPEN session: incoming local UUID0x111f or outgoing peer UUID0x111e.
 * Exclusive application RX/TX ownership is required from init through release;
 * other serialized stack workers may advance lower transport. Public fields
 * other than voice_admit are read-only. All backing/inputs must be disjoint
 * and remain alive on fault.
 * Initial object must be zero. No existing application TX is admitted.
 * Queued RX is adopted from this exact session; no previous application reader
 * or writer may remain.
 * Indicators: service0..1, call0..1, callsetup0..3, callheld0..2, signal0..5,
 * roam0..1, battery0..5. Values must describe actual application/device state.
 * Setup deadline and per-line/response timeout use absolute UART microseconds
 * and duration microseconds respectively; timeout1..60000000.
 * codecs0 advertises no codec negotiation; 6 enables CVSD(1)/mSBC(2), and
 * selection commits only after matching BCS and complete response acceptance.
 * voice1 advertises BT_HFP_AG_VOICE; the SDP SupportedFeatures must match.
 * After setup the AG answers AT+CMEE, AT+CLIP, AT+CCWA, AT+COPS (no operator),
 * AT+CNUM (no subscriber number), AT+CLCC and, with codec negotiation,
 * AT+BCC. Refusals use +CME ERROR after AT+CMEE=1. Exact1/-1. */
int bt_hfp_ag_init(struct bt_hfp_ag *,struct bt_rfcomm *,const uint8_t indicators[7],
    uint64_t setup_deadline,uint64_t timeout_us,unsigned codecs,unsigned voice);
int bt_hfp_ag_select_codec(struct bt_hfp_ag *,unsigned codec);
/* Voice recognition (HFP 1.9 4.25). recognition is the committed state: on
 * once the UART accepts the complete OK for AT+BVRA=1 or the complete
 * +BVRA: 1, off likewise for AT+BVRA=0 or +BVRA: 0.
 * AT+BVRA=1 answers OK only while the application admits an activation, the
 * HF set BT_HFP_AG_HF_VOICE and recognition is off; AT+BVRA=0 only while it
 * is on; enhanced status (AT+BVRA=2) is not offered. Each other request
 * answers ERROR and leaves the session usable.
 * voice_admit is the one field the application writes: the task that steps
 * the AG sets it between steps, nonzero to admit the next AT+BVRA=1.
 * voice queues +BVRA: 1 or +BVRA: 0 for a state change after READY, with the
 * HF feature; 1 queued, 0 while a response or partial line owns the serial
 * stream, -1 invalid. */
int bt_hfp_ag_voice(struct bt_hfp_ag *,unsigned on);
#ifdef SUNSTONE_BT_HFP_CALL_CONTROL
/* ring_us 1..60000000 also enables call control and advertises
 * BT_HFP_AG_CALL_FEATURES; the SDP Network attribute must then be1. The AG
 * then owns the call, callsetup and callheld indicators; callheld stays0
 * because no second call exists. Initial call indicators must be zero.
 * Commands: ATA, AT+CHUP, ATD<dial>;, ATD>n; (no memory: ERROR), AT+BLDN and
 * AT+VTS; AT+CLCC reports the call. Dialing needs service1. */
int bt_hfp_ag_init_calls(struct bt_hfp_ag *,struct bt_rfcomm *,const uint8_t indicators[7],
    uint64_t setup_deadline,uint64_t timeout_us,unsigned codecs,unsigned voice,uint64_t ring_us);
/* Application call events after READY: INCOMING (number NULL or1..32 of
 * 0-9*#+A-D, type128..175) starts RING alerts every ring_us after the callsetup
 * report, with +CLIP after each RING when enabled. ALERTING follows a dialed
 * call; ACTIVE answers or connects; END ends any call or setup. Exact1 applied,
 * 0 while a response owns the serial stream, -1 invalid event or state. */
int bt_hfp_ag_call(struct bt_hfp_ag *,unsigned event,const char *number,unsigned type);
#endif
/* One lower step, at most byte_budget input bytes (1..512), one complete AT
 * command and one new response fragment. uart_budget follows the lower1..64
 * bound. READY requires complete CMER OK UART acceptance; controller retirement
 * remains independently owned and does not prove peer/application delivery.
 * Return1 active,0 closed,-1 retained fault/refusal. */
int bt_hfp_ag_step(struct bt_hfp_ag *,unsigned uart_budget,unsigned byte_budget,uint64_t deadline);
/* Copies truthful indicator state atomically. With call control enabled by
 * bt_hfp_ag_init_calls the call, callsetup and callheld values must equal the
 * AG's own state. Reports changed enabled values with CIEV after
 * initialization; call precedes callsetup. Intermediate updates may coalesce
 * while a response owns the serial stream. No call/audio action is performed.
 * Exact1/-1. */
int bt_hfp_ag_indicators(struct bt_hfp_ag *,const uint8_t indicators[7]);
/* Close preserves copied and lower-owned bytes until confirmed RFCOMM close.
 * A bounded failure retains the object. Release detaches only after CLOSED;
 * the caller then owns the separate RFCOMM release. */
int bt_hfp_ag_close(struct bt_hfp_ag *,uint64_t deadline);
int bt_hfp_ag_release(struct bt_hfp_ag *);
#endif
