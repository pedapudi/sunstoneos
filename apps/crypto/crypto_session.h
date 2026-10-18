#ifndef SUNSTONE_CRYPTO_SESSION_H
#define SUNSTONE_CRYPTO_SESSION_H
#include "a733_board_application.h"
#define A733_CRYPTO_CONTRACT_MAGIC UINT64_C(0x4153373343524331)
#define A733_CRYPTO_SESSION_MAGIC UINT64_C(0x4153373343524131)
#define CRYPTO_OUTPUT_BYTES 160u
struct crypto_session {
 struct a733_board_session board;
 uint64_t magic,version,bytes,owner,authority;
 uint8_t initial[32],memory[32],clocks[32],supervision[32],fixture[32],reference[32];
};
_Static_assert(sizeof(struct crypto_session)==824,"Crypto wire layout");
int crypto_session_valid(const struct crypto_session *);
extern const uint8_t crypto_inputs[83],crypto_reference[160];
#endif
