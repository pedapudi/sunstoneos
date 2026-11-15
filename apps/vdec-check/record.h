#ifndef VDEC_CHECK_RECORD_H
#define VDEC_CHECK_RECORD_H
/* The stage-1 board record of the video decoder service (docs/video-decode-os.md, "Board stages"). Only an image built
 * with SUNSTONE_VIDEO_DECODE_MEASURE (the vdec-check image) fills it; it is not part of the class interface
 * (include/sunstone_video_decode.h). apps/vdec-check/host.py decodes it. */
#include <stdint.h>
/* Words the owner reads under the register lock: at open before any write (entry), after the cold start (started) and
 * after the close (closed). The index names the register. */
enum {
    VDEC_CHECK_CCU_PLL_REF,    /* CCU 0x000 */
    VDEC_CHECK_RTC_OSC,        /* RTC 0x160, oscillator detection */
    VDEC_CHECK_CCU_AHB,        /* CCU 0x500, AHB clock */
    VDEC_CHECK_CCU_NSI,        /* CCU 0x580, NSI clock */
    VDEC_CHECK_CCU_MBUS,       /* CCU 0x588, MBUS clock */
    VDEC_CHECK_CCU_PLL_VE0,    /* CCU 0x220, PLL_VE0 control */
    VDEC_CHECK_CCU_PLL_VE0_P0, /* CCU 0x228, PLL_VE0 pattern 0 */
    VDEC_CHECK_CCU_PLL_VE0_P1, /* CCU 0x22c, PLL_VE0 pattern 1 */
    VDEC_CHECK_CCU_GATES,      /* CCU 0x1918, PLL_VE output gate configuration */
    VDEC_CHECK_CCU_STATUS,     /* CCU 0x1998, PLL_VE output gate status */
    VDEC_CHECK_CCU_AUTO,       /* CCU 0x1b10, autonomous VE power control */
    VDEC_CHECK_CCU_MODULE,     /* CCU 0xa88, decoder module clock: gate 31, mux 26:24, divider 4:0 */
    VDEC_CHECK_CCU_BGR,        /* CCU 0xa8c, decoder reset (bit 18) and bus gate (bit 2) */
    VDEC_CHECK_CCU_AHB_GATES,  /* CCU 0x5c0, VE AHB master gate (bit 0) */
    VDEC_CHECK_CCU_MBUS_GATES, /* CCU 0x5e0, VE_DEC MBUS master gate (bit 14) */
    VDEC_CHECK_CCU_MCLK_GATES, /* CCU 0x5e4, VE_DEC MBUS clock gate (bit 18) */
    VDEC_CHECK_RCCU_PCK,       /* R_CCU 0x1ac, PCK600 bus clock (bit 0) */
    VDEC_CHECK_GICD_ENABLE,    /* GICD_ISENABLER2 (0x03400108): INTID 81 is bit 17 */
    VDEC_CHECK_GICD_PENDING,   /* GICD_ISPENDR2 (0x03400208) */
    VDEC_CHECK_GICD_ACTIVE,    /* GICD_ISACTIVER2 (0x03400308) */
    VDEC_CHECK_WORDS
};
/* The power-domain owner's record of domain 2's latest transition (struct a733_power_transition), as the owner
 * published it with the row's latest answer. Times are the owner's clock in microseconds. */
struct vdec_check_transition {
    uint32_t domain, policy_before, status_before, policy_written, first_word, pending_word, reached_word;
    uint32_t all_and, all_or, reads, spin_reads, commands;
    uint64_t command_at, first_at, pending_at, reached_at;
};
#define VDEC_CHECK_INFO_MAGIC UINT32_C(0x49444556) /* "VEDI" */
struct vdec_check_record {
    uint32_t magic, bytes;
    uint32_t entry[VDEC_CHECK_WORDS], started[VDEC_CHECK_WORDS], closed[VDEC_CHECK_WORDS];
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
    struct vdec_check_transition transition;
};
/* Copy the record of the latest open or close into out: 1 copied, 0 none yet, -1 not a measurement image. */
int vdec_check_record(struct vdec_check_record *out);
#endif
