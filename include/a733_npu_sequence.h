#ifndef SUNSTONE_A733_NPU_SEQUENCE_H
#define SUNSTONE_A733_NPU_SEQUENCE_H
#include <stddef.h>
/* Loader-only preparation of a resident sequence. Each part is an already
 * admitted native command stream with the standard flush/stall, event-zero
 * and END footer. Buffers, descriptors and mappings referenced by every part
 * remain resident for the sequence's lifetime. This is not a command validator
 * or a dependency planner; callers choose compatible buffer dependencies.
 *
 * Preserve each part's final device flush/stall, omit its CPU event/END and
 * append the next part. Only the final part emits completion. No live command
 * is modified. Sources must not overlap destination or result. Destination
 * is 64-byte aligned and covers the complete hardware fetch span, with END
 * padding. All validation precedes writes; returns 0 without changing output. */
struct a733_npu_sequence_part { const void *cpu; size_t bytes; };
int a733_npu_sequence_prepare(void *destination, size_t capacity,
    const struct a733_npu_sequence_part *, size_t count, size_t *stream_bytes);
#endif
