/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_RADIO_SDIO_ID_PROFILE_H
#define SUNSTONE_RADIO_SDIO_ID_PROFILE_H
#include "a733_radio_platform.h"
#include "aic8800_sdio_transport.h"
#define RADIO_ID_MAGIC UINT64_C(0x524144494f494431)
#define RADIO_ID_OWNER 7u
/* Immutable producer input. Zero authority is the refusal image; it cannot
 * initialize providers. The producer retains the board/lifetime evidence. */
struct radio_id_profile {
    uint64_t magic, authority;
    uint32_t four_bit, reserved;
    struct a733_pin_latch_handoff pm_latch;
    struct a733_radio_platform_config platform;
    struct aic8800_transport_config transport;
    struct aic8800_identity_admission bootstrap;
};
extern const struct radio_id_profile radio_id_profile;
struct radio_id_result {
    uint64_t magic, authority, outcome, polls, transport_error, host_error;
    uint64_t host_status, host_irq, host_bytes, wide, card_hz, transfers;
    uint64_t chip, subrevision, elapsed_us, retained;
};
#endif
