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
    /* The numbers are stored in records and are not the order: identified comes before firmware. */
    A733_RADIO_STAGE_POWERED, A733_RADIO_STAGE_ENUMERATED, A733_RADIO_STAGE_FIRMWARE,
    A733_RADIO_STAGE_IDENTIFIED, A733_RADIO_STAGE_SCANNED, A733_RADIO_STAGE_ASSOCIATED
};
#define A733_RADIO_RESULT_MAGIC UINT64_C(0x3152444152333337) /* bytes "733RADR1" */
/* Version 3 adds the scan counts (head flags bit 10) and the RSN summary (bit 11). The image exports the version it
 * writes as os_radio_result_version, and admission refuses an image whose version the runner's decoder lacks. */
#define A733_RADIO_RESULT_VERSION 3u
/* Four supplies are observed (DCDC1, DCDC8, BLDO5, CLDO1); SDIO defines
 * function numbers 0 to 7. Both are protocol or topology counts, not caps. */
#define A733_RADIO_RESULT_RAILS 4u
#define A733_RADIO_RESULT_FUNCTIONS 8u
struct a733_radio_result_rail { uint8_t rail, enable, selector, flags; uint32_t programmed_uv; }; /* flags: bit0 enabled, bit1 decoded */
struct a733_radio_result_function { uint16_t manufacturer, device; uint8_t class_code, present, pad[2]; };
/* frequency_mhz and channel are as reported; security is 0 (not parsed). ssid_bytes and ssid are always 0: the
 * layout keeps the fields so that records already captured still decode. */
