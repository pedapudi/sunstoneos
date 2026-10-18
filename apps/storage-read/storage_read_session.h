#ifndef SUNSTONE_STORAGE_READ_SESSION_H
#define SUNSTONE_STORAGE_READ_SESSION_H
#include "a733_board_application.h"
#include "a733_pins.h"
#define A733_STORAGE_READ_CONTRACT_MAGIC UINT64_C(0x4153373352444331)
#define A733_STORAGE_READ_SESSION_MAGIC UINT64_C(0x4153373352444131)
#define STORAGE_READ_HEADER 256u
#define STORAGE_READ_OUTPUT 512u
struct storage_read_session {
 struct a733_board_session board;
 uint64_t magic,version,bytes,owner,authority;
 uint8_t initial[32],memory[32],supplies[32],topology[32],supervision[32];
 struct a733_pin_config expected[7],desired[7];
 uint8_t reserved[6];
 uint64_t identify_us,read_us;
 uint32_t voltage_mask,reserved32;
};
_Static_assert(sizeof(struct storage_read_session)==864,"SD read session layout");
_Static_assert(offsetof(struct storage_read_session,identify_us)==840,"SD deadline wire offset");
int storage_read_session_valid(const struct storage_read_session *);
#endif
