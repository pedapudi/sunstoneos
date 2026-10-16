#include "aic8800_sdio_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

extern uint8_t reference_header_crc(uint8_t *, uint16_t);

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}

static void confirmation(uint8_t *p, uint32_t address, uint32_t value)
{
    /* Source format: 4-byte transport + 12-byte IPC + 8-byte confirmation. */
    memset(p, 0xa5, 24);
    put16(p, 20); p[2] = 0x91; /* Bit7 is ignored by the reference. */
    put16(p + 4, 0x0401); put16(p + 10, 8);
    put32(p + 16, address); put32(p + 20, value);
}

static void unchanged(const uint8_t *p, size_t bytes,
                      enum aic8800_codec_result expected)
{
    struct aic8800_sdio_response output, before;
    memset(&output, 0x6b, sizeof(output));
    memcpy(&before, &output, sizeof(output));
    assert(aic8800_sdio_decode_identity_response(p, bytes, AIC8800_IDENTITY_CHIP,
                                                &output) == expected);
    assert(memcmp(&output, &before, sizeof(output)) == 0);
}

static void requests(void)
{
    uint8_t storage[23], before[23];
    const uint8_t expected[20] = {
        0x10, 0x00, 0x11, 0x00, 0, 0, 0, 0,
        0x00, 0x04, 0x01, 0x00, 0x64, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x50, 0x40
    };
    memset(storage, 0x79, sizeof(storage));
    assert(aic8800_sdio_encode_identity_read(AIC8800_IDENTITY_CHIP, storage + 1, 21)
           == AIC8800_CODEC_OK);
    for (size_t i = 0; i < sizeof(expected); ++i)
        if (i != 3) assert(storage[i + 1] == expected[i]);
    assert(storage[4] == reference_header_crc(storage + 1, 3));
    assert(storage[0] == 0x79 && storage[21] == 0x79 && storage[22] == 0x79);
    memcpy(before, storage, sizeof(before));
    assert(aic8800_sdio_encode_identity_read(AIC8800_IDENTITY_CHIP, storage + 1, 19)
           == AIC8800_CODEC_TRUNCATED);
    assert(memcmp(storage, before, sizeof(storage)) == 0);
    assert(aic8800_sdio_encode_identity_read((enum aic8800_identity_register)2, storage, 20)
           == AIC8800_CODEC_INVALID);
    assert(memcmp(storage, before, sizeof(storage)) == 0);
    assert(aic8800_sdio_encode_identity_read(AIC8800_IDENTITY_SUBREV, storage + 1, 20)
           == AIC8800_CODEC_OK);
    assert(storage[17] == 4 && storage[18] == 0 && storage[19] == 0 && storage[20] == 0);
}

