#ifndef SUNSTONE_VIDEO_DECODE_H
#define SUNSTONE_VIDEO_DECODE_H
/* Video decoder class (the VE decoder: JPEG, H.264 and HEVC). An application declares the video-decode device in its
 * manifest.json ("devices": {"required": ["video-decode"]}); the session grants it with SS_DEVICE_VIDEO_DECODE. One OS
 * owner task does every register access (apps/os/video_decode.c, docs/video-decode-os.md). Every call returns at once:
 * 1 done, 0 not yet (repeat the identical call after ss_video_decode_wait), -1 refused or failed. One request is in
 * flight.
 *
 * Stage 1 offers open and close only. Open powers PCK600 domain 2 through the shared power-domain owner, cold-starts
 * the decoder with PLL_VE0 at 624 MHz and reads its identity; close quiesces it again. No decode runs. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Words the owner reads under the register lock: at open before any write (entry), after the cold start (started) and
 * after the close (closed). The index names the register. */
enum {
    SS_VIDEO_DECODE_CCU_PLL_REF,    /* CCU 0x000 */
    SS_VIDEO_DECODE_RTC_OSC,        /* RTC 0x160, oscillator detection */
    SS_VIDEO_DECODE_CCU_AHB,        /* CCU 0x500, AHB clock */
    SS_VIDEO_DECODE_CCU_NSI,        /* CCU 0x580, NSI clock */
    SS_VIDEO_DECODE_CCU_MBUS,       /* CCU 0x588, MBUS clock */
    SS_VIDEO_DECODE_CCU_PLL_VE0,    /* CCU 0x220, PLL_VE0 control */
    SS_VIDEO_DECODE_CCU_PLL_VE0_P0, /* CCU 0x228, PLL_VE0 pattern 0 */
    SS_VIDEO_DECODE_CCU_PLL_VE0_P1, /* CCU 0x22c, PLL_VE0 pattern 1 */
    SS_VIDEO_DECODE_CCU_GATES,      /* CCU 0x1918, PLL_VE output gate configuration */
    SS_VIDEO_DECODE_CCU_STATUS,     /* CCU 0x1998, PLL_VE output gate status */
    SS_VIDEO_DECODE_CCU_AUTO,       /* CCU 0x1b10, autonomous VE power control */
    SS_VIDEO_DECODE_CCU_MODULE,     /* CCU 0xa88, decoder module clock: gate 31, mux 26:24, divider 4:0 */
    SS_VIDEO_DECODE_CCU_BGR,        /* CCU 0xa8c, decoder reset (bit 18) and bus gate (bit 2) */
    SS_VIDEO_DECODE_CCU_AHB_GATES,  /* CCU 0x5c0, VE AHB master gate (bit 0) */
    SS_VIDEO_DECODE_CCU_MBUS_GATES, /* CCU 0x5e0, VE_DEC MBUS master gate (bit 14) */
    SS_VIDEO_DECODE_CCU_MCLK_GATES, /* CCU 0x5e4, VE_DEC MBUS clock gate (bit 18) */
    SS_VIDEO_DECODE_RCCU_PCK,       /* R_CCU 0x1ac, PCK600 bus clock (bit 0) */
    SS_VIDEO_DECODE_GICD_ENABLE,    /* GICD_ISENABLER2 (0x03400108): INTID 81 is bit 17 */
    SS_VIDEO_DECODE_GICD_PENDING,   /* GICD_ISPENDR2 (0x03400208) */
    SS_VIDEO_DECODE_GICD_ACTIVE,    /* GICD_ISACTIVER2 (0x03400308) */
    SS_VIDEO_DECODE_WORDS
};
/* The power-domain owner's record of the latest command to one domain (include/a733_power_domains.h, struct
 * a733_power_transition), copied after the close. Times are the owner's clock in microseconds. */
struct ss_video_decode_transition {
    uint32_t domain, policy_before, status_before, policy_written, first_word, pending_word, reached_word;
    uint32_t all_and, all_or, reads, spin_reads, commands;
    uint64_t command_at, first_at, pending_at, reached_at;
};
#define SS_VIDEO_DECODE_INFO_MAGIC UINT32_C(0x49444556) /* "VEDI" */
#define SS_VIDEO_DECODE_DOMAINS 4u
struct ss_video_decode_info {
    uint32_t magic, bytes;
    uint32_t entry[SS_VIDEO_DECODE_WORDS], started[SS_VIDEO_DECODE_WORDS], closed[SS_VIDEO_DECODE_WORDS];
    /* The cold start's own record: the critical gates at entry and at A1, the 0x5e4 word written and read back when
     * its bit 18 was opened (zero when no write was needed), and the PLL lock wait's reads. */
    uint32_t gates_entry[3], gates_a1[3], mclk_written, mclk_readback, lock_reads;
    /* Decoder identity at VE +0xf0, +0xe0 and +0xe4, read once started; the admitted compound version (0 when
     * refused); the cold-start phase reached (5 started) and whether the binding failed. */
    uint32_t feature, identity_high, identity_low, version_high, version_low, cold_phase, failed;
    /* The domain-2 row's answer and the power authority word read at that answer; 1 when the owner measured
     * transitions back to back (stage 1). */
    uint32_t row_state, authority, measured, reserved;
    /* Binding clock (microseconds): start and end of the entry hold, the ON answer, end of hold A, end of the lock
     * wait, end of hold B. */
    uint64_t entry_at, entered_at, on_at, hold_a_at, locked_at, hold_b_at;
    /* Owner clock (microseconds): the first acquire call and the row's ON answer as the decoder task saw them. */
    uint64_t row_request_us, row_on_us;
    /* System counter ticks (24 MHz): open request, started, closed. */
    uint64_t requested_ticks, started_ticks, closed_ticks;
    struct ss_video_decode_transition transition[SS_VIDEO_DECODE_DOMAINS];
};
/* Open: power, cold start and identity. info, when not NULL, receives the record whenever the call does not return 0. */
int ss_video_decode_open(struct ss_video_decode_info *info);
/* Close: the quiesce (module gate, bus gate, reset, then PLL_VE0 disabled; domain 2 stays ON). info as for open. */
int ss_video_decode_close(struct ss_video_decode_info *info);
/* 1 with the step and value of the check that refused or failed, 0 when none, -1 when the device is not granted. */
int ss_video_decode_fault(uint32_t *step, uint32_t *value);
/* Block until the request in flight is answered or wait_us elapses. */
void ss_video_decode_wait(uint32_t wait_us);
#ifdef __cplusplus
}
#endif
#endif
