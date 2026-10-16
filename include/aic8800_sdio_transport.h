#ifndef SUNSTONE_AIC8800_SDIO_TRANSPORT_H
#define SUNSTONE_AIC8800_SDIO_TRANSPORT_H
#include "sdio_card.h"
#include "aic8800_sdio_codec.h"

#define AIC8800_TRANSPORT_RX_BYTES 4096u
#define AIC8800_TRANSPORT_TX_BYTES 512u

enum aic8800_transport_variant { AIC8800_TRANSPORT_D80_BOOTSTRAP = 1 };
enum aic8800_transport_pad { AIC8800_PAD_KEEP = 1, AIC8800_PAD_WRITE_80 };
enum aic8800_transport_state { AIC8800_TRANSPORT_NEW, AIC8800_TRANSPORT_SETUP,
    AIC8800_TRANSPORT_READY, AIC8800_TRANSPORT_IDENTITY,
    AIC8800_TRANSPORT_COMPLETE, AIC8800_TRANSPORT_FAULT };
enum aic8800_transport_error { AIC8800_TRANSPORT_NO_ERROR,
    AIC8800_TRANSPORT_HOST_ERROR, AIC8800_TRANSPORT_TIMEOUT,
    AIC8800_TRANSPORT_SETUP_ERROR, AIC8800_TRANSPORT_RX_BOUNDS,
    AIC8800_TRANSPORT_UNSUPPORTED_IRQ, AIC8800_TRANSPORT_MESSAGE_ERROR };
struct aic8800_transport_config {
    enum aic8800_transport_variant variant; /* Explicit source protocol choice. */
    enum aic8800_transport_pad pad; /* Source's conditional function-0 F1 write. */
    uint64_t ticks_per_ms, command_timeout, poll_interval;
};
struct aic8800_identity_admission {
    /* Trusted caller evidence for an earlier, separately admitted and completed
     * radio-memory write in the current power/reset lifetime. The transport cannot
     * verify this evidence. An opaque nonzero record identifies the caller's retained evidence. Address/value
     * must be 0x40500058/0x40. No such write is encoded or executed here.
     * For PAD_WRITE_80, evidence also covers the source's subsequent function-0
     * F1=0 restore and >=1 ms settling before these reads. Caller attests awake
     * bootstrap state, no outstanding debug reads or stale confirmations, and
     * continuous exclusive ownership across prior setup and this transaction.
     * A reset, power change, or ownership loss invalidates admission; dispose
     * externally after quiescence, never clear/reinitialize an owned object.
     */
    uint64_t record;
    uint32_t address, value;
    uint8_t awake, no_outstanding_debug_reads;
};
struct aic8800_sdio_transport {
    struct sdio_card *card;
    struct aic8800_transport_config config;
    struct aic8800_identity_admission admission;
    struct sunxi_sdio_result last_result;
    struct aic8800_sdio_response confirmations[2], unrelated;
    struct aic8800_radio_identity identity;
    enum aic8800_transport_state state;
    enum aic8800_transport_error error;
    enum aic8800_codec_result codec_result;
    uint64_t deadline, wait_until, enable_deadline;
    size_t rx_bytes, cursor;
    uint16_t prior_block_bytes;
    uint8_t initialized, entered, pending, phase, expected, have[2], held;
    uint8_t prior_enable, prior_irq, prior_function_irq, raw_rx_status, byte_length;
    uint8_t tx[AIC8800_TRANSPORT_TX_BYTES], rx[AIC8800_TRANSPORT_RX_BYTES];
};

/* Single serialized owner, no reentry/concurrent ISR. Storage must be zero
 * before first use; later init is refused. Config/card/host/transport must not
 * overlap. Caller owns the entire card/host and retains them, callback context,
 * and all buffers until external disposal, including after uncertain failure.
 * Context storage must not alias transport/card; its extent is caller-owned.
 * Requires real completed enumeration, c8a1:0182 function 1 and at least two
 * functions, >=512-byte block capability, multi-block support, prepared host.
 * IDs only validate compatibility with the explicit variant, never firmware.
 */
int aic8800_transport_init(struct aic8800_sdio_transport *, struct sdio_card *,
                          const struct aic8800_transport_config *);
/* Pure admission. Setup owns F1 block/enable, source pad/byte-mode settings and
 * card IRQ enable bits for functions 1/2. GIC delivery remains disabled.
 * Other IOEx/IEN bits survive read-modify-write. No automatic rollback exists.
 */
int aic8800_transport_setup(struct aic8800_sdio_transport *, uint64_t deadline);
/* Pure admission, once only. Requires READY and trusted setup evidence above.
 * Two address-correlated reads; no firmware choice/upload or memory write.
 */
int aic8800_transport_read_identity(struct aic8800_sdio_transport *,
                                  const struct aic8800_identity_admission *, uint64_t deadline);
/* At most one host step, one next admission, or one record parse per call.
 * Deadlines continue while unrelated-message backpressure blocks receive reuse.
 */
void aic8800_transport_poll(struct aic8800_sdio_transport *);
/* Borrowed view valid until consume or external disposal. Copy before retaining
 * asynchronous work. Peek works after FAULT; consume refuses in FAULT. Output
 * must not alias transport/card/host. No callbacks or hardware access occur.
 */
int aic8800_transport_peek(const struct aic8800_sdio_transport *, struct aic8800_sdio_response *);
int aic8800_transport_consume(struct aic8800_sdio_transport *);
#endif
