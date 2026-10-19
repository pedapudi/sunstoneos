#ifndef SUNSTONE_STORAGE_READ_SESSION_H
#define SUNSTONE_STORAGE_READ_SESSION_H
#include "a733_board_application.h"
#include "a733_pins.h"
#ifdef SUNSTONE_EMMC_READ
#define A733_STORAGE_READ_CONTRACT_MAGIC UINT64_C(0x4153373345524331)
#define A733_STORAGE_READ_SESSION_MAGIC UINT64_C(0x4153373345524131)
#define STORAGE_READ_PIN_COUNT 12
#define STORAGE_READ_SESSION_BYTES 912
#else
#define STORAGE_READ_PIN_COUNT 7
#define STORAGE_READ_SESSION_BYTES 880
#define A733_STORAGE_READ_CONTRACT_MAGIC UINT64_C(0x4153373352444331)
#define A733_STORAGE_READ_SESSION_MAGIC UINT64_C(0x4153373352444131)
#endif
struct storage_read_session {
 struct a733_board_session board;
 uint64_t magic,version,bytes,owner,authority;
 uint8_t initial[32],memory[32],supplies[32],topology[32],supervision[32];
 struct a733_pin_config expected[STORAGE_READ_PIN_COUNT],desired[STORAGE_READ_PIN_COUNT];
#ifdef SUNSTONE_EMMC_READ
 uint8_t reserved[8];
#else
 uint8_t reserved[6];
#endif
 uint64_t identify_us,read_us;
 uint32_t voltage_mask;
#ifdef SUNSTONE_EMMC_READ
 uint32_t emmc_host;
#else
 uint32_t sd_profile; /* enum sunxi_mmc_sd_profile, legacy zero preserved. */
#endif
 uint64_t application_first_sector;
 uint32_t application_blocks,application_reserved;
};
_Static_assert(sizeof(struct storage_read_session)==STORAGE_READ_SESSION_BYTES,"Read session layout");
#ifdef SUNSTONE_EMMC_READ
_Static_assert(offsetof(struct storage_read_session,identify_us)==872,"eMMC deadline wire offset");
#else
_Static_assert(offsetof(struct storage_read_session,identify_us)==840,"SD deadline wire offset");
#endif
int storage_read_session_valid(const struct storage_read_session *);
#endif
