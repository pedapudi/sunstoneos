#ifndef SUNSTONE_NBG_RELOCATION_H
#define SUNSTONE_NBG_RELOCATION_H
#include <stddef.h>
#include <stdint.h>

#define NBG_RELOCATION_MAX_PATCHES 4096u

enum nbg_address_encoding {
    NBG_ADDRESS_WORD = 0,
    NBG_ADDRESS_SHIFT6 = 1,
    NBG_ADDRESS_FLAGS6 = 2,
    NBG_ADDRESS_END_SHIFT6 = 3
};

/* Resolved address reference, not a serialized NBG patch record. The loader
 * supplies the destination offset and the source allocation independently of
 * model-controlled values. referenced_bytes covers the entire device access,
 * including padding; logical tensor bytes alone do not establish this bound. */
struct nbg_relocation {
    uint64_t source_bytes;
    uint64_t referenced_bytes;
    uint32_t source_device;
    uint32_t source_offset;
    uint32_t destination_offset;
    enum nbg_address_encoding encoding;
};

/* Patch an OFFLINE, exclusively owned byte buffer for a 32-bit device address
 * space. Destination words are little-endian and have four-byte offsets. The
 * CPU buffer itself need not be aligned. SHIFT6 requires a 64-byte-aligned
 * address and preserves destination bits 31:26. FLAGS6 requires a 64-byte-
 * aligned address and preserves destination bits 5:0. WORD replaces all 32 bits.
 * END_SHIFT6 validates the nonempty span at source_offset, then encodes its
 * exclusive end shifted by six, preserving destination bits 31:26. The end may
 * equal the allocation end, but must be 64-byte aligned and fit in 32 bits.
 *
 * Refuse empty/out-of-range allocations and accesses, address zero, unknown
 * encodings, duplicate destinations, metadata/buffer aliases, wrapping CPU
 * ranges, and counts outside 1..MAX_PATCHES. Refusal leaves the buffer unchanged.
 * Caller keeps the buffer and metadata stable throughout validation and write.
 * No allocation, cache operations, MMIO, or synchronization occurs here.
 *
 * This does NOT parse NBG records, resolve source kinds, validate commands,
 * establish mappings or buffer ownership, or authorize submission. The caller
 * must establish those properties separately. Wider addresses and split high
 * address fields are unsupported. Returns 1 on success, 0 on refusal. */
int nbg_relocate(void *buffer, size_t bytes,
                 const struct nbg_relocation *patches, size_t count);
#endif
