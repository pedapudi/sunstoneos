#ifndef SUNSTONE_A733_RADIO_RESULT_H
#define SUNSTONE_A733_RADIO_RESULT_H
#include "a733_radio_platform.h"
/* The radio service's result: the stage it reached and the facts observed on
 * the way. The service keeps a report object; its scan list is allocated from
 * memory and grows with what the scan finds. A report encodes to a
 * little-endian record that begins with a fixed head, which gives the total
 * length, followed by one entry per access point. A host decoder
 * (tools/radio_result.py) reads the length from the head. Every field is a
 * copy; the record grants nothing. A later stage implies every earlier one. */
enum a733_radio_stage {
    A733_RADIO_STAGE_NONE, A733_RADIO_STAGE_RAILS_OBSERVED, A733_RADIO_STAGE_GUARD_PASSED,
    A733_RADIO_STAGE_POWERED, A733_RADIO_STAGE_ENUMERATED, A733_RADIO_STAGE_FIRMWARE,
    A733_RADIO_STAGE_IDENTIFIED, A733_RADIO_STAGE_SCANNED, A733_RADIO_STAGE_ASSOCIATED
};
#define A733_RADIO_RESULT_MAGIC UINT64_C(0x3152444152333337) /* bytes "733RADR1" */
#define A733_RADIO_RESULT_VERSION 2u
/* Four supplies are observed (DCDC1, DCDC8, BLDO5, CLDO1); SDIO defines
 * function numbers 0 to 7. Both are protocol or topology counts, not caps. */
#define A733_RADIO_RESULT_RAILS 4u
#define A733_RADIO_RESULT_FUNCTIONS 8u
struct a733_radio_result_rail { uint8_t rail, enable, selector, flags; uint32_t programmed_uv; }; /* flags: bit0 enabled, bit1 decoded */
struct a733_radio_result_function { uint16_t manufacturer, device; uint8_t class_code, present, pad[2]; };
/* frequency_mhz and channel are as reported; security is 0 (not parsed). */
struct a733_radio_result_ap { uint8_t bssid[6], ssid_bytes; int8_t rssi_dbm; uint16_t frequency_mhz; uint8_t channel, security, ssid[32]; };
struct a733_radio_result_head {
    uint64_t magic;
    uint32_t version, bytes, head_bytes, ap_entry_bytes;
    uint32_t stage, failed, failure_stage, flags; /* flags: bit0 guard asked, bit1 guard observed, bit2 BLDO5 needs power-on (the rails showed it off; the writes ran only once the stage reaches powered), bit3 shut down with every readback matched, bit4 BLDO5 was already on (guard
 * accepted it), bit5 the failure shutdown ran, bit6 every step of it that applied read back matched, bit7 the first-run guard refused before any write (fail_site then holds 0x400 | PG bits << 4 | unmet mask, shifted into the line field), bit8 the failure left nothing powered (a refusal, or a failure shutdown that finished with every readback matched) and ended without a reset */
    uint32_t pad_power[4];                         /* GPIO_POW_MOD_SEL, GPIO_POW_VAL, SGPIO_POW_MOD_SEL, SGPIO_POW_VAL */
    struct a733_radio_result_rail rails[A733_RADIO_RESULT_RAILS];
    uint32_t rail_count, platform_stage, ocr, selected_ocr;
    uint32_t rca, function_count, cccr_revision, sdio_revision;
    uint32_t sd_revision, capabilities, wide, card_hz;
    struct a733_radio_result_function functions[A733_RADIO_RESULT_FUNCTIONS];
    /* firmware_flags: bits 3:0 the engine state (enum aic8800_firmware_state), 7:4 its error, 15:8 its phase (0 adid,
     * 1 patch, 2 table, 3 Wi-Fi image, 4 read pointers, 5 Wi-Fi patch, 6 start), 31:16 the clock reading at the last
     * confirmation in milliseconds, modulo 65536. firmware_progress: bits 18:0 bytes of the current image sent, 30:19
     * Bluetooth table pairs written in all. Both are zero before the engine has claimed the module. */
    uint32_t chip_word, subrevision_word, firmware_flags, fail_site; /* fail_site: source line << 16 | stage of the first platform failure */
    uint32_t ap_count, aps_reported, aps_dropped, firmware_progress; /* dropped: results not kept because memory ran out */
};
_Static_assert(sizeof(struct a733_radio_result_rail)==8,"rail record");
_Static_assert(sizeof(struct a733_radio_result_function)==8,"function record");
_Static_assert(sizeof(struct a733_radio_result_ap)==44,"AP record");
_Static_assert(sizeof(struct a733_radio_result_head)%4==0,"head alignment");
/* Memory comes from the caller: the radio build supplies the FreeRTOS heap. */
struct a733_radio_allocator {
    void *(*allocate)(void *context,size_t bytes);
    void (*release)(void *context,void *memory);
    void *context;
};
struct a733_radio_report {
    struct a733_radio_result_head head;
    struct a733_radio_result_ap *aps;
    size_t ap_count,ap_capacity;
    struct a733_radio_allocator memory;
};
/* Zero storage. Returns 0 if the allocator is incomplete. */
int a733_radio_report_init(struct a733_radio_report *,const struct a733_radio_allocator *);
void a733_radio_report_release(struct a733_radio_report *);
/* Fill the platform's part: rails, guard words, stage reached through
 * enumeration, and the card's identity. Later stages keep what a caller has
 * already set. Copies only; no hardware access. */
void a733_radio_report_platform(struct a733_radio_report *,const struct a733_radio_platform *);
/* The session's failure site, when the platform decided none (its fail_site is zero): source line << 16 | 0x100 |
 * stage. Call after a733_radio_report_platform. */
void a733_radio_report_session_failure(struct a733_radio_report *,unsigned line,unsigned stage);
void a733_radio_report_firmware(struct a733_radio_report *);
struct aic8800_firmware;
void a733_radio_report_firmware_progress(struct a733_radio_report *,const struct aic8800_firmware *,uint64_t ticks_per_ms);
void a733_radio_report_identity(struct a733_radio_report *,uint32_t chip_word,uint32_t subrevision_word);
/* One scan result; the list keeps one entry per BSSID (the latest sighting) and
 * grows as needed. When memory runs out the result is counted in aps_dropped. */
void a733_radio_report_ap(struct a733_radio_report *,const uint8_t bssid[6],const uint8_t *ssid,unsigned ssid_bytes,
    uint16_t frequency_mhz,int8_t rssi_dbm);
void a733_radio_report_scan_done(struct a733_radio_report *,unsigned reported);
/* Length of the encoded record, and the encoding. encode returns the bytes
 * written, or 0 when capacity is smaller than the length. */
size_t a733_radio_report_bytes(const struct a733_radio_report *);
size_t a733_radio_report_encode(const struct a733_radio_report *,void *out,size_t capacity);
#endif
