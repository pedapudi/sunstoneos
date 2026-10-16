#ifndef SUNSTONE_AIC8800_WIFI_H
#define SUNSTONE_AIC8800_WIFI_H
#include "aic8800_sdio_codec.h"
#define AIC8800_WIFI_CHANNELS 42u
#define AIC8800_WIFI_FRAME_BYTES 1000u
struct aic8800_wifi_channel { uint16_t frequency; uint8_t band, flags; int8_t max_power_dbm; };
struct aic8800_wifi_config {
    /* Nonzero records identify the caller-approved module RF profile and channel
     * policy. They do not create regulatory authority or authorize board testing.
     * Profile 1 uses the pinned D80 default power table and real calibration
     * request, no file/xtal/offset overrides. Channels are copied, then immutable.
     * Every channel must carry NO_IR=1; optional RADAR=4 is preserved. */
    uint64_t rf_profile_record, channel_policy_record, command_timeout;
    unsigned profile, channel_count;
    struct aic8800_wifi_channel channels[AIC8800_WIFI_CHANNELS];
};
enum aic8800_wifi_state { AIC8800_WIFI_NEW, AIC8800_WIFI_PREPARING,
    AIC8800_WIFI_REQUEST, AIC8800_WIFI_WAITING, AIC8800_WIFI_READY,
    AIC8800_WIFI_SCAN_DONE, AIC8800_WIFI_FAULT };
enum aic8800_wifi_error { AIC8800_WIFI_NO_ERROR, AIC8800_WIFI_TIME_ERROR,
    AIC8800_WIFI_MESSAGE_ERROR, AIC8800_WIFI_MAC_ERROR, AIC8800_WIFI_VERSION_ERROR,
    AIC8800_WIFI_STATUS_ERROR, AIC8800_WIFI_RESULT_ERROR, AIC8800_WIFI_TRANSPORT_ERROR };
struct aic8800_wifi_scan_result {
    uint16_t frequency, frame_control, frame_bytes;
    int8_t rssi_dbm;
    uint8_t band, vif, bssid[6], ssid[32], ssid_bytes, has_ssid;
    /* Owned by worker; valid only while held. Copy before consume. */
    const uint8_t *frame;
};
struct aic8800_wifi_scan_summary { uint32_t received; uint8_t vif, status, reported_results, accepted; };
enum aic8800_wifi_security { AIC8800_WIFI_OPEN = 1, AIC8800_WIFI_WPA2_PSK_CCMP };
enum aic8800_wifi_link { AIC8800_WIFI_LINK_DOWN, AIC8800_WIFI_CONNECTING,
    AIC8800_WIFI_ASSOCIATED_OPEN, AIC8800_WIFI_SECURITY_PENDING, AIC8800_WIFI_DISCONNECTING, AIC8800_WIFI_LINK_UNRESOLVED, AIC8800_WIFI_SECURED };
struct aic8800_wifi_target {
    /* Independent caller authority for active association and a previously
     * observed matching beacon. A scan result or numeric ID alone is no proof.
     * Copies remain immutable through connection/disconnection. No credentials. */
    uint64_t connection_authority, beacon_evidence;
    unsigned channel_index;
    enum aic8800_wifi_security security;
    uint8_t bssid[6], ssid[32], ssid_bytes;
};
enum aic8800_wifi_station_event_kind { AIC8800_WIFI_ASSOCIATION,
    AIC8800_WIFI_CONNECTION_REFUSED, AIC8800_WIFI_DISCONNECTION };
struct aic8800_wifi_station_event {
    enum aic8800_wifi_station_event_kind kind;
    enum aic8800_wifi_link link;
    uint16_t status, aid, frequency, request_ie_bytes, response_ie_bytes, bytes;
    uint32_t center_frequency1, center_frequency2, ac_parameters[4];
    uint8_t bssid[6], vif, ap_index, channel_index, qos, acm, band, width, ft_over_ds, reassoc;
    /* Entire source indication retained, including unspecified padding. Owned
     * by worker; borrowed only until generic transport consume. */
    const uint8_t *payload;
};
struct aic8800_wifi {
    struct aic8800_wifi_config config;
    struct aic8800_wifi_target target;
    struct aic8800_wifi_station_event station_event;
    enum aic8800_wifi_link link;
    const uint8_t *station_record;
    uint64_t association_generation; /* Never reused during this lifetime. */
    uint8_t ap_index, peer_qos;
    uint16_t disconnect_reason;
    uint8_t station_held, station_accepted, station_indicated;
    uint8_t station_payload[852];
    const void *owner;
    enum aic8800_wifi_state state;
    enum aic8800_wifi_error error;
    uint64_t last_now, deadline, command_deadline, completion_deadline;
    uint32_t calibration_addresses[4], version[6];
    struct aic8800_wifi_scan_result result;
    struct aic8800_wifi_scan_summary summary;
    const uint8_t *held_record;
    uint16_t last_cfm_id, last_cfm_bytes, expected_cfm;
    uint16_t max_stations;
    uint8_t initialized, phase, mac[6], max_vifs, vif, scan_done, held;
    uint8_t request[512], last_cfm[1024], frame[AIC8800_WIFI_FRAME_BYTES];
};
/* Zero storage, once only, no hardware access. All effects/resources remain
 * retained after fault. READY means management initialized, not association.
 * Serialized privileged owner; no reentry. No arbitrary command or RF writes. */
int aic8800_wifi_init(struct aic8800_wifi *, const struct aic8800_wifi_config *);
int aic8800_wifi_claim(struct aic8800_wifi *, const void *owner, uint64_t now, uint64_t deadline);
int aic8800_wifi_scan(struct aic8800_wifi *, const void *owner, uint64_t now, uint64_t deadline);
/* Only the serialized transport owner admits these operations. Association
 * never means network readiness; WPA2 remains SECURITY_PENDING. */
int aic8800_wifi_connect(struct aic8800_wifi *, const void *, const struct aic8800_wifi_target *, uint64_t, uint64_t);
int aic8800_wifi_disconnect(struct aic8800_wifi *, const void *, uint16_t reason, uint64_t, uint64_t);
void aic8800_wifi_step(struct aic8800_wifi *, const void *owner, uint64_t now);
void aic8800_wifi_observe(struct aic8800_wifi *, const void *owner, uint64_t now);
int aic8800_wifi_sent(struct aic8800_wifi *, const void *owner);
enum aic8800_codec_result aic8800_wifi_response(struct aic8800_wifi *, const void *owner,
    const struct aic8800_sdio_response *, uint64_t now);
void aic8800_wifi_consumed(struct aic8800_wifi *, const void *owner, const uint8_t *record);
void aic8800_wifi_stop(struct aic8800_wifi *, const void *owner, enum aic8800_wifi_error);
#endif
