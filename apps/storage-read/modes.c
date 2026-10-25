#include <stdint.h>
/* New SD profiles require this immutable capability in addition to the
 * unchanged legacy session/service geometry and the exact linked build ID.
 * Version 2 admits profiles 0-3 (mask 15), where 3 is UHS-I SDR104, and
 * publishes record version 4 with per-read timing for nonzero profiles. */
__attribute__((used,retain)) const uint64_t storage_read_sd_modes[4]={
 UINT64_C(0x4153373353444d31),2,15,400000000};