struct a733_radio_result_ap { uint8_t bssid[6], ssid_bytes; int8_t rssi_dbm; uint16_t frequency_mhz; uint8_t channel, security, ssid[32]; };
struct a733_radio_result_head {
    uint64_t magic;
    uint32_t version, bytes, head_bytes, ap_entry_bytes;
    uint32_t stage, failed, failure_stage, flags; /* flags: bit0 guard asked, bit1 guard observed, bit2 BLDO5 needs power-on (the rails showed it off; the writes ran only once the stage reaches powered), bit3 the orderly shutdown reached down with every step read back (a failure runs the failure shutdown of bits 5 and 6 instead, so a failed run leaves it clear), bit4 BLDO5 was already on (guard
 * accepted it), bit5 the failure shutdown ran, bit6 every step of it that applied read back matched, bit7 the first-run guard refused before any write (fail_site then holds 0x400 | PG bits << 4 | unmet mask, shifted into the line field), bit9 an association tail follows the access point list (struct a733_radio_assoc), bit8 the failure left nothing powered (a refusal, or a failure shutdown that finished with every readback matched) and ended without a reset */
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
/* The association stage's progress, appended after the access point list when head flags bit 9 is set (the record's
 * length covers it). It holds counts, codes and times only: no SSID, BSSID, key or credential byte.
 * stage: 0 none, 1 candidate found, 2 entropy ready, 3 connecting, 4 security pending, 5 securing, 6 authorized.
 * link: enum aic8800_wifi_link. status: the association status code of the station event. supplicant, rsn: enum
 * aic8800_supplicant_state and the WPA state machine's state. eapol_rx and eapol_tx: EAPOL frames received from and sent
 * to the access point (the four-way handshake is received 1 and 3, sent 2 and 4). flags: bit 0 entropy unqualified (bring-up
 * only), 1 a scan result carried the credential's SSID, 2 its RSN element offers PSK and CCMP, 3 a target was built, 4 the CE
 * entropy reservoir was ready, 5 the credential was present at EL1, 6 the CE entropy source failed or is missing. candidates: scan results with the credential's SSID.
 * frequency_rssi: low 16 bits the matched access point's frequency in MHz, high 16 bits its RSSI in dBm as int16.
 * ms_connect and ms_authorized: the clock reading in milliseconds, modulo 65536, when connect started and when the
 * supplicant authorized. */
struct a733_radio_assoc {
    uint32_t stage, link, status, supplicant, rsn, eapol_rx, eapol_tx, flags, candidates, frequency_rssi, ms_connect, ms_authorized;
};
_Static_assert(sizeof(struct a733_radio_assoc)==48,"association tail");
/* The scan's counts, appended after the access point list and before any association tail when head flags bit 10 is
 * set. sightings: scan results delivered as events. firmware_reported: the count in the firmware's scan-end event
 * (SCANU_START_CFM result_cnt, which the vendor header describes as "Number of scan results available"). merges:
 * sightings of a BSSID already listed, which replace its entry. set_aside: scan result records that the dispatcher
 * retired without an event (another interface, or after the scan-end event). The listed count is sightings - merges -
 * dropped. */
struct a733_radio_scan_counts { uint32_t sightings, firmware_reported, merges, set_aside; };
_Static_assert(sizeof(struct a733_radio_scan_counts)==16,"scan counts tail");
/* The RSN element of the scan result the association stage judged: the passing candidate, or while none passes, the
 * strongest result with the credential's SSID. Appended after the association tail when head flags bit 11 is set. It
 * holds security capabilities only: no address, SSID or key.
 * check: why the candidate test refused the element (enum a733_radio_rsn_check). group: the group cipher's suite
 * type under OUI 00-0F-AC, A733_RADIO_RSN_SUITE_OTHER for another OUI, A733_RADIO_RSN_SUITE_ABSENT when the element
 * ends before it. flags: bit 0 an RSN element was found, bit 1 a pairwise or key-management suite has another OUI,
 * bit 2 the element is malformed (a count or length runs past its end), bit 3 it has a pairwise list, bit 4 it has a
 * key-management list (an absent list means CCMP and 802.1X). parse_result: wpa_parse_wpa_ie_rsn's return
 * (0 accepted). version and capabilities: the element's fields (capabilities bit 6 MFP required, bit 7 MFP capable).
 * pairwise and akm: bit n set for suite type n under OUI 00-0F-AC (n < 32). */
struct a733_radio_rsn {
    uint8_t check, group, flags;
    int8_t parse_result;
    uint16_t version, capabilities;
    uint32_t pairwise, akm;
};
_Static_assert(sizeof(struct a733_radio_rsn)==16,"RSN summary tail");
#define A733_RADIO_RSN_SUITE_OTHER 0xffu
#define A733_RADIO_RSN_SUITE_ABSENT 0xfeu
/* The candidate test's checks in the order aic8800_supplicant_config_valid applies them. */
enum a733_radio_rsn_check {
    A733_RADIO_RSN_PASSED, A733_RADIO_RSN_NO_ELEMENT, A733_RADIO_RSN_TOO_LONG, A733_RADIO_RSN_MALFORMED,
    A733_RADIO_RSN_GROUP_NOT_CCMP, A733_RADIO_RSN_PAIRWISE_NO_CCMP, A733_RADIO_RSN_MFP_REQUIRED,
    A733_RADIO_RSN_NO_PSK, A733_RADIO_RSN_OTHER /* every RSN condition met; the configuration or parser refused */
};
/* Summarize an RSN element (ID 48, its length byte, then the body); element NULL records that none was found. */
void a733_radio_rsn_summarize(struct a733_radio_rsn *,const uint8_t *element,size_t bytes);
/* The first RSN condition of the PSK candidate test that the summary fails, or A733_RADIO_RSN_PASSED. */
enum a733_radio_rsn_check a733_radio_rsn_classify(const struct a733_radio_rsn *);
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
    struct a733_radio_assoc assoc;
    struct a733_radio_scan_counts scan;
    struct a733_radio_rsn rsn;
    unsigned has_assoc,has_scan,has_rsn;
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
/* Set the RSN summary (and head flags bit 11). */
void a733_radio_report_rsn(struct a733_radio_report *,const struct a733_radio_rsn *);
/* Set the association tail (and head flags bit 9). */
void a733_radio_report_association(struct a733_radio_report *,const struct a733_radio_assoc *);
void a733_radio_report_firmware(struct a733_radio_report *);
struct aic8800_firmware;
void a733_radio_report_firmware_progress(struct a733_radio_report *,const struct aic8800_firmware *,uint64_t ticks_per_ms);
void a733_radio_report_identity(struct a733_radio_report *,uint32_t chip_word,uint32_t subrevision_word);
/* One scan result; the list keeps one entry per BSSID (the latest sighting) and
 * grows as needed. When memory runs out the result is counted in aps_dropped.
 * The entry takes no SSID: a scan hears the user's network among its neighbours,
 * and the record reaches the host's result files. ssid_bytes stays 0, and the
 * host decoder refuses an entry whose length is not 0. */
void a733_radio_report_ap(struct a733_radio_report *,const uint8_t bssid[6],uint16_t frequency_mhz,int8_t rssi_dbm);
void a733_radio_report_scan_done(struct a733_radio_report *,unsigned reported);
/* The dispatcher's count of scan result records retired without an event (saturates at UINT32_MAX). */
void a733_radio_report_set_aside(struct a733_radio_report *,uint64_t set_aside);
/* Length of the encoded record, and the encoding. encode returns the bytes
 * written, or 0 when capacity is smaller than the length. */
size_t a733_radio_report_bytes(const struct a733_radio_report *);
size_t a733_radio_report_encode(const struct a733_radio_report *,void *out,size_t capacity);
#endif
