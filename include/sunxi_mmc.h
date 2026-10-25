#ifndef SUNSTONE_SUNXI_MMC_H
#define SUNSTONE_SUNXI_MMC_H
#include <stddef.h>
#include <stdint.h>

#define SUNXI_MMC_BLOCK_BYTES 512u
#define SUNXI_MMC_MAX_BLOCKS 8u
/* FIFO words moved by one bounded poll: one 512-byte block. */
#define SUNXI_MMC_FIFO_WORDS 128u
/* Explicit physical slot selection; SDC1 belongs to the radio driver. */
enum sunxi_mmc_kind { SUNXI_MMC_SD = 1, SUNXI_MMC_EMMC = 2 };
enum sunxi_mmc_sd_profile { SUNXI_MMC_SD_LEGACY=0, SUNXI_MMC_SD_DEFAULT4=1,
    SUNXI_MMC_SD_HIGH_SPEED4=2, SUNXI_MMC_SD_UHS_SDR104=3 };
/* Outcome of the UHS-I profile (card.uhs_outcome). Every value other than
 * SDR104 leaves a working high-speed card: 3.3 V when the switch never began,
 * SDR25 at 1.8 V after it completed. */
enum sunxi_mmc_uhs_outcome { SUNXI_MMC_UHS_NONE, SUNXI_MMC_UHS_SDR104,
    SUNXI_MMC_UHS_HOST, SUNXI_MMC_UHS_DECLINED, SUNXI_MMC_UHS_NO_SDR104,
    SUNXI_MMC_UHS_NO_WINDOW };
/* sd_signal operations and QUERY capability bits. */
#define SUNXI_MMC_SIGNAL_QUERY 0u
#define SUNXI_MMC_SIGNAL_SET 1u
#define SUNXI_MMC_SIGNAL_POLL 2u
#define SUNXI_MMC_SIGNAL_1V8 1u  /* Bank switch to 1.8 V is admitted. */
#define SUNXI_MMC_SIGNAL_300M 2u /* A stable 300 MHz module parent exists. */
/* sd_module divisor flag: divide the 300 MHz parent instead of 400 MHz. */
#define SUNXI_MMC_MODULE_300M 0x100u
#define SUNXI_MMC_TUNING_DELAYS 64u /* SAMP_DL_SW steps, manual 11.3.8.37. */
enum sunxi_mmc_state { SUNXI_MMC_COLD, SUNXI_MMC_IDENTIFY,
    SUNXI_MMC_READY, SUNXI_MMC_READING, SUNXI_MMC_DONE, SUNXI_MMC_FAULT,
    SUNXI_MMC_WRITING };
enum sunxi_mmc_error { SUNXI_MMC_OK, SUNXI_MMC_IO, SUNXI_MMC_TIME,
    SUNXI_MMC_TIMEOUT, SUNXI_MMC_ADMISSION, SUNXI_MMC_VARIANT,
    SUNXI_MMC_CONTROLLER, SUNXI_MMC_CARD, SUNXI_MMC_UNSUPPORTED,
    /* The CMD11 sequence failed after the card accepted it. The SD physical
     * layer requires a card power cycle; this board can only remove power
     * from the whole board. */
    SUNXI_MMC_SIGNAL };
