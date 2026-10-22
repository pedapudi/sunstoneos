/* Byte protocol reference: linux-orangepi 2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f,
 * aic8800_bsp/{aic_bsp_driver.c,aic_bsp_driver.h,aic_bsp_txrxif.c,aicsdio.c,
 * aic_bsp_8800d80.c,aic_bsp_export.h}. See docs/radio-codec.md.
 */
#include "aic8800_sdio_codec.h"
#include "a733_range.h"

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
uint8_t aic8800_sdio_header_crc(const uint8_t header[3])
{
    uint8_t crc = 0;
    for (unsigned i = 0; i < 3; ++i) {
        crc ^= header[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint8_t)((crc << 1) ^ ((crc & 0x80u) ? 0x07u : 0u));
    }
    return crc;
}

void aic8800_sdio_command_header(uint8_t header[16], uint16_t id, uint16_t parameters)
{
    uint16_t length = (uint16_t)(12u + parameters);
    const uint8_t bytes[16] = {
        (uint8_t)length, (uint8_t)(length >> 8), 0x11, 0,
        0, 0, 0, 0, (uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 10), 0,
        100, 0, (uint8_t)parameters, (uint8_t)(parameters >> 8)
    };
    for (unsigned i = 0; i < 16; ++i)
        header[i] = bytes[i];
    header[3] = aic8800_sdio_header_crc(header);
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
    if (!address_for(which, &address) || !a733_span_excl(output, sizeof(bytes)))
        return AIC8800_CODEC_INVALID;
    if (capacity < sizeof(bytes))
        return AIC8800_CODEC_TRUNCATED;
    bytes[3] = aic8800_sdio_header_crc(bytes);
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
    if (!address_for(expected, &expected_address) || !a733_span_excl(input, available) ||
        !a733_span_excl(output, sizeof(*output)) || a733_extents_overlap_prechecked((uintptr_t)input, available, (uintptr_t)output, sizeof(*output)))
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

enum aic8800_codec_result aic8800_sdio_encode_bootstrap_setup(void *output, size_t capacity)
{
    uint8_t bytes[AIC8800_SDIO_SETUP_REQUEST_BYTES] = {
        0x14, 0x00, 0x11, 0x00, /* 20 bytes after the outer header. */
        0, 0, 0, 0, 0x02, 0x04, 0x01, 0x00, /* DBG_MEM_WRITE_REQ, TASK_DBG. */
        0x64, 0x00, 0x08, 0x00, 0, 0, 0, 0, 0, 0, 0, 0
    };
    if (!a733_span_excl(output, sizeof(bytes))) return AIC8800_CODEC_INVALID;
    if (capacity < sizeof(bytes)) return AIC8800_CODEC_TRUNCATED;
    bytes[3] = aic8800_sdio_header_crc(bytes);
    put32(bytes + 16, AIC8800_SDIO_SETUP_ADDRESS);
    put32(bytes + 20, AIC8800_SDIO_SETUP_VALUE);
    for (size_t i = 0; i < sizeof(bytes); ++i) ((uint8_t *)output)[i] = bytes[i];
    return AIC8800_CODEC_OK;
}

enum aic8800_codec_result aic8800_sdio_decode_bootstrap_setup_response(
    const void *input, size_t available, struct aic8800_sdio_response *output)
{
    struct aic8800_sdio_response r;
    if (!a733_span_excl(input, available) || !a733_span_excl(output, sizeof(*output)) ||
        a733_extents_overlap_prechecked((uintptr_t)input, available, (uintptr_t)output, sizeof(*output))) return AIC8800_CODEC_INVALID;
    enum aic8800_codec_result result = aic8800_sdio_decode_identity_response(
        input, available, AIC8800_IDENTITY_CHIP, &r);
    if (result != AIC8800_CODEC_OK && result != AIC8800_CODEC_UNRELATED) return result;
    if (r.has_message && r.message_id == AIC8800_SDIO_WRITE_CONFIRM_ID) {
        if (r.parameter_bytes != 8) return AIC8800_CODEC_MALFORMED;
        r.is_memory_write = 1; r.address = le32(r.parameters); r.value = le32(r.parameters + 4);
        *output = r; return AIC8800_CODEC_OK;
    }
    *output = r; return AIC8800_CODEC_UNRELATED;
}

enum aic8800_codec_result aic8800_sdio_identity(
    const struct aic8800_sdio_response *chip,
    const struct aic8800_sdio_response *subrevision,
    struct aic8800_radio_identity *output)
{
    struct aic8800_radio_identity result = {0};
    if (!a733_span_excl(chip, sizeof(*chip)) || !a733_span_excl(subrevision, sizeof(*subrevision)) ||
        !a733_span_excl(output, sizeof(*output)) || a733_extents_overlap_prechecked((uintptr_t)chip, sizeof(*chip), (uintptr_t)output, sizeof(*output)) ||
        a733_extents_overlap_prechecked((uintptr_t)subrevision, sizeof(*subrevision), (uintptr_t)output, sizeof(*output)))
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
