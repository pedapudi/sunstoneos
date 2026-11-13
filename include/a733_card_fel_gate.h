#ifndef SUNSTONE_A733_CARD_FEL_GATE_H
#define SUNSTONE_A733_CARD_FEL_GATE_H
#define A733_CARD_FEL_STAGE 0x0c02
#define A733_CARD_FEL_ENTRY 1
#define A733_CARD_FEL_MODE_CHECKED 2
#define A733_CARD_FEL_WATCHDOG_SET 3
#define A733_CARD_FEL_REQUEST_CLEARED 4
#define A733_CARD_FEL_BEFORE_ROM 5
/* WDT0 policy word appended after the gate code. The card first stage
 * disarms WDT0. The FEL proof re-arms it for 16 s (the largest interval,
 * A733 manual 8.3.6.6), so a ROM that does not re-enumerate still resets
 * to FEL; docs/designs/card-fel-recovery.md has the time budget. */
#define A733_CARD_FEL_PRODUCTION_WATCHDOG 0
#define A733_CARD_FEL_PROOF_WATCHDOG 0xb1
/* Cold-boot marker: RTC IC_CHARA_REG (A733 manual 9.1.7.20), in the VDD_RTC
 * domain and zero after the RTC supply was lost. A write takes effect only
 * with key 0x16aa in bits 31:16; bits 15:0 then read 0x16aa. Only the gate's
 * cold path and the runner's first FEL contact write it. */
#define A733_CARD_FEL_RTC_MARKER 0x070901f0
#define A733_CARD_FEL_MARKER_KEYED 0x16aa16aa
#define A733_CARD_FEL_MARKER_SET 0x16aa
/* Gate record in RTC general-purpose register 2 (0x07090108) after an entry
 * without the FEL request: A733_CARD_FEL_STAGE in bits 31:16, the flags
 * below, the refused watchdog step in bits 11:8 and the WDT0 mode read at
 * entry in bits 7:0. It never equals the vendor request 0x5aa5a55a. */
#define A733_CARD_FEL_RECORD_COLD 0x8000        /* A cold path ran in this power epoch. */
#define A733_CARD_FEL_RECORD_WARM 0x4000        /* The latest entry selected FEL. */
#define A733_CARD_FEL_RECORD_UNMARKED 0x2000    /* The cold path's marker did not read back. */
#define A733_CARD_FEL_RECORD_ONCE 0x1000        /* The cold path ran because GP2 held the boot-once value. */
/* Boot-once value in GP2: software cannot clear the marker (measured 2026-11-12), so the runner requests one
 * card boot by writing this value and resetting. The gate consumes it: it replaces GP2 with its cold record and
 * reads that back before boot0 runs, so every later reset finds no boot-once value and, with the marker set,
 * enters FEL. It never equals the vendor request or a gate record. */
#define A733_CARD_FEL_BOOT_ONCE 0xb0070c02
/* The cold path arms WDT0 for 16 s before boot0, so a hang in boot0 resets
 * into the warm path and FEL. */
#define A733_CARD_FEL_COLD_WATCHDOG 0xb1
#endif