struct sunxi_mmc_ops {
    /* Every callback is bounded, serialized and non-reentrant. Return exactly
     * 1 for success. Failed writes may have taken effect. Offsets are relative
     * to SDC0 (SD) or SDC2/SDC3 (eMMC), selected by the platform binding.
     */
    int (*read32)(void *, uint32_t, uint32_t *);
    int (*write32)(void *, uint32_t, uint32_t);
    int (*barrier)(void *);
    int (*now_us)(void *, uint64_t *);
    /* Admit native 1-bit legacy mode with cold card authority, or the first
     * read-only SD CMD0 initialization policy documented in storage-read-application.
     * The latter requires native 3.3 V signaling and no failed native SD lifetime.
     * Both require exclusive controller/card ownership, reviewed recovery, settled rails and pins, accessible reset
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
    /* SD-only negotiated profile: called with card output stopped and UPDATE
     * completed. Provider owns stable PERI0_400M and changes only the selected
     * module to 400MHz/divisor (N=1), preserving other writers. The UHS-I
     * profile may also pass divisor 2, or SUNXI_MMC_MODULE_300M|1 for the
     * PERI0_300M parent. Exactly1 completes; uncertainty is terminal. */
    int (*sd_module)(void *,uintptr_t token,unsigned divisor);
    /* Move 1..SUNXI_MMC_FIFO_WORDS words through the FIFO register in order.
     * The core sizes each burst from one STATUS sample: reads take at most the
     * reported fill level, writes at most the free space. Admission happens
     * once per burst. A failed write burst may have transferred any prefix. */
    int (*read_fifo)(void *,uint32_t *words,unsigned count);
    int (*write_fifo)(void *,const uint32_t *words,unsigned count);
    /* UHS-I profile only; NULL otherwise. QUERY stores the capability bits
     * and changes nothing. SET, called once with the card clock stopped after
     * the CMD11 response, selects 1.8 V for the card's pin bank. POLL stores 1
     * once the bank reports 1.8 V and 0 while it is still changing. Exactly 1
     * means the operation itself succeeded; uncertainty is terminal. */
    int (*sd_signal)(void *,uintptr_t token,unsigned operation,unsigned *state);
};
struct sunxi_mmc_config {
    struct sunxi_mmc_ops ops;
    void *context;
    uintptr_t token;
    enum sunxi_mmc_kind kind;
    uint32_t module_hz; /* Even 200000..800000; SDR card clock = module / 2. */
    uint32_t voltage_mask; /* Admitted 3.2–3.4 V OCR bits: nonzero subset 0x00300000. */
    enum sunxi_mmc_sd_profile sd_profile; /* SD only; zero preserves initial profile. */
};
struct sunxi_mmc_card {
    uint32_t cid[4], csd[4]; /* Card bit 127 is word 0 bit 31. */
    uint32_t ocr;
    uint64_t sectors; /* Count of 512-byte blocks; 2^32 is representable. */
    uint16_t rca;
    uint8_t sector_addressed, ext_revision;
    uint8_t scr[8],switch_status[64]; /* Raw wire byte order, SD profiles only. */
    uint32_t bus_width, timing, module_hz, card_hz, module_divisor;
    /* Configured rates are floor(parent/divisor) and floor(parent/(2*divisor)),
     * where parent is 400 MHz, or 300 MHz with SUNXI_MMC_MODULE_300M; these
     * decode register configuration and do not measure frequency. timing is
     * the CMD6 access mode: 0 default, 1 high speed or SDR25, 3 SDR104. */
    uint8_t signal_1v8, uhs_outcome, uhs_caps, sample_delay, tuning_start, tuning_width;
    uint64_t tuning_map; /* Bit d set: every CMD19 at sample delay d matched. */
};
struct sunxi_mmc_result {
    enum sunxi_mmc_error error;
    uint32_t interrupts, status, response, stop_response;
    size_t received; /* Diagnostic only unless state is DONE and error is OK. */
    size_t submitted; /* FIFO bytes attempted; never evidence of media contents. */
    uint32_t confirmed_blocks; /* CMD13 reported READY+TRAN after these writes. */
    unsigned write_started; /* Sticky before attempting the first CMD24 write. */
};
struct sunxi_mmc_write_request {
    uintptr_t policy_token;
    uint64_t first_sector;
    uint32_t blocks;
    const uint8_t *source;
    size_t bytes;
    uint64_t deadline_us;
};
struct sunxi_mmc_write_identity {
    uintptr_t core, session;
    enum sunxi_mmc_kind kind;
    struct sunxi_mmc_card card;
};
struct sunxi_mmc_write_authority {
    void *context;
    /* Exactly 1 authorizes this exact immutable request for this actual card
     * session. Bounded, non-reentrant; temporary arguments must not be retained
     * or changed. The provider must own the range and enforce qualified recovery,
     * durable backup and physical write-protect policy. The concrete storage update service binds this policy to actual backup reads,
     * immutable range ownership and trusted host artifacts. An arbitrary callback
     * is not evidence of that authority. */
    int (*admit)(void *,const struct sunxi_mmc_write_identity *,
                 const struct sunxi_mmc_write_request *);
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
    uint8_t read_tail; /* Open-ended CMD18 ending at the last sector. */
    struct sunxi_mmc_write_request write_request;
    struct sunxi_mmc_write_identity write_identity;
    struct sunxi_mmc_write_authority write_authority;
    uint8_t write_payload[SUNXI_MMC_MAX_BLOCKS * SUNXI_MMC_BLOCK_BYTES];
    unsigned writing;
    unsigned sd_ios;
    /* UHS-I switch and tuning progress. */
    uint64_t signal_us;
    unsigned sd_sig, tune_delay, tune_repeat, tune_recover;
    uint8_t s18r, signal_window, tuning, tune_failed, after_clock;
    uint8_t tuning_block[64];
};
/* Zero storage before its first use. Single task owner; no concurrent ISR or
 * callback access. Config, context and callback-owned storage must not overlap
 * the core or read buffer, and must outlive all retained leases. Initialization
 * is pure and permanent. No disposal, hotplug recovery, erase, partition
 * switching or generic command escape exists. External recovery owns faults.
 */
int sunxi_mmc_init(struct sunxi_mmc *, const struct sunxi_mmc_config *);
int sunxi_mmc_open(struct sunxi_mmc *, uint64_t absolute_deadline_us);
/* Entire range and byte/sector command argument are validated before MMIO.
 * Borrowed storage must remain exclusive through DONE or terminal FAULT.
 */
int sunxi_mmc_read(struct sunxi_mmc *, uint64_t first_sector, uint32_t blocks,
                   uint8_t *buffer, size_t bytes, uint64_t absolute_deadline_us);
/* Low-level, default-denied write primitive, below boot/update policy. A missing
 * authority refuses without callbacks or MMIO. Accepted input is copied into a
 * fixed 4096-byte private snapshot. The original source stays borrowed until
 * successful take; a fault retains it and every lease. Source and callback-owned
 * storage must stay exclusive and disjoint from the core. Request and authority
 * descriptors are copied; the caller may reuse those descriptors after return.
 * Writes use CMD24 separately for each sector in the active user area. No atomic
 * multi-sector, power-loss durability, rollback or readback claim is made.
 * Default native platform APIs and register filters remain read-only; the
 * separate concrete storage service may bind an authenticated writer. */
int sunxi_mmc_write(struct sunxi_mmc *,const struct sunxi_mmc_write_request *,
                    const struct sunxi_mmc_write_authority *);
void sunxi_mmc_poll(struct sunxi_mmc *);
/* DONE returns to READY and returns the complete buffer to its owner. FAULT
 * copies diagnostics but keeps its buffer and all platform leases retained.
 * out must not overlap the core or borrowed buffer. */
int sunxi_mmc_take(struct sunxi_mmc *, struct sunxi_mmc_result *out);
/* Pure CSD/EXT_CSD capacity decoder. Output is unchanged on refusal; inputs and
 * output must not overlap. EXT_CSD is required only for eMMC. */
int sunxi_mmc_capacity(enum sunxi_mmc_kind, const uint32_t csd[4], uint32_t ocr,
                       const uint8_t ext_csd[512], uint64_t *sectors);
#endif
