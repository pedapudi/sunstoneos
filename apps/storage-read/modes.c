#include <stdint.h>
/* New SD profiles require this immutable capability in addition to the
 * unchanged legacy session/service geometry and the exact linked build ID. */
__attribute__((used,retain)) const uint64_t storage_read_sd_modes[4]={
 UINT64_C(0x4153373353444d31),1,7,400000000};
