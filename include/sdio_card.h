#ifndef SUNSTONE_SDIO_CARD_H
#define SUNSTONE_SDIO_CARD_H
#include "sunxi_sdio.h"

#define SDIO_CARD_FUNCTIONS 8u /* Common function 0 plus functions 1..7. */
#define SDIO_CARD_CIS_BYTES 4096u
#define SDIO_CARD_CIS_TUPLES 128u

enum sdio_card_state { SDIO_CARD_NEW, SDIO_CARD_ENUMERATING, SDIO_CARD_READY, SDIO_CARD_FAULT };
struct sdio_function_identity {
    uint32_t cis_address;
    uint16_t manufacturer, device, max_block_bytes;
    uint32_t enable_timeout_ms;
    uint8_t class_code, has_manufacturer, has_function_extension;
};
struct sdio_card_config {
    uint32_t supported_ocr; /* Admitted fixed supply range, bits0..23 only. */
    uint64_t command_timeout, ready_poll_interval; /* Same units as host clock. */
};
struct sdio_card {
    struct sunxi_sdio *host; /* Exclusive ownership throughout enumeration. */
    struct sdio_card_config config;
    struct sdio_function_identity functions[SDIO_CARD_FUNCTIONS];
    struct sunxi_sdio_result last_result;
    enum sdio_card_state state;
    uint64_t deadline, retry_at;
    uint32_t ocr, selected_ocr, cursor;
    uint16_t rca, cis_bytes, tuple_count, skipped_tuples;
    uint8_t function_count, cccr_revision, sdio_revision, sd_revision, capabilities;
    uint8_t phase, function, index, tuple_code, tuple_length, pending, entered, initialized;
    uint8_t tuple[254];
};

/* Pure init/admission. Card storage must be zero-initialized before first use;
 * every init after a successful init is refused. There is no disposal API.
 * Never clear/reuse storage while its host ownership or fault remains retained.
 * Config, initialized host, and card must not overlap. Enumeration uses
 * the prepared host's one-bit initialization clock and preserves that mode.
 * CMD5 supply negotiation never requests voltage switching or a faster clock.
 */
int sdio_card_init(struct sdio_card *, struct sunxi_sdio *, const struct sdio_card_config *);
int sdio_card_start(struct sdio_card *, uint64_t absolute_deadline);
/* At most one host step and one next-command admission per call. A caller must
 * not submit other host work during enumeration. Error/timeout stops the host;
 * partial identities and last_result remain inspectable and cannot be retried.
 */
void sdio_card_poll(struct sdio_card *);
/* Pure constructors, valid only after enumeration. They do not submit commands
 * or change function enable/block-size registers. RAW readback only for writes.
 * CMD53 requires caller-verified function enable and block size; max_block_bytes
 * is a capability, not evidence that the card has been programmed to that size.
 * Whole-word transfers of 4..SUNXI_SDIO_MAX_BYTES only. Byte mode <=512; block
 * count 1..511.
 * Buffers remain caller-owned until admitted by sunxi_sdio_submit.
 */
int sdio_card_cmd52(const struct sdio_card *, unsigned function, uint32_t address,
                   int write, int read_after_write, uint8_t value, struct sunxi_sdio_request *);
int sdio_card_cmd53(const struct sdio_card *, unsigned function, uint32_t address,
                   int write, int increment, uint16_t block_bytes,
                   uint8_t *data, size_t bytes, struct sunxi_sdio_request *);
#endif
