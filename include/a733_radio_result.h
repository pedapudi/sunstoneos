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
    uint32_t stage, failed, failure_stage, flags; /* flags: bit0 guard asked, bit1 guard observed, bit2 BLDO5 powered on */
    uint32_t pad_power[4];                         /* GPIO_POW_MOD_SEL, GPIO_POW_VAL, SGPIO_POW_MOD_SEL, SGPIO_POW_VAL */
    struct a733_radio_result_rail rails[A733_RADIO_RESULT_RAILS];
    uint32_t rail_count, platform_stage, ocr, selected_ocr;
    uint32_t rca, function_count, cccr_revision, sdio_revision;
    uint32_t sd_revision, capabilities, wide, card_hz;
    struct a733_radio_result_function functions[A733_RADIO_RESULT_FUNCTIONS];
    uint32_t chip_word, subrevision_word, firmware_flags, reserved0;
    uint32_t ap_count, aps_reported, aps_dropped, reserved1; /* dropped: results not kept because memory ran out */
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
void a733_radio_report_firmware(struct a733_radio_report *);
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
