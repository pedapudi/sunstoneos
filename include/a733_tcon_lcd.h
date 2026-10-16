#ifndef SUNSTONE_A733_TCON_LCD_H
#define SUNSTONE_A733_TCON_LCD_H
#include "a733_dsi_video.h"
#include <stdint.h>

#define A733_TCON_LCD1_BASE UINT32_C(0x05502000)
#define A733_TCON_TOP_BASE UINT32_C(0x05500000)
struct a733_tcon_lcd_timing {
    struct a733_dsi_video_timing dsi;
    uint32_t actual_dclk_hz;
};
struct a733_tcon_lcd_word { uintptr_t address; uint32_t mask, value; };
#define A733_TCON_LCD_WORDS 16u
struct a733_tcon_lcd_plan {
    struct a733_tcon_lcd_word words[A733_TCON_LCD_WORDS];
    uint32_t count, block_space, start_delay, safe_fifo;
};
/* Pure, atomic output. Fixed LCD1 -> DSI1, single link, DE pixel source;
 * supported DSI video profile, at most 1920x1080 progressive, vertical front
 * porch >=2, vertical back porch >=4, block space >20. Actual DCLK must equal
 * the DSI1 display PLL LS output; divider1, phase0 only. No clock is programmed
 * or measured. Nominal DSI pixel rate and actual DCLK are distinct in burst.
 * Timing/output storage must be disjoint. No selected panel defaults. */
int a733_tcon_lcd_plan(const struct a733_tcon_lcd_timing *, struct a733_tcon_lcd_plan *);

enum a733_tcon_lcd_action { A733_TCON_PREPARE, A733_TCON_START,
    A733_TCON_OBSERVE, A733_TCON_STOP };
enum a733_tcon_lcd_state { A733_TCON_PENDING, A733_TCON_READY,
    A733_TCON_ACTIVE, A733_TCON_STOPPED, A733_TCON_RETAINED };
enum a733_tcon_lcd_error { A733_TCON_OK, A733_TCON_TIME, A733_TCON_IO,
    A733_TCON_AUTHORITY, A733_TCON_INITIAL_STATE, A733_TCON_VERIFY,
    A733_TCON_UNDERFLOW, A733_TCON_REENTRY };
struct a733_tcon_lcd_config;
struct a733_tcon_lcd_ops {
    int (*admit)(void *, const struct a733_tcon_lcd_config *, enum a733_tcon_lcd_action);
    int (*read32)(void *, uintptr_t, uint32_t *);
    int (*write32)(void *, uintptr_t, uint32_t);
    int (*barrier)(void *);
    int (*now_us)(void *, uint64_t *);
};
struct a733_tcon_lcd_config {
    struct a733_tcon_lcd_ops ops;
    void *context;
    uint64_t lease;
    struct a733_tcon_lcd_timing timing;
};
struct a733_tcon_lcd {
    struct a733_tcon_lcd_config config;
    struct a733_tcon_lcd_plan plan;
    uint64_t deadline, last_now;
    uint32_t expected, debug_status, irq_status;
    enum a733_tcon_lcd_state state;
    enum a733_tcon_lcd_error error;
    enum a733_tcon_lcd_action action;
    unsigned initialized, entered, step, index, substep, writes_attempted;
};
/* Fresh zero storage, privileged serialized owner, no reinitialization.
 * Copies inputs. Callback/context storage stays private, live and disjoint.
 * Callbacks return exactly1, are bounded/non-reentrant; Device MMIO is ordered
 * and barrier completes prior writes. admit must authenticate lifetime exclusive
 * ownership of LCD1, DSI1/PHY/PLL, its DE route and the COMPLETE shared top words
 * 0x04,0x0c,0x20 against every writer, including other LCD/LVDS/DSI users.
 * Stable power/bus clocks/released CCU reset, a resolved boot handoff, pins and
 * independent watchdog recovery are required. COMBOPHY1 clock source must have
 * been set to display PLL while inactive, before PHY preparation. No live mux
 * switching is authorized here. DE routing, clock/reset/pin managers and the
 * production evidence provider remain external prerequisites.
 * PREPARE: LCD1 disabled, DSI1 idle and no DE producer; START: DSI1/panel prepared
 * but engine idle, DE armed with valid mapped/visible pixels and exact timing;
 * OBSERVE: same ownership and valid feed; STOP: DSI1 already stopped, then DE
 * quiescent and transactions drained. Register values cannot prove DE drain.
 */
int a733_tcon_lcd_init(struct a733_tcon_lcd *, const struct a733_tcon_lcd_config *, uint64_t deadline);
/* READY->START, ACTIVE->OBSERVE/STOP; refusal invokes no callback. Absolute
 * monotonic microsecond deadline. No restart, rollback, retry or release. */
int a733_tcon_lcd_request(struct a733_tcon_lcd *, enum a733_tcon_lcd_action, uint64_t deadline);
/* At most one admission/MMIO/barrier per poll, pre/post deadline checks.
 * ACTIVE proves enables/readbacks, not scanout. Explicit OBSERVE samples line
 * and underflow status without clearing flags. STOP disables LCD output and
 * global control (documented local-idle reset), then gates data/output clocks.
 * STOPPED establishes local disable only; resources stay held. Every fault is
 * terminal/retained; states between operations are quiet. No CCU/PLL reset or
 * clock programming, IRQ acknowledgement, DE programming or buffer release. */
enum a733_tcon_lcd_state a733_tcon_lcd_poll(struct a733_tcon_lcd *);
#endif
