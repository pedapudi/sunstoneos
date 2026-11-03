#ifndef SUNSTONE_A733_RADIO_RESULT_H
#define SUNSTONE_A733_RADIO_RESULT_H
#include "a733_radio_platform.h"
/* The radio service's result: the stage it reached and the facts observed on
 * the way, in a fixed little-endian record that a host decoder reads
 * (tools/radio_result.py). Every field is a copy; the record grants nothing.
 * Stages run in this order and a later stage implies the earlier ones. */
enum a733_radio_stage {
    A733_RADIO_STAGE_NONE, A733_RADIO_STAGE_RAILS_OBSERVED, A733_RADIO_STAGE_GUARD_PASSED,
    A733_RADIO_STAGE_POWERED, A733_RADIO_STAGE_ENUMERATED, A733_RADIO_STAGE_FIRMWARE,
    A733_RADIO_STAGE_IDENTIFIED, A733_RADIO_STAGE_SCANNED, A733_RADIO_STAGE_ASSOCIATED
};
#define A733_RADIO_RESULT_MAGIC UINT64_C(0x3152444152333337) /* bytes "733RADR1" */
#define A733_RADIO_RESULT_VERSION 1u
#define A733_RADIO_RESULT_RAILS 4u
#define A733_RADIO_RESULT_FUNCTIONS 8u
#define A733_RADIO_RESULT_APS 16u
struct a733_radio_result_rail { uint8_t rail, enable, selector, flags; uint32_t programmed_uv; }; /* flags: bit0 enabled, bit1 decoded */
struct a733_radio_result_function { uint16_t manufacturer, device; uint8_t class_code, present, pad[2]; };
struct a733_radio_result_ap { uint8_t bssid[6], ssid_bytes, rssi_dbm; uint16_t frequency; uint8_t channel, security, ssid[32]; };
struct a733_radio_result {
    uint64_t magic;
    uint32_t version, bytes;
    uint32_t stage, failed, failure_stage, flags;      /* flags: bit0 guard enabled, bit1 guard observed, bit2 BLDO5 powered on */
    uint32_t pad_power[4];                              /* GPIO_POW_MOD_SEL, GPIO_POW_VAL, SGPIO_POW_MOD_SEL, SGPIO_POW_VAL */
    struct a733_radio_result_rail rails[A733_RADIO_RESULT_RAILS];
    uint32_t rail_count, platform_stage, ocr, selected_ocr;
    uint32_t rca, function_count, cccr_revision, sdio_revision;
    uint32_t sd_revision, capabilities, wide, card_hz;
    struct a733_radio_result_function functions[A733_RADIO_RESULT_FUNCTIONS];
    uint32_t chip_word, subrevision_word, firmware_flags, reserved0;
    uint32_t ap_count, aps_reported, reserved1[2];
    struct a733_radio_result_ap aps[A733_RADIO_RESULT_APS];
};
_Static_assert(sizeof(struct a733_radio_result_rail)==8,"rail record");
_Static_assert(sizeof(struct a733_radio_result_function)==8,"function record");
_Static_assert(sizeof(struct a733_radio_result_ap)==44,"AP record");
_Static_assert(sizeof(struct a733_radio_result)==928,"radio result geometry");
/* Fill the platform's part: rails, guard words, stage reached through
 * enumeration, and the card's identity. Later stages keep what a caller has
 * already set in the later fields. Copies only; no hardware access. */
void a733_radio_result_platform(struct a733_radio_result *,const struct a733_radio_platform *);
/* Module identity (chip and subrevision words) once read: stage IDENTIFIED. */
void a733_radio_result_identity(struct a733_radio_result *,uint32_t chip_word,uint32_t subrevision_word);
/* Firmware started: stage FIRMWARE (a later identity read raises it further). */
void a733_radio_result_firmware(struct a733_radio_result *);
/* Append one scan result, strongest kept if the list is full; stage SCANNED once complete. */
void a733_radio_result_ap(struct a733_radio_result *,const uint8_t bssid[6],const uint8_t *ssid,unsigned ssid_bytes,
    uint16_t frequency,int8_t rssi_dbm);
void a733_radio_result_scan_done(struct a733_radio_result *,unsigned reported);
#endif