static void responses_and_identity(void)
{
    uint8_t storage[53];
    uint8_t *p = storage + 1; /* Input deliberately lacks word alignment. */
    struct aic8800_sdio_response chip, subrev, unrelated;
    struct aic8800_radio_identity identity, before;
    memset(storage, 0x9c, sizeof(storage));
    confirmation(p, AIC8800_SDIO_CHIP_ADDRESS, 0x00c31234);
    confirmation(p + 24, AIC8800_SDIO_SUBREV_ADDRESS, 0x00003ab0);
    assert(aic8800_sdio_decode_identity_response(p, 48, AIC8800_IDENTITY_CHIP, &chip)
           == AIC8800_CODEC_OK);
    assert(chip.consumed == 24 && chip.record_bytes == 24 && chip.record == p);
    assert(chip.parameters == p + 16 && chip.parameter_bytes == 8);
    assert(chip.has_message && chip.is_memory_read && chip.transport_type == 0x91);
    assert(aic8800_sdio_decode_identity_response(p + chip.consumed, 24,
           AIC8800_IDENTITY_SUBREV, &subrev) == AIC8800_CODEC_OK);
    /* Decode scalars after original byte storage changes. No borrowed dereference. */
    memset(p, 0, 48);
    assert(aic8800_sdio_identity(&chip, &subrev, &identity) == AIC8800_CODEC_OK);
    assert(identity.chip_word == 0x00c31234 && identity.subrevision_word == 0x3ab0);
    assert(identity.revision == 3 && identity.is_h && identity.subrevision == 0xab);
    assert(identity.known_revision);
    chip.value = 0x00421234;
    assert(aic8800_sdio_identity(&chip, &subrev, &identity) == AIC8800_CODEC_OK);
    assert(identity.revision == 2 && !identity.is_h && !identity.known_revision);
    before = identity;
    subrev.address = AIC8800_SDIO_CHIP_ADDRESS;
    assert(aic8800_sdio_identity(&chip, &subrev, &identity) == AIC8800_CODEC_INVALID);
    assert(memcmp(&identity, &before, sizeof(identity)) == 0);

    confirmation(p, AIC8800_SDIO_SUBREV_ADDRESS, 7);
    assert(aic8800_sdio_decode_identity_response(p, 24, AIC8800_IDENTITY_CHIP, &unrelated)
           == AIC8800_CODEC_UNRELATED);
    assert(unrelated.is_memory_read && unrelated.address == 4 && unrelated.value == 7);
    put16(p + 4, 0x0420);
    assert(aic8800_sdio_decode_identity_response(p, 24, AIC8800_IDENTITY_CHIP, &unrelated)
           == AIC8800_CODEC_UNRELATED);
    assert(unrelated.has_message && !unrelated.is_memory_read && unrelated.message_id == 0x0420);
    assert(unrelated.parameters == p + 16 && unrelated.parameter_bytes == 8);
    p[2] = 0x12; put16(p, 1);
    assert(aic8800_sdio_decode_identity_response(p, 8, AIC8800_IDENTITY_CHIP, &unrelated)
           == AIC8800_CODEC_UNRELATED);
    assert(!unrelated.has_message && unrelated.parameters == NULL);
    assert(unrelated.record_bytes == 5 && unrelated.consumed == 8);
    unchanged(p, 7, AIC8800_CODEC_TRUNCATED);

    /* Unspecified payload/padding bytes are not invented validation fields. */
    confirmation(p, AIC8800_SDIO_CHIP_ADDRESS, 0);
    put16(p, 21);
    assert(aic8800_sdio_decode_identity_response(p, 28, AIC8800_IDENTITY_CHIP, &chip)
           == AIC8800_CODEC_OK);
    assert(chip.record_bytes == 25 && chip.consumed == 28);
}

static void invalid_records(void)
{
    uint8_t p[32];
    confirmation(p, AIC8800_SDIO_CHIP_ADDRESS, 0);
    for (size_t n = 0; n < 24; ++n) unchanged(p, n, AIC8800_CODEC_TRUNCATED);
    put16(p, 0xffff); unchanged(p, sizeof(p), AIC8800_CODEC_TRUNCATED);
    put16(p, 8); unchanged(p, 12, AIC8800_CODEC_MALFORMED);
    put16(p, 20); put16(p + 10, 9); unchanged(p, 24, AIC8800_CODEC_MALFORMED);
    put16(p + 10, 7); unchanged(p, 24, AIC8800_CODEC_MALFORMED);
    put16(p + 10, 1025); unchanged(p, 24, AIC8800_CODEC_MALFORMED);
    put16(p, 0); unchanged(p, 4, AIC8800_CODEC_END);
    put16(p, 20); p[2] = 1; unchanged(p, 24, AIC8800_CODEC_UNSUPPORTED);
    union {
        struct aic8800_sdio_response output;
        uint8_t bytes[sizeof(struct aic8800_sdio_response)];
    } alias;
    uint8_t before[sizeof(alias)];
    memset(&alias, 0x81, sizeof(alias));
    confirmation(alias.bytes, AIC8800_SDIO_CHIP_ADDRESS, 0);
    memcpy(before, &alias, sizeof(alias));
    assert(aic8800_sdio_decode_identity_response(alias.bytes, sizeof(alias),
           AIC8800_IDENTITY_CHIP, &alias.output) == AIC8800_CODEC_INVALID);
    assert(memcmp(&alias, before, sizeof(alias)) == 0);
    assert(aic8800_sdio_decode_identity_response(NULL, 0, AIC8800_IDENTITY_CHIP,
           &alias.output) == AIC8800_CODEC_INVALID);
}

int main(void)
{
    requests();
    responses_and_identity();
    invalid_records();
    puts("radio codec: requests, retained responses, identity, malformed bounds passed");
    return 0;
}
