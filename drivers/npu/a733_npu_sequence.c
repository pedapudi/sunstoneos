#include "a733_npu_sequence.h"
#include "a733_npu_controller.h"
#include "a733_range.h"
#include <string.h>

static const uint32_t tail[] = {
    0x08010e02, 0x30000701, 0x48000000, 0x30000701,
    0x08010e03, 0xc23, 0x08010e01, 0x40, 0x10000000, 0, 0, 0, 0, 0
};
static int overlap(const void *a, size_t an, const void *b, size_t bn)
{ return a733_extents_intersect((uintptr_t)a, an, (uintptr_t)b, bn); }
int a733_npu_sequence_prepare(void *destination, size_t capacity,
    const struct a733_npu_sequence_part *parts, size_t count, size_t *stream_bytes)
{
    if (!destination || !parts || !count || !stream_bytes ||
        ((uintptr_t)destination & 63) || (capacity & 7) || capacity < A733_NPU_FETCH_BYTES ||
        !a733_span_incl_nonempty(destination, capacity) || count > SIZE_MAX / sizeof(*parts) ||
        !a733_span_incl_nonempty(parts, count * sizeof(*parts)) ||
        !a733_span_incl_nonempty(stream_bytes, sizeof(*stream_bytes)) ||
        overlap(destination, capacity, parts, count * sizeof(*parts)) ||
        overlap(destination, capacity, stream_bytes, sizeof(*stream_bytes)) ||
        overlap(parts, count * sizeof(*parts), stream_bytes, sizeof(*stream_bytes))) return 0;
    size_t total = 32;
    for (size_t i = 0; i < count; ++i) {
        const struct a733_npu_sequence_part *p = &parts[i];
        if (!a733_span_incl_nonempty(p->cpu, p->bytes) || (p->bytes & 7) || p->bytes < sizeof(tail) ||
            p->bytes - 32 > A733_NPU_FETCH_BYTES - total ||
            overlap(destination, capacity, p->cpu, p->bytes) ||
            overlap(stream_bytes, sizeof(*stream_bytes), p->cpu, p->bytes) ||
            memcmp((const unsigned char *)p->cpu + p->bytes - sizeof(tail), tail, sizeof(tail))) return 0;
        total += p->bytes - 32;
    }
    /* Slow construction only: every fetch byte outside the live stream is
     * a safe END pair before the sequence is registered and published. */
    for (size_t at = 0; at < capacity; at += 8) {
        const uint64_t end = 0x10000000;
        memcpy((unsigned char *)destination + at, &end, sizeof end);
    }
    size_t at = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t n = parts[i].bytes - 32;
        memcpy((unsigned char *)destination + at, parts[i].cpu, n); at += n;
    }
    memcpy((unsigned char *)destination + at, (const unsigned char *)tail + sizeof(tail) - 32, 32);
    *stream_bytes = total;
    return 1;
}
