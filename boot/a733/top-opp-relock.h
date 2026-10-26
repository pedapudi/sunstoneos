#ifndef A733_TOP_OPP_RELOCK_H
#define A733_TOP_OPP_RELOCK_H
/* The CPU0 relock of the workload's top-operating-point profile
 * (boot/a733/top-opp-relock.c). Its record is 13 words in the identity
 * header's reserved words 8 to 20 (report offset 0x80). */
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
#define TOP_OPP_RECORD_WORDS 13u
#define TOP_OPP_DONE 1u
#define TOP_OPP_REFUSED_STATE 2u      /* A domain not on its locked PLL, or an update pending. */
#define TOP_OPP_REFUSED_MODULATION 3u /* A pattern or LFM enable set. */
#define TOP_OPP_REFUSED_RANGE 4u      /* N outside 26..86. */
#define TOP_OPP_REFUSED_DEADLINE 5u
#define TOP_OPP_FAULT 0x100u          /* | domain index << 4 | 1 park readback, 2 update, 3 lock,
                                       * 4 select readback, 5 final words, 6 deadline in a settle. */
#ifndef __ASSEMBLER__
#include <stdint.h>
uint64_t top_opp_relock(volatile uint64_t *record, uint64_t deadline);
#endif
#endif
