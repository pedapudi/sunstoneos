#ifndef SUNSTONE_AIC8800_SDIO_TRANSPORT_H
#define SUNSTONE_AIC8800_SDIO_TRANSPORT_H
#include "sdio_card.h"
#include "aic8800_sdio_codec.h"

#define AIC8800_TRANSPORT_RX_BYTES 4096u
#define AIC8800_TRANSPORT_TX_BYTES 1536u
struct aic8800_firmware;
struct aic8800_runtime;
struct aic8800_wifi;
struct aic8800_data;
struct aic8800_security;
struct aic8800_supplicant;
struct aic8800_data_result;
struct aic8800_data_frame;
struct aic8800_wifi_scan_result;
struct aic8800_wifi_target;
struct aic8800_wifi_station_event;
struct aic8800_wifi_scan_summary;

enum aic8800_transport_variant { AIC8800_TRANSPORT_D80_BOOTSTRAP = 1 };
enum aic8800_transport_pad { AIC8800_PAD_KEEP = 1, AIC8800_PAD_WRITE_80 };
enum aic8800_transport_state { AIC8800_TRANSPORT_NEW, AIC8800_TRANSPORT_SETUP,
    AIC8800_TRANSPORT_READY, AIC8800_TRANSPORT_IDENTITY,
    AIC8800_TRANSPORT_COMPLETE, AIC8800_TRANSPORT_FAULT,
    AIC8800_TRANSPORT_FIRMWARE, AIC8800_TRANSPORT_START_CONFIRMED,
    AIC8800_TRANSPORT_RUNTIME };
enum aic8800_transport_error { AIC8800_TRANSPORT_NO_ERROR,
    AIC8800_TRANSPORT_HOST_ERROR, AIC8800_TRANSPORT_TIMEOUT,
    AIC8800_TRANSPORT_SETUP_ERROR, AIC8800_TRANSPORT_RX_BOUNDS,
    AIC8800_TRANSPORT_UNSUPPORTED_IRQ, AIC8800_TRANSPORT_MESSAGE_ERROR,
    AIC8800_TRANSPORT_FIRMWARE_ERROR, AIC8800_TRANSPORT_RUNTIME_ERROR, AIC8800_TRANSPORT_WIFI_ERROR, AIC8800_TRANSPORT_DATA_ERROR };
