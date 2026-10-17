#ifndef SUNSTONE_SUNXI_MMC_H
#define SUNSTONE_SUNXI_MMC_H
#include <stddef.h>
#include <stdint.h>

#define SUNXI_MMC_BLOCK_BYTES 512u
#define SUNXI_MMC_MAX_BLOCKS 8u
/* Explicit physical slot selection; SDC1 belongs to the radio driver. */
enum sunxi_mmc_kind { SUNXI_MMC_SD = 1, SUNXI_MMC_EMMC = 2 };
enum sunxi_mmc_state { SUNXI_MMC_COLD, SUNXI_MMC_IDENTIFY,
    SUNXI_MMC_READY, SUNXI_MMC_READING, SUNXI_MMC_DONE, SUNXI_MMC_FAULT };
enum sunxi_mmc_error { SUNXI_MMC_OK, SUNXI_MMC_IO, SUNXI_MMC_TIME,
    SUNXI_MMC_TIMEOUT, SUNXI_MMC_ADMISSION, SUNXI_MMC_VARIANT,
    SUNXI_MMC_CONTROLLER, SUNXI_MMC_CARD, SUNXI_MMC_UNSUPPORTED };
struct sunxi_mmc_ops {
    /* Every callback is bounded, serialized and non-reentrant. Return exactly
     * 1 for success. Failed writes may have taken effect. Offsets are relative
     * to SDC0 (SD) or SDC2/SDC3 (eMMC), selected by the platform binding.
     */
    int (*read32)(void *, uint32_t, uint32_t *);
    int (*write32)(void *, uint32_t, uint32_t);
    int (*barrier)(void *);
    int (*now_us)(void *, uint64_t *);
    /* Admit a cold card in native 1-bit legacy mode, exclusive controller/card
     * ownership, independent recovery, settled rails and pins, accessible reset
     * release and measured module rate. The command queue must be disabled and drained. No outstanding DMA or
     * command may exist. Remaining controller controls must have known reset
     * defaults, including disabled encryption and extended-command modes.
     * SD presence must come from its board card-detect pin. eMMC presence and
     * population require independent evidence; a timeout is not absence.
     * Retain every lease, including after errors. token is caller-owned opaque
     * storage; callbacks must authenticate the configured owner and aperture.
     */
    int (*admit)(void *, uintptr_t token, enum sunxi_mmc_kind, uint32_t module_hz);
    /* Revalidate retained ownership and physical presence before every poll.
     * This must not access an inaccessible/unpowered controller. A failure
     * freezes the core and retains ownership; it does not release any lease.
     */
    int (*retained)(void *, uintptr_t token);
};
struct sunxi_mmc_config {
    struct sunxi_mmc_ops ops;
    void *context;
    uintptr_t token;
    enum sunxi_mmc_kind kind;
    uint32_t module_hz; /* Even 200000..800000; SDR card clock = module / 2. */
    uint32_t voltage_mask; /* Admitted 3.2–3.4 V OCR bits: nonzero subset 0x00300000. */
};
struct sunxi_mmc_card {
    uint32_t cid[4], csd[4]; /* Card bit 127 is word 0 bit 31. */
    uint32_t ocr;
    uint64_t sectors; /* Count of 512-byte blocks; 2^32 is representable. */
    uint16_t rca;
    uint8_t sector_addressed, ext_revision;
};
struct sunxi_mmc_result {
    enum sunxi_mmc_error error;
    uint32_t interrupts, status, response, stop_response;
    size_t received; /* Diagnostic only unless state is DONE and error is OK. */
};
struct sunxi_mmc {
    struct sunxi_mmc_config config;
    struct sunxi_mmc_card card;
    struct sunxi_mmc_result result;
    enum sunxi_mmc_state state;
    uint64_t last_us, deadline_us, retry_us;
    uint32_t version, response[4], command, argument, seen;
    uint8_t ext_csd[512];
    uint8_t *data, *borrowed;
    size_t data_bytes, position, borrowed_bytes;
    uint16_t setup, protocol;
    uint8_t command_step, response_seen, entered, initialized, admitted;
};
/* Zero storage before its first use. Single task owner; no concurrent ISR or
 * callback access. Config, context and callback-owned storage must not overlap
 * the core or read buffer, and must outlive all retained leases. Initialization
 * is pure and permanent. No disposal, hotplug recovery, writes, erase, partition
 * switching or generic command escape exists. External recovery owns faults.
 */
int sunxi_mmc_init(struct sunxi_mmc *, const struct sunxi_mmc_config *);
int sunxi_mmc_open(struct sunxi_mmc *, uint64_t absolute_deadline_us);
/* Entire range and byte/sector command argument are validated before MMIO.
 * Borrowed storage must remain exclusive through DONE or terminal FAULT.
 */
int sunxi_mmc_read(struct sunxi_mmc *, uint64_t first_sector, uint32_t blocks,
                   uint8_t *buffer, size_t bytes, uint64_t absolute_deadline_us);
void sunxi_mmc_poll(struct sunxi_mmc *);
/* DONE returns to READY and returns the complete buffer to its owner. FAULT
 * copies diagnostics but keeps its buffer and all platform leases retained.
 * out must not overlap the core or read buffer. */
int sunxi_mmc_take(struct sunxi_mmc *, struct sunxi_mmc_result *out);
/* Pure CSD/EXT_CSD capacity decoder. Output is unchanged on refusal; inputs and
 * output must not overlap. EXT_CSD is required only for eMMC. */
int sunxi_mmc_capacity(enum sunxi_mmc_kind, const uint32_t csd[4], uint32_t ocr,
                       const uint8_t ext_csd[512], uint64_t *sectors);
#endif
