#ifndef A733_TOP_OPP_RELOCK_H
#define A733_TOP_OPP_RELOCK_H
/* The CPU0 relock of the workload's top-operating-point profile
 * (boot/a733/top-opp-relock.c). Its record is 16 words in the identity
 * header's reserved words 8 to 23 (report offset 0x80 to the header's end). */
#define TOP_OPP_DOMAINS 3u
#define TOP_OPP_RECORD_OFFSET 0x80u
#define TOP_OPP_MAGIC 0x3130505054504f54ull /* "TOPTPP01" in little-endian byte order. */
#define TOP_OPP_MAGIC_WORD 0u
#define TOP_OPP_RESULT_WORD 1u
#define TOP_OPP_START_WORD 2u
#define TOP_OPP_END_WORD 3u
#define TOP_OPP_ENTRY_WORD 4u   /* PLL << 32 | mux at entry: CPU-B, CPU-L, DSU. */
#define TOP_OPP_FINAL_WORD 7u   /* PLL << 32 | mux after the relock. */
#define TOP_OPP_PARKED_WORD 10u /* Ticks from park to select << 32 | lock reads. */
#define TOP_OPP_BOARD_WORD 13u  /* Oscillator word << 32 | SID word 0x0300624c. */
#define TOP_OPP_PMIC_WORD 14u   /* The seven PMIC bytes read, byte i of read i. */
#define TOP_OPP_BUS_WORD 15u    /* S_TWI0 divider << 32 | reads completed << 8 | last status. */
#define TOP_OPP_RECORD_WORDS 16u
#define TOP_OPP_DONE 1u
#define TOP_OPP_REFUSED_STATE 2u      /* A domain not on its locked PLL, or an update pending. */
#define TOP_OPP_REFUSED_MODULATION 3u /* A pattern or LFM enable set. */
#define TOP_OPP_REFUSED_RANGE 4u      /* N outside 26..86. */
#define TOP_OPP_REFUSED_DEADLINE 5u
#define TOP_OPP_REFUSED_OSCILLATOR 6u /* The DCXO not enabled and stable at 26 MHz. */
#define TOP_OPP_REFUSED_BIN 7u        /* A speed-bin code other than 0x04 (vf0400). */
#define TOP_OPP_REFUSED_BUS 8u        /* S_TWI0 not in the PMIC provider's idle baseline. */
#define TOP_OPP_REFUSED_TRANSFER 9u   /* A PMIC read with another status, or no event in time. */
#define TOP_OPP_REFUSED_SUPPLY 10u    /* Another PMIC, DCDC3 or DCDC5 off, or not at 1050 and 1000 mV. */
#define TOP_OPP_FAULT 0x100u          /* | domain index << 4 | 1 park readback, 2 update, 3 lock,
                                       * 4 select readback, 5 final words, 6 deadline in a settle. */
/* The CPU-B lowering record: four words in the identity header's reserved
 * words 4 to 7 (report offset 0x60). */
#define TOP_OPP_LOWER_OFFSET 0x60u
#define TOP_OPP_LOWER_RESULT_WORD 0u
#define TOP_OPP_LOWER_ENTRY_WORD 1u
#define TOP_OPP_LOWER_FINAL_WORD 2u
#define TOP_OPP_LOWER_PARKED_WORD 3u
#define TOP_OPP_BOOT_B_N 39u /* CPU-B 1014 MHz, boot0's word 0xf8802700. */
#ifndef __ASSEMBLER__
#include <stdint.h>
uint64_t top_opp_relock(volatile uint64_t *record, uint64_t deadline);
uint64_t top_opp_lower_b(volatile uint64_t *lower, uint64_t deadline);
#endif
#endif
