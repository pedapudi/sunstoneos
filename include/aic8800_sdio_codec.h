#ifndef SUNSTONE_AIC8800_SDIO_CODEC_H
#define SUNSTONE_AIC8800_SDIO_CODEC_H

#include <stddef.h>
#include <stdint.h>

#define AIC8800_SDIO_IDENTITY_REQUEST_BYTES 20u
#define AIC8800_SDIO_SETUP_REQUEST_BYTES 24u
#define AIC8800_SDIO_SETUP_ADDRESS UINT32_C(0x40500058)
#define AIC8800_SDIO_SETUP_VALUE UINT32_C(0x00000040)
#define AIC8800_SDIO_WRITE_CONFIRM_ID 0x0403u
#define AIC8800_SDIO_CHIP_ADDRESS UINT32_C(0x40500000)
#define AIC8800_SDIO_SUBREV_ADDRESS UINT32_C(0x00000004)
#define AIC8800_SDIO_READ_CONFIRM_ID 0x0401u
#define AIC8800_SDIO_MAX_PARAMETERS 1024u

enum aic8800_identity_register {
    AIC8800_IDENTITY_CHIP,
    AIC8800_IDENTITY_SUBREV
};

enum aic8800_codec_result {
    AIC8800_CODEC_OK,
    AIC8800_CODEC_UNRELATED,
    AIC8800_CODEC_END,
    AIC8800_CODEC_INVALID,
    AIC8800_CODEC_TRUNCATED,
    AIC8800_CODEC_MALFORMED,
    AIC8800_CODEC_UNSUPPORTED
};

/* CRC-8 of the first three bytes of a 4-byte transfer header. */
uint8_t aic8800_sdio_header_crc(const uint8_t header[3]);
/* Write the 16-byte host command header: length, type 0x11, CRC, zero word,
 * message ID, destination task ID >> 10, DRV_TASK_ID 100, parameter length.
 * Parameters follow at offset 16; parameters + 12 must fit in 16 bits. */
void aic8800_sdio_command_header(uint8_t header[16], uint16_t id, uint16_t parameters);

/* Encode exactly 20 bytes. The unused destination tail remains unchanged.
 * No allocation, hardware access, CMD53 block padding, or setup writes occur.
 * The complete board identity sequence needs additional setup, including the
 * reference's write of 0x40 to radio address 0x40500058 before these reads.
 * Output remains unchanged on failure. Buffers need no alignment.
 */
enum aic8800_codec_result aic8800_sdio_encode_identity_read(
    enum aic8800_identity_register which, void *output, size_t capacity);

struct aic8800_sdio_response {
    /* Borrowed bytes for future dispatch; valid while input storage is alive.
     * record_bytes excludes alignment padding; consumed includes that padding.
     * Copy bytes into owned storage before retaining asynchronous work.
     */
    const uint8_t *record;
    size_t record_bytes;
    size_t consumed;
    uint8_t transport_type; /* Original byte, including the source-ignored bit7. */
    uint8_t has_message;
    uint8_t is_memory_read;
    uint16_t message_id;
    const uint8_t *parameters; /* Borrowed; NULL for other control subtypes. */
    uint16_t parameter_bytes;
    uint32_t address;
    uint32_t value;
    uint8_t is_memory_write; /* Set only by the bootstrap-setup decoder. */
};

/* Decode one control record from an SDIO receive aggregate.
 * OK matches the requested identity address. UNRELATED retains a valid control
 * record, including a read confirmation for another address, for later dispatch.
 * A zero outer length returns END. Non-control data returns UNSUPPORTED because
 * that transport has a different framing rule. Neither result consumes input.
 * All other failures leave output unchanged. OK/UNRELATED publish output only
 * after validation. Output must not overlap any byte of the supplied input.
 * RX byte3, dummy routing IDs, IPC pattern, and padding have no checked invariant
 * in the reference. They are not compared with invented constants or a TX CRC.
 */
enum aic8800_codec_result aic8800_sdio_decode_identity_response(
    const void *input, size_t available, enum aic8800_identity_register expected,
    struct aic8800_sdio_response *output);

/* Fixed bootstrap operation only: encode DBG_MEM_WRITE_REQ for
 * 0x40500058=0x40. No caller-selected address/value or hardware access. Exactly
 * 24 bytes; destination tail and rejected output remain unchanged.
 */
enum aic8800_codec_result aic8800_sdio_encode_bootstrap_setup(void *output, size_t capacity);

/* Decode one control record for the single outstanding bootstrap write.
 * Write CFM requires exactly eight parameters. Address/data fields are retained,
 * not compared: the published sender ignores them and establishes no echo rule.
 * OK correlates by message ID under the caller's exclusive, no-stale-message
 * admission. It is not register readback or independent proof of the write.
 * Other valid control messages remain UNRELATED. Borrowing, padding and rejected
 * output rules match the identity decoder above.
 */
enum aic8800_codec_result aic8800_sdio_decode_bootstrap_setup_response(
    const void *input, size_t available, struct aic8800_sdio_response *output);

struct aic8800_radio_identity {
    uint32_t chip_word;
    uint32_t subrevision_word;
    uint8_t revision;
    uint8_t subrevision;
    uint8_t is_h;
    uint8_t known_revision; /* Source accepts 1, 3, 7, or 15. */
};

/* Decode address-correlated register values without claiming physical identity.
 * The caller must first identify SDIO c8a1:0182 and retain two confirmed reads.
 * Unknown revisions remain visible with known_revision=0; no firmware is chosen.
 * Output is unchanged on bad addresses, NULL pointers, or overlapping objects.
 * Only address/value/is_memory_read are consulted; borrowed packet storage may
 * have expired if those scalar fields were copied into owned response objects.
 */
enum aic8800_codec_result aic8800_sdio_identity(
    const struct aic8800_sdio_response *chip,
    const struct aic8800_sdio_response *subrevision,
    struct aic8800_radio_identity *output);

#endif