struct aic8800_transport_config {
    enum aic8800_transport_variant variant; /* Explicit source protocol choice. */
    enum aic8800_transport_pad pad; /* Source's conditional function-0 F1 write. */
    uint64_t ticks_per_ms, command_timeout, poll_interval;
};
struct aic8800_identity_admission {
    /* Explicit authorization for the fixed bootstrap write, not evidence that
     * it already happened. The nonzero record identifies the caller's admission;
     * address/value must be 0x40500058/0x40. The transport performs this write,
     * conditional pad restore/settle, then both identity reads.
     * Caller attests awake bootstrap state, no outstanding debug operations or
     * stale confirmations, and continuous exclusive ownership for this lifetime.
     * A reset, power change, or ownership loss invalidates admission; dispose
     * externally after quiescence, never clear/reinitialize an owned object.
     */
    uint64_t record;
    uint32_t address, value;
    uint8_t awake, no_outstanding_debug_operations;
};
struct aic8800_sdio_transport {
    struct sdio_card *card;
    struct aic8800_firmware *firmware;
    struct aic8800_runtime *runtime;
    struct aic8800_wifi *wifi;
    struct aic8800_data *data;
    struct aic8800_security *security;
    struct aic8800_supplicant *supplicant;
    struct aic8800_transport_config config;
    struct aic8800_identity_admission admission;
    struct sunxi_sdio_result last_result;
    struct aic8800_sdio_response confirmations[2], unrelated, setup_confirmation;
    struct aic8800_radio_identity identity;
    enum aic8800_transport_state state;
    enum aic8800_transport_error error;
    enum aic8800_codec_result codec_result;
    uint64_t deadline, wait_until, enable_deadline;
    size_t rx_bytes, cursor, tx_bytes;
    uint16_t prior_block_bytes;
    uint8_t data_tx, security_tx;
    uint8_t initialized, entered, pending, phase, expected, have[2], held, setup_confirmed;
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
/* Pure admission, once only. Requires READY and explicit authorization above.
 * Poll executes the fixed memory write and conditional F0 F1=0 pad restore,
 * waits >=1 ms after restoring, then performs two address-correlated reads.
 * Write completion is an exact-layout CFM matched by ID, not echoed field values.
 * No firmware choice/upload or arbitrary memory-write API.
 */
int aic8800_transport_read_identity(struct aic8800_sdio_transport *,
                                  const struct aic8800_identity_admission *, uint64_t deadline);
/* Privileged firmware engine handoff after COMPLETE identity, once only, under
 * the same clean, awake, exclusive bootstrap lifetime. No raw command API is
 * exposed. Engine and immutable borrowed blobs cannot alias transport/card/host.
 * Both objects retain ownership through START_CONFIRMED or fault; this state
 * is not normal Wi-Fi/HCI readiness. Binding events remain consumer-owned work.
 */
int aic8800_transport_load_firmware(struct aic8800_sdio_transport *,
                                  struct aic8800_firmware *, uint64_t deadline);
/* Attach a zero-initialized runtime after START_CONFIRMED under uninterrupted
 * power/reset and exclusive ownership. Imports any retained early challenge.
 * The lease deadline bounds ongoing receive, including consumer backpressure.
 * There are no arbitrary application commands or implicit Wi-Fi/HCI readiness.
 */
int aic8800_transport_start_runtime(struct aic8800_sdio_transport *,
                                  struct aic8800_runtime *, uint64_t deadline);
/* Lease extension with one bounded clock observation, only between aggregates,
 * with no pending controller request, held record, or unfinished binding action.
 * Caller reaffirms continued awake platform ownership. Expired leases cannot be
 * revived: renewal checks the old deadline against the shared clock.
 */
int aic8800_transport_renew_runtime(struct aic8800_sdio_transport *, uint64_t deadline);
/* Normal D80 management initialization, then separately admitted passive scan.
 * Both admissions require idle runtime ownership and a deadline within its lease.
 * A late binding challenge runs before the next Wi-Fi command; an outstanding
 * Wi-Fi confirmation is drained first. No RF permission follows from these APIs.
 */
int aic8800_transport_start_wifi(struct aic8800_sdio_transport *, struct aic8800_wifi *, uint64_t deadline);
int aic8800_transport_scan(struct aic8800_sdio_transport *, uint64_t deadline);
/* Active association requires independent caller authority and observed-beacon
 * evidence in the copied target. No credentials or network-ready state exist.
 * Command acceptance, association and security completion are distinct. */
int aic8800_transport_connect(struct aic8800_sdio_transport *, const struct aic8800_wifi_target *, uint64_t deadline);
int aic8800_transport_disconnect(struct aic8800_sdio_transport *, uint16_t reason, uint64_t deadline);
/* Event/payload view remains valid until generic consume, which acknowledges
 * ownership only. It does not authenticate WPA or authorize network data. */
int aic8800_transport_peek_station(const struct aic8800_sdio_transport *, struct aic8800_wifi_station_event *);
/* Scan view is valid until generic consume. Copy before asynchronous use.
 * Summary take acknowledges completion and permits the next scan; it requires
 * a drained aggregate. No request/result is overwritten without consumption. */
int aic8800_transport_peek_scan(const struct aic8800_sdio_transport *, struct aic8800_wifi_scan_result *);
int aic8800_transport_take_scan(struct aic8800_sdio_transport *, struct aic8800_wifi_scan_summary *);
/* Attach a fresh packet worker to the same serialized runtime. No network
 * readiness follows. Open-port admission requires an explicit caller record and
 * lasts only for the current open association. WPA remains EAPOL-only.
 * Send copies a 14..1514-byte Ethernet frame. One TX and one RX are retained;
 * ordinary TX completion means bus transfer only. EAPOL additionally waits for
 * firmware status; its ACK bit is reported separately from transfer completion.
 * Pending TX excludes connect/disconnect/lease renewal. Unsolicited link loss
 * faults the owner and retains uncertain TX. No automatic retry/disposal.
 */
int aic8800_transport_start_data(struct aic8800_sdio_transport *, struct aic8800_data *);
int aic8800_transport_authorize_open_data(struct aic8800_sdio_transport *, uint64_t record);
int aic8800_transport_send_data(struct aic8800_sdio_transport *, const uint8_t *ethernet, size_t, uint64_t deadline);
int aic8800_transport_take_data_result(struct aic8800_sdio_transport *, struct aic8800_data_result *);
/* Copy RX before generic consume; generation identifies its association. This
 * view remains inspectable after FAULT, but grants no current send authority. */
int aic8800_transport_peek_data(const struct aic8800_sdio_transport *, struct aic8800_data_frame *);
#ifdef SUNSTONE_AIC8800_RSN
/* Real BSD RSN adapter build only. One initialized security worker and supplicant
 * attach to an associated data owner. PMK/observed RSN/entropy are supplied via
 * supplicant init; no generic key or secure-port API is exposed. Explicit
 * retirement confirms port close and all deletes before detachment. */
int aic8800_transport_start_security(struct aic8800_sdio_transport *,struct aic8800_security *,struct aic8800_supplicant *,uint64_t);
int aic8800_transport_retire_security(struct aic8800_sdio_transport *,uint64_t);
int aic8800_transport_finish_security(struct aic8800_sdio_transport *);
#endif
/* At most one host step, one next admission, or one record parse per call.
 * Deadlines continue while unrelated-message backpressure blocks receive reuse.
 */
void aic8800_transport_poll(struct aic8800_sdio_transport *);
/* Borrowed view valid until consume or external disposal. Copy before retaining
 * asynchronous work. Peek works after FAULT; consume refuses in FAULT. Output
 * must not alias transport/card/host, runtime, Wi-Fi worker, firmware or immutable blobs. No callbacks or hardware access occur.
 */
int aic8800_transport_peek(const struct aic8800_sdio_transport *, struct aic8800_sdio_response *);
int aic8800_transport_consume(struct aic8800_sdio_transport *);
#endif
