/* Byte protocol reference: linux-orangepi 2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f,
 * aic8800_bsp/{aic_bsp_driver.c,aic_bsp_driver.h,aic_bsp_txrxif.c,aicsdio.c,
 * aic_bsp_8800d80.c,aic_bsp_export.h}. See docs/radio-codec.md.
 */
#include "aic8800_sdio_codec.h"

static int span(const void *p, size_t bytes)
{
    return p != NULL && bytes <= UINTPTR_MAX - (uintptr_t)p;
}

static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t av = (uintptr_t)a, bv = (uintptr_t)b;
    return av < bv + bn && bv < av + an;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int address_for(enum aic8800_identity_register which, uint32_t *address)
{
    if (which == AIC8800_IDENTITY_CHIP)
        *address = AIC8800_SDIO_CHIP_ADDRESS;
    else if (which == AIC8800_IDENTITY_SUBREV)
        *address = AIC8800_SDIO_SUBREV_ADDRESS;
    else
        return 0;
    return 1;
}

/* CRC-8, polynomial 0x107, initial value zero, no reflection or final XOR. */
static uint8_t header_crc(const uint8_t *header)
{
    uint8_t crc = 0;
    for (unsigned i = 0; i < 3; ++i) {
        crc ^= header[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint8_t)((crc << 1) ^ ((crc & 0x80u) ? 0x07u : 0u));
    }
    return crc;
}

enum aic8800_codec_result aic8800_sdio_encode_identity_read(
    enum aic8800_identity_register which, void *output, size_t capacity)
{
    uint32_t address;
    uint8_t bytes[AIC8800_SDIO_IDENTITY_REQUEST_BYTES] = {
        0x10, 0x00, 0x11, 0x00, /* 16 bytes after outer header; command type. */
        0x00, 0x00, 0x00, 0x00, /* Dummy word. */
        0x00, 0x04, 0x01, 0x00, /* DBG_MEM_READ_REQ, TASK_DBG. */
        0x64, 0x00, 0x04, 0x00, /* DRV_TASK_ID, four-byte payload. */
        0x00, 0x00, 0x00, 0x00
    };
    if (!address_for(which, &address) || !span(output, sizeof(bytes)))
        return AIC8800_CODEC_INVALID;
    if (capacity < sizeof(bytes))
        return AIC8800_CODEC_TRUNCATED;
    bytes[3] = header_crc(bytes);
    put32(bytes + 16, address);
    for (size_t i = 0; i < sizeof(bytes); ++i)
        ((uint8_t *)output)[i] = bytes[i];
    return AIC8800_CODEC_OK;
}

enum aic8800_codec_result aic8800_sdio_decode_identity_response(
    const void *input, size_t available, enum aic8800_identity_register expected,
    struct aic8800_sdio_response *output)
{
    const uint8_t *bytes = input;
    struct aic8800_sdio_response result = {0};
    uint32_t expected_address;
    size_t payload, consumed;
    if (!address_for(expected, &expected_address) || !span(input, available) ||
        !span(output, sizeof(*output)) || overlap(input, available, output, sizeof(*output)))
        return AIC8800_CODEC_INVALID;
    if (available < 4)
        return AIC8800_CODEC_TRUNCATED;
    payload = le16(bytes);
    if (payload == 0)
        return AIC8800_CODEC_END;
    if ((bytes[2] & 0x10u) == 0)
        return AIC8800_CODEC_UNSUPPORTED;
    consumed = 4u + ((payload + 3u) & ~(size_t)3u);
    if (consumed > available)
        return AIC8800_CODEC_TRUNCATED;
    result.record = bytes;
    result.record_bytes = payload + 4u;
    result.consumed = consumed;
    result.transport_type = bytes[2];
    if ((bytes[2] & 0x7fu) != 0x11u) {
        *output = result;
        return AIC8800_CODEC_UNRELATED;
    }
    if (payload < 12)
        return AIC8800_CODEC_MALFORMED;
    result.message_id = le16(bytes + 4);
    result.parameter_bytes = le16(bytes + 10);
    if (result.parameter_bytes > AIC8800_SDIO_MAX_PARAMETERS ||
        result.parameter_bytes > payload - 12u)
        return AIC8800_CODEC_MALFORMED;
    result.has_message = 1;
    result.parameters = bytes + 16;
    if (result.message_id != AIC8800_SDIO_READ_CONFIRM_ID) {
        *output = result;
        return AIC8800_CODEC_UNRELATED;
    }
    if (result.parameter_bytes != 8)
        return AIC8800_CODEC_MALFORMED;
    result.is_memory_read = 1;
    result.address = le32(result.parameters);
    result.value = le32(result.parameters + 4);
    *output = result;
    return result.address == expected_address ? AIC8800_CODEC_OK : AIC8800_CODEC_UNRELATED;
}

enum aic8800_codec_result aic8800_sdio_identity(
    const struct aic8800_sdio_response *chip,
    const struct aic8800_sdio_response *subrevision,
    struct aic8800_radio_identity *output)
{
    struct aic8800_radio_identity result = {0};
    if (!span(chip, sizeof(*chip)) || !span(subrevision, sizeof(*subrevision)) ||
        !span(output, sizeof(*output)) || overlap(chip, sizeof(*chip), output, sizeof(*output)) ||
        overlap(subrevision, sizeof(*subrevision), output, sizeof(*output)))
        return AIC8800_CODEC_INVALID;
    if (!chip->is_memory_read || !subrevision->is_memory_read ||
        chip->address != AIC8800_SDIO_CHIP_ADDRESS ||
        subrevision->address != AIC8800_SDIO_SUBREV_ADDRESS)
        return AIC8800_CODEC_INVALID;
    result.chip_word = chip->value;
    result.subrevision_word = subrevision->value;
    result.revision = (uint8_t)((chip->value >> 16) & 0x3fu);
    result.is_h = (uint8_t)(((chip->value >> 16) & 0xc0u) == 0xc0u);
    result.subrevision = (uint8_t)(subrevision->value >> 4);
    result.known_revision = result.revision == 1 || result.revision == 3 ||
                            result.revision == 7 || result.revision == 15;
    *output = result;
    return AIC8800_CODEC_OK;
}
