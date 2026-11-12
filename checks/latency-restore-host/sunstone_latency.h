#ifndef SS_RESTORE_TEST_LATENCY_H
#define SS_RESTORE_TEST_LATENCY_H
#include <stdint.h>
uint64_t ss_latency_now(void);
void ss_latency_mask_open(unsigned, uint64_t, uintptr_t);
#endif
