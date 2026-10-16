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
struct aic8800_wifi {
    struct aic8800_wifi_config config;
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
void aic8800_wifi_step(struct aic8800_wifi *, const void *owner, uint64_t now);
void aic8800_wifi_observe(struct aic8800_wifi *, const void *owner, uint64_t now);
int aic8800_wifi_sent(struct aic8800_wifi *, const void *owner);
enum aic8800_codec_result aic8800_wifi_response(struct aic8800_wifi *, const void *owner,
    const struct aic8800_sdio_response *, uint64_t now);
void aic8800_wifi_consumed(struct aic8800_wifi *, const void *owner, const uint8_t *record);
void aic8800_wifi_stop(struct aic8800_wifi *, const void *owner, enum aic8800_wifi_error);
#endif
