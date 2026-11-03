/* The image side of the card identity in the bounded-write admission: a blob names a card by its CID in its last 16 bytes; the bounds object
 * opens the write ranges only after the backup hash matches, and says the card is the named one only for that CID. Real drivers/storage/a733_storage_bounds.c. */
#include "a733_storage_bounds.h"
#include "a733_storage_digest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const uint32_t CID33[4] = {0x27504853u, 0x44333247u, 0x12000002u, 0x9d014c47u};   /* 3-3's card */
static const uint32_t CID31[4] = {0x6d4a7f01u, 0x53443634u, 0x47200062u, 0x0a6832eau};   /* a different card (3-1's is not recorded here: any other value) */
static uint8_t blob[48 + 16 * 3 + 16];
static void put64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static int read_card(void *c, uint64_t s, uint8_t *out) { (void)c; for (unsigned i = 0; i < 512; ++i) out[i] = (uint8_t)(s * 7 + i); return 1; }
static void make(const uint32_t cid[4])
{
    memset(blob, 0, sizeof blob); memcpy(blob, A733_BOUNDS_MAGIC, 8); blob[8] = 2; blob[12] = 1;
    struct storage_digest d; storage_digest_init(&d); uint8_t rec[520]; put64(rec, 5); read_card(NULL, 5, rec + 8); storage_digest_add(&d, rec, sizeof rec);
    storage_digest_end(&d, blob + 16);
    put64(blob + 48, 100); put64(blob + 56, 10); put64(blob + 64, 200); put64(blob + 72, 10); put64(blob + 80, 5); put64(blob + 88, 1);
    for (unsigned i = 0; i < 4; ++i) for (unsigned k = 0; k < 4; ++k) blob[96 + 4 * i + k] = (uint8_t)(cid[i] >> (24 - 8 * k));
}
int main(void)
{
    struct a733_storage_bounds b;
    make(CID33);
    memset(&b, 0, sizeof b);
    assert(a733_storage_bounds_parse(&b, blob, sizeof blob) && !a733_storage_bounds_card_is(&b, CID33));           /* nothing is the card before the hash has matched */
    assert(a733_storage_bounds_verify(&b, read_card, NULL));
    assert(a733_storage_bounds_card_is(&b, CID33) && a733_storage_bounds_card_ok(&b, 1, 1, CID33));                /* its own card */
    assert(!a733_storage_bounds_card_is(&b, CID31) && !a733_storage_bounds_card_ok(&b, 1, 1, CID31));              /* the other board's card */
    assert(!a733_storage_bounds_card_is(&b, NULL) && !a733_storage_bounds_card_is(NULL, CID33));
    for (unsigned w = 0; w < 4; ++w) for (unsigned bit = 0; bit < 32; ++bit) {
        uint32_t other[4]; memcpy(other, CID33, sizeof other); other[w] ^= 1u << bit;
        assert(!a733_storage_bounds_card_is(&b, other));                                                             /* every single-bit difference */
    }
    /* The identity cannot be edited after parse: the blob is re-hashed on each use. */
    blob[96] ^= 1; assert(!a733_storage_bounds_card_is(&b, CID33)); blob[96] ^= 1; assert(a733_storage_bounds_card_is(&b, CID33));
    /* A blob made for the other card does not name this one, and an old blob without an identity (no trailer) does not parse. */
    make(CID31); memset(&b, 0, sizeof b);
    assert(a733_storage_bounds_parse(&b, blob, sizeof blob) && a733_storage_bounds_verify(&b, read_card, NULL));
    assert(a733_storage_bounds_card_is(&b, CID31) && !a733_storage_bounds_card_is(&b, CID33));
    memset(&b, 0, sizeof b); assert(!a733_storage_bounds_parse(&b, blob, sizeof blob - 16));
    puts("PASS: the bounds blob names its card; only that CID is accepted (every single-bit difference refused), the identity is unreadable before the hash matches and cannot be edited after parse, a blob without one does not parse");
    return 0;
}
