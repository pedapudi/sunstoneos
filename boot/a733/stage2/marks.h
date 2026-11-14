/* Stage-2 progress words in the RTC domain (docs/designs/card-boot-chain.md,
 * "Stage-2 progress words"). A WDT0 reset keeps them, so the runner's first
 * contact after a stopped cold boot reads where stage 2 stopped; a power loss
 * clears them. Only the general-purpose words 0x07090100..0x0709011c are
 * written, never GP2 (0x07090108, the card gate's record, which only
 * a733_stage2_gate_entry() clears) and never the cold-boot marker. */
#ifndef A733_STAGE2_MARKS_H
#define A733_STAGE2_MARKS_H
#include <stdint.h>

enum { A733_MARK_TAG, A733_MARK_LAST, A733_MARK_GP2, A733_MARK_SNAPSHOT,
       A733_MARK_FAULT, A733_MARK_ESR, A733_MARK_ELR, A733_MARK_FAR };
#define A733_MARK_TAG_VALUE UINT32_C(0x53320001)  /* Layout version 1. */
#define A733_MARK_STEP UINT32_C(0x0c030000)       /* LAST: step in 15:8, refused check in 7:0. */
#define A733_MARK_FINISH UINT32_C(0x0c040000)     /* FAULT: the finish() status in 7:0. */

/* Defined once, in loader.c; it ignores GP2 and any word past FAR, so no call
 * can reach GP2 or the marker at 0x070901f0. */
void a733_mark_word(unsigned word, uint32_t value);
#endif
