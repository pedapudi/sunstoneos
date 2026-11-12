#include <stdint.h>
#include "gate-info.h"
__attribute__((used,retain,section(".rodata.profile")))
const struct {
    uint64_t magic,version,bytes,address,gate_bytes,state_offset,test_offset;
    uint8_t sha256[32];
} a733_card_fel_profile={UINT64_C(0x314c454644524143),1,88,0x41001000,
    CARD_FEL_GATE_BYTES,CARD_FEL_STATE_OFFSET,CARD_FEL_TEST_OFFSET,CARD_FEL_SHA256};
_Static_assert(sizeof(a733_card_fel_profile)==88,"card recovery profile geometry");
