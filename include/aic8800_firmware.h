#ifndef SUNSTONE_AIC8800_FIRMWARE_H
#define SUNSTONE_AIC8800_FIRMWARE_H
#include "aic8800_sdio_codec.h"
#include "a733_storage_digest.h"
#define AIC8800_FIRMWARE_PACKET_BYTES 1536u
#define AIC8800_FIRMWARE_BLOBS 4u
/* Artifact bytes hashed per step; bounded and below one tick on an A55. */
#define AIC8800_FIRMWARE_HASH_STEP_BYTES 4096u
/* Roles are separate radio resources, not files in a runtime filesystem. */
enum aic8800_firmware_role { AIC8800_BT_ADID, AIC8800_BT_PATCH,
    AIC8800_BT_TABLE, AIC8800_WIFI_IMAGE };
enum aic8800_firmware_state { AIC8800_FW_NEW, AIC8800_FW_VERIFYING,
    AIC8800_FW_VALIDATING, AIC8800_FW_PREPARING, AIC8800_FW_REQUEST,
    AIC8800_FW_WAITING, AIC8800_FW_DELAY, AIC8800_FW_START_CONFIRMED, AIC8800_FW_FAULT };
enum aic8800_firmware_error { AIC8800_FW_NO_ERROR, AIC8800_FW_HASH_ERROR,
    AIC8800_FW_FORMAT_ERROR, AIC8800_FW_TIME_ERROR, AIC8800_FW_PROTOCOL_ERROR,
    AIC8800_FW_READBACK_ERROR, AIC8800_FW_TRANSPORT_ERROR };
struct aic8800_firmware_blob { const uint8_t *data; size_t bytes; };
struct aic8800_firmware_config {
    struct aic8800_radio_identity identity; /* Raw words are decoded again. */
    struct aic8800_firmware_blob blobs[AIC8800_FIRMWARE_BLOBS];
    /* Only profile 1: normal SDIO, shared antenna, UART 1.5M with flow control
     * and low-power signaling, AMSDU receive, no OOB/adaptivity/RF-test overrides. */
    unsigned profile;
};
struct aic8800_firmware {
    struct aic8800_firmware_config config;
    const void *owner;
    enum aic8800_firmware_state state;
    enum aic8800_firmware_error error;
    uint64_t last_now, deadline, power_delay_ticks, delay_until;
    struct storage_digest digest;
    uint32_t pointers[4];
    size_t hash_offset, table_offset, image_offset, request_bytes;
    uint32_t table_type, table_pairs, pair_index, last_status, bootstatus;
    uint32_t expected_address, last_response[2], first_nonzero_block_status;
    uint16_t last_response_message;
    uint16_t expected_message;
    uint8_t initialized, hash_role, h_variant, phase, patch_index;
    uint8_t binding_required, bt_patch_applied, binding_challenge[16];
    uint8_t request[AIC8800_FIRMWARE_PACKET_BYTES];
};
/* Pure initialization; first use requires zero storage. Reject unsupported
 * revisions/artifact sizes/profile and every repeated init. Blob bytes must be
 * immutable and pinned throughout verification, use, fault and external disposal.
 * Config, object and every blob must be independent; blobs cannot overlap.
 * No allocation, filesystem, board access or firmware copy occurs here. */
int aic8800_firmware_init(struct aic8800_firmware *, const struct aic8800_firmware_config *);
/* Privileged transport integration only. Owner is a retained identity for one
 * serialized transport, not an application capability. No reentry/concurrency.
 * Claim follows completed identity under uninterrupted clean bootstrap ownership.
 * One step hashes <=4 KiB, validates one record, or builds one request.
 * REQUEST/WAITING retain the request until a matching CFM; faults never retry.
 * START_CONFIRMED reports the published start CFM, not Wi-Fi/HCI readiness.
 */
int aic8800_firmware_claim(struct aic8800_firmware *, const struct aic8800_radio_identity *,
    const void *owner, uint64_t last_now, uint64_t deadline, uint64_t power_delay_ticks);
void aic8800_firmware_step(struct aic8800_firmware *, const void *owner, uint64_t now);
int aic8800_firmware_sent(struct aic8800_firmware *, const void *owner);
enum aic8800_codec_result aic8800_firmware_response(struct aic8800_firmware *,
    const void *owner, const void *input, size_t available, uint64_t now,
    struct aic8800_sdio_response *output);
void aic8800_firmware_stop(struct aic8800_firmware *, const void *owner,
    enum aic8800_firmware_error);
#endif
