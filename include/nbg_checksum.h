#ifndef SUNSTONE_NBG_CHECKSUM_H
#define SUNSTONE_NBG_CHECKSUM_H
#include <stddef.h>
#include <stdint.h>

enum nbg_checksum_result {
    NBG_CHECKSUM_INVALID = 0, NBG_CHECKSUM_MATCH = 1, NBG_CHECKSUM_MISMATCH = 2
};
struct nbg_checksum { uint32_t stored, computed; };

/* Check the complete 0x1001e file with CRC-32/ISO-HDLC, treating bytes 108..111
 * (the stored checksum) as zero. Does not modify the file. Covers padding and
 * trailing bytes, not only declared sections. A stored zero is compared normally;
 * it does not disable verification. Only NBG magic, version, and file-size limits
 * are checked here. Container structure and execution admission remain separate.
 *
 * Caller supplies stable readable bytes, 592..256 MiB, and a writable result
 * disjoint from the file. Input may be unaligned. MATCH and MISMATCH publish both
 * values. INVALID leaves result unchanged. No heap, device access, mutable static
 * state, or synchronization. CRC detects corruption; it is not authentication. */
enum nbg_checksum_result nbg_checksum_verify(const void *, size_t,
                                             struct nbg_checksum *);
#endif
