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
#endif
