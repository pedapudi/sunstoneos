#ifndef VDEC_CHECK_RECORD_H
#define VDEC_CHECK_RECORD_H
/* The board record of the video decoder service (docs/video-decode-os.md, "Board stages"). Only an image built
 * with SUNSTONE_VIDEO_DECODE_MEASURE (the vdec-check image) fills it; it is not part of the class interface
 * (include/sunstone_video_decode.h). apps/vdec-check/host.py decodes it. */
#include <stdint.h>
#include "a733_power_domain_row.h"
#include "a733_video_platform.h"
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
#define VDEC_CHECK_INFO_MAGIC UINT32_C(0x49444556) /* "VEDI" */
/* The decoder writes of one picture: the executor's 659 programming writes, the interrupt-enable disable and the
 * completion acknowledgment. */
#define VDEC_CHECK_LOG 661u
/* The stage-2 decode (docs/video-decode-os.md, "The stage-2 record"). */
struct vdec_check_decode {
    /* The executor's state and index when the decode ended, its status word (0x11c, read at completion), the format
     * word it read before programming (0xec), the status word the drain read, and the decoder writes made. */
    uint32_t state, index, status, format_old, status_after, writes;
    /* The parsed picture, its padded plane geometry, the input bytes and the active output bytes. */
    uint32_t width, height, stride, rows, file_bytes, output_bytes;
    /* Device address and mapped bytes of the input, luma and chroma buffers. */
    uint32_t iova[3], mapped[3];
    /* INTID 81: interrupts taken, those taken with no decode armed, whether the armed one was seen; the guard result
     * (0 every poisoned byte intact, else the buffer and kind of the first change); os_iommu_scan of masters 6 and 7
     * (1 none, 0 a fault) and their fault words (struct a733_iommu_owner_fault); the GIC enable, pending and active
     * words of INTID 81 after the decode. */
    uint32_t irq_count, stray, seen, guard, scan[2];
    uint32_t iommu_fault[2][10];
    uint32_t gic[3], reserved;
    /* System counter ticks (24 MHz): the decode request, the line enabled, the trigger write, the interrupt, the drain
     * done and the output written. */
    uint64_t begin_ticks, armed_ticks, trigger_ticks, irq_ticks, finish_ticks, done_ticks;
    /* SHA-256 of the active output bytes as written to the application's region. */
    uint8_t output_sha256[32];
    /* Each decoder write: offset | width << 24, then the value. */
    uint32_t log[VDEC_CHECK_LOG][2];
};
struct vdec_check_record {
    uint32_t magic, bytes;
    uint32_t entry[VDEC_CHECK_WORDS], started[VDEC_CHECK_WORDS], closed[VDEC_CHECK_WORDS];
    /* The binding's cold record (include/a733_video_platform.h): the critical gates at entry and at A1, the 0x5e4
     * word written and read back, the binding-clock stamps of each phase, the lock wait's reads and the VCU pulse's
     * two VE+0x804 words. */
    struct a733_video_cold_record cold;
    /* Decoder identity at VE +0xf0, +0xe0 and +0xe4, read once started; the admitted compound version (0 when
     * refused); the cold-start phase reached (5 started) and whether the binding failed. */
    uint32_t feature, identity_high, identity_low, version_high, version_low, cold_phase, failed;
    /* The domain-2 row's answer and the power authority word read at that answer; 1 when the owner measured
     * transitions back to back (stage 1). */
    uint32_t row_state, authority, measured, fault_closed;
    /* A fault after the first write: the service's fault step and value, and the address and word of the binding's
     * latest access before the close, which is the word a refused comparison judged. failed above holds the unit's
     * failed (bit 0), mutex held (1), uncertain (2) and late (3) flags at the fault. fault_closed is 1 when the
     * binding then closed the decoder, and closed[] holds the words after that close. */
    uint32_t fault_step, fault_value, refused_address, refused_word;
    /* Owner clock (microseconds): before the first open call, and at the end of the first call that saw the row
     * answered. That call runs holds A and B after the answer, so the row's own wait is the cold record's entered_at
     * to on_at (binding clock) and the transition's command_at to reached_at (owner clock), not these two. */
    uint64_t row_request_us, row_seen_us;
    /* Owner clock (microseconds): the power-domain owner's first step and the step that published DCDC2 verified
     * (struct a733_power_domains, started_at and verified_at); both zero until DCDC2 is verified. */
    uint64_t owner_started_us, owner_verified_us;
    /* System counter ticks (24 MHz): open request, started, closed. */
    uint64_t requested_ticks, started_ticks, closed_ticks;
    /* Domain 2's latest transition (struct a733_power_transition) as the power-domain owner published it with the
     * row's latest answer, on the owner's clock in microseconds; all zero while the row is pending. */
    struct a733_power_transition transition;
    struct vdec_check_decode decode;
};
/* Copy the record of the latest open or close into out: 1 copied, 0 none yet, -1 not a measurement image. */
int vdec_check_record(struct vdec_check_record *out);
#endif
