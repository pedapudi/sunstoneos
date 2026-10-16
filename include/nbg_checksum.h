#ifndef SUNSTONE_NBG_CHECKSUM_H
#define SUNSTONE_NBG_CHECKSUM_H
#include <stddef.h>
#include <stdint.h>

enum nbg_checksum_result {
    NBG_CHECKSUM_INVALID = 0, NBG_CHECKSUM_MATCH = 1, NBG_CHECKSUM_MISMATCH = 2
};
struct nbg_checksum { uint32_t stored, computed; };

/* CRC-32/ISO-HDLC, without modifying the file. For 0x1001e, checks the complete
 * file, including gaps/trailing bytes, with bytes 108..111 treated as zero.
 * This path checks magic, version, and size; structure is validated separately.
 *
 * For 32-bit-address 0x20000, first calls nbg_model_open. Requires the packed
 * 48-byte header, 12-byte subheader, and consecutive 44-byte directory records.
 * Checks every byte after that directory, including gaps/trailing bytes, with
 * section-checksum values treated as zero. Then checks every listed section CRC.
 * The checksum table must be nonempty and reference distinct, present sections;
 * references to itself are invalid. HEADER AND DIRECTORY BYTES ARE NOT COVERED.
 * A match is not target admission, trusted model identity, or command validation.
 *
 * Caller supplies stable readable bytes, at most 256 MiB, and a writable result
 * disjoint from the file. Input may be unaligned. A stored zero is compared
 * normally; it never disables verification. MATCH returns the format's primary
 * stored/computed pair. MISMATCH returns the first failing pair, checking the
 * primary CRC before section CRCs in table order. INVALID leaves output unchanged.
 * No heap, device access, mutable static state, or synchronization. CRC detects
 * corruption; it is not authentication. Link with the native container decoder. */
enum nbg_checksum_result nbg_checksum_verify(const void *, size_t,
                                             struct nbg_checksum *);
#endif
