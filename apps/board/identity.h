#ifndef SUNSTONE_BOARD_IDENTITY_H
#define SUNSTONE_BOARD_IDENTITY_H
#include <stdint.h>
/* Produced by admitted EL3 boot code, retained and immutable at EL1. The
 * record authorizes exactly four chip-ID shadow reads, not other SID access. */
struct board_radio_chip_id { uint64_t secure_read_record; uint32_t words[4]; };
const struct board_radio_chip_id *board_radio_chip_id(void);
#endif
