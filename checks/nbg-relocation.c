#include "nbg_relocation.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char storage[34], original[34];
static struct nbg_relocation p[2];
static unsigned refusals;

static void refuse(size_t bytes, size_t count)
{
    assert(!nbg_relocate(storage + 1, bytes, p, count));
    assert(!memcmp(storage, original, sizeof(storage)));
    ++refusals;
}

static void baseline(void)
{
    memset(storage, 0xa5, sizeof(storage));
    memcpy(original, storage, sizeof(storage));
    p[0] = (struct nbg_relocation){4096, 64, 0x10000000, 0, 0, NBG_ADDRESS_WORD};
    p[1] = (struct nbg_relocation){4096, 64, 0x20000000, 64, 28, NBG_ADDRESS_SHIFT6};
}

int main(void)
{
    baseline();
    assert(nbg_relocate(storage + 1, 32, p, 2));
    const unsigned char word[] = {0, 0, 0, 0x10};
    const unsigned char shifted[] = {1, 0, 0x80, 0xa4};
    assert(!memcmp(storage + 1, word, 4));
    assert(!memcmp(storage + 29, shifted, 4));
    assert(storage[0] == 0xa5 && storage[33] == 0xa5);
    assert(!memcmp(storage + 5, original + 5, 24));
    baseline();
    refuse(32, 0); refuse(32, NBG_RELOCATION_MAX_PATCHES + 1);
    refuse(0, 2); refuse(31, 2);
    p[1].destination_offset = 0; refuse(32, 2);
    p[1].destination_offset = 1; refuse(32, 2);
    p[1].destination_offset = UINT32_MAX; refuse(32, 2);
    baseline(); p[1].source_device = 0; refuse(32, 2);
    baseline(); p[1].source_device = UINT32_MAX; refuse(32, 2);
    baseline(); p[1].source_bytes = 0; refuse(32, 2);
    baseline(); p[1].source_bytes = UINT64_MAX; refuse(32, 2);
    baseline(); p[1].source_offset = 4096; refuse(32, 2);
    baseline(); p[1].source_offset = UINT32_MAX; refuse(32, 2);
    baseline(); p[1].referenced_bytes = 0; refuse(32, 2);
    baseline(); p[1].referenced_bytes = 4096; refuse(32, 2);
    baseline(); p[1].referenced_bytes = UINT64_MAX; refuse(32, 2);
    baseline(); p[1].source_offset = 1; refuse(32, 2);
    baseline(); p[1].encoding = (enum nbg_address_encoding)2; refuse(32, 2);
    baseline(); p[1].encoding = (enum nbg_address_encoding)-1; refuse(32, 2);
    assert(!nbg_relocate(NULL, 32, p, 2));
    assert(!nbg_relocate(storage, 32, NULL, 2));
    assert(!nbg_relocate((void *)(UINTPTR_MAX - 3), 4, p, 2));
    assert(!nbg_relocate(storage, 32,
                         (void *)(UINTPTR_MAX - sizeof(*p)), 2));
    struct nbg_relocation snapshot[2];
    memcpy(snapshot, p, sizeof(p));
    assert(!nbg_relocate(p, sizeof(p), p, 2));
    assert(!memcmp(snapshot, p, sizeof(p)));
    assert(!memcmp(storage, original, sizeof(storage)));

    /* Inclusive last device byte and last aligned address remain representable. */
    baseline(); p[0] = (struct nbg_relocation){64, 1, 0xffffffc0, 63, 0, NBG_ADDRESS_WORD};
    p[1] = (struct nbg_relocation){64, 64, 0xffffffc0, 0, 28, NBG_ADDRESS_SHIFT6};
    assert(nbg_relocate(storage + 1, 32, p, 2));
    const unsigned char last[] = {0xff, 0xff, 0xff, 0xff};
    const unsigned char last_shifted[] = {0xff, 0xff, 0xff, 0xa7};
    assert(!memcmp(storage + 1, last, 4));
    assert(!memcmp(storage + 29, last_shifted, 4));

    /* The cap and quadratic duplicate check are exercised on a full batch. */
    static struct nbg_relocation many[NBG_RELOCATION_MAX_PATCHES];
    static unsigned char large[NBG_RELOCATION_MAX_PATCHES * 4];
    for (unsigned i = 0; i < NBG_RELOCATION_MAX_PATCHES; ++i)
        many[i] = (struct nbg_relocation){4096, 4, 4096, 0, i * 4, NBG_ADDRESS_WORD};
    assert(nbg_relocate(large, sizeof(large), many, NBG_RELOCATION_MAX_PATCHES));
    for (unsigned i = 0; i < NBG_RELOCATION_MAX_PATCHES; ++i)
        assert(large[i * 4] == 0 && large[i * 4 + 1] == 16 &&
               large[i * 4 + 2] == 0 && large[i * 4 + 3] == 0);
    printf("PASS relocation: encoding, bounds, guards, atomic refusal (%u cases), full batch\n", refusals);
}
