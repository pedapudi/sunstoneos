#ifndef SUNSTONE_AIC8800_RUNTIME_H
#define SUNSTONE_AIC8800_RUNTIME_H
#include "aic8800_sdio_codec.h"
struct aic8800_runtime_config {
    /* The published Allwinner path requires a successful platform chip-ID read.
     * Its returned bytes are not used by the challenge algorithm or a secret.
     * Return 1 for success, 0 for failure. Callback runs once, must be bounded, and may not reenter any radio API.
     * Context and its storage remain independent and retained through disposal. */
    int (*read_platform_id)(void *context, uint8_t id[16]);
    void *context;
    uint64_t binding_timeout;
};
enum aic8800_runtime_state { AIC8800_RUNTIME_NEW, AIC8800_RUNTIME_RECEIVING,
    AIC8800_RUNTIME_VERIFYING, AIC8800_RUNTIME_REQUEST, AIC8800_RUNTIME_WAITING,
    AIC8800_RUNTIME_FAULT };
enum aic8800_runtime_error { AIC8800_RUNTIME_NO_ERROR, AIC8800_RUNTIME_TIME_ERROR,
    AIC8800_RUNTIME_PLATFORM_ERROR, AIC8800_RUNTIME_CHALLENGE_ERROR,
    AIC8800_RUNTIME_MESSAGE_ERROR, AIC8800_RUNTIME_BINDING_REJECTED,
    AIC8800_RUNTIME_TRANSPORT_ERROR };
struct aic8800_runtime {
    struct aic8800_runtime_config config;
    const void *owner;
    enum aic8800_runtime_state state;
    enum aic8800_runtime_error error;
    uint64_t last_now, binding_deadline;
    uint8_t initialized, binding_seen, binding_confirmed, binding_status, stage;
    uint8_t challenge[16], platform_id[16], random[4], encrypted[4], reply[16];
    uint8_t request[512];
};
/* Pure initialization of fresh zero storage. No reuse, disposal, retry or reset.
 * Runtime/config must not overlap; retain the object throughout its transport
 * lifetime, including after faults. RECEIVING is not Wi-Fi or Bluetooth readiness.
 * Data frames and other messages retain raw bytes for a future protocol consumer.
 */
int aic8800_runtime_init(struct aic8800_runtime *, const struct aic8800_runtime_config *);
/* Privileged transport integration. No concurrent calls or application command
 * interface. One step invokes one bounded platform callback, computes one SHA-256
 * block, or encodes one fixed binding request. A second binding indication remains
 * unrelated; it never replaces an owned challenge. Binding has its own deadline.
 */
int aic8800_runtime_claim(struct aic8800_runtime *, const void *owner, uint64_t now,
                          const uint8_t *early_challenge);
void aic8800_runtime_observe(struct aic8800_runtime *, const void *owner, uint64_t now);
void aic8800_runtime_step(struct aic8800_runtime *, const void *owner, uint64_t now);
int aic8800_runtime_sent(struct aic8800_runtime *, const void *owner);
enum aic8800_codec_result aic8800_runtime_response(struct aic8800_runtime *,
    const void *owner, const void *input, size_t available, uint64_t now,
    struct aic8800_sdio_response *output);
void aic8800_runtime_stop(struct aic8800_runtime *, const void *owner,
                         enum aic8800_runtime_error);
#endif
