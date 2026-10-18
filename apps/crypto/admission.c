#include "crypto_session.h"
#include "a733_rail_leases.h"
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
int crypto_session_valid(const struct crypto_session *s)
{
 uint8_t digest[32];
 if(!s||!a733_board_session_valid(&s->board,0)||s->board.workload!=2||
 s->magic!=A733_CRYPTO_SESSION_MAGIC||s->version!=1||s->bytes!=sizeof(*s)||
 !s->owner||s->owner==s->board.pmic_owner||s->owner==s->board.usb0_owner||!s->authority||
 !present(s->initial,32)||!present(s->memory,32)||!present(s->clocks,32)||!present(s->supervision,32))return 0;
 storage_digest_bytes(crypto_inputs,83,digest);if(memcmp(digest,s->fixture,32))return 0;
 storage_digest_bytes(crypto_reference,160,digest);return !memcmp(digest,s->reference,32);
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct crypto_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
    if(!c||!crypto_session_valid(s)||c->magic!=A733_CRYPTO_CONTRACT_MAGIC||
       c->reserved[1]!=sizeof(*s))return 0;
    uint8_t hash[32];storage_digest_bytes(s,sizeof(*s),hash);
    if(memcmp(hash,&c->reserved[2],32))return 0;
    struct a733_smp_contract base=*c;
    base.magic=A733_BOARD_CONTRACT_MAGIC;base.reserved[1]=sizeof(s->board);
    storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
    return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
