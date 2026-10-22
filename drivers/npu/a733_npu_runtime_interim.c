/* Interim integrity checks of the NPU runtime. Until fixed mappings make the
 * read-only group read-only at EL0 and EL1, every task can write the parent
 * allocation; these checks detect accidental CPU writes before the NPU can
 * fetch changed tables, descriptors or command bytes (T and A) and after each
 * job (H). They read through the CPU cache, so they do not detect NPU writes.
 * The fixed-mapping change deletes this file (docs/designs/npu-breadth.md). */
#include "npu_runtime.h"

/* CRC32C with the Armv8 CRC instructions. No final inversion: values are
 * compared only with references computed by this function. */
__attribute__((target("crc")))
uint32_t npu_runtime_crc32c(uint32_t crc, const void *data, size_t bytes)
{
    const unsigned char *p = data;
    while (bytes && ((uintptr_t)p & 7)) { crc = __builtin_arm_crc32cb(crc, *p++); --bytes; }
    typedef uint64_t __attribute__((may_alias)) word;
    for (; bytes >= 8; bytes -= 8, p += 8) crc = __builtin_arm_crc32cd(crc, *(const word *)p);
    while (bytes--) crc = __builtin_arm_crc32cb(crc, *p++);
    return crc;
}

int npu_runtime_tables_intact(struct npu_runtime *rt)
{
    size_t bytes = rt->native.memory.image.bytes;
    rt->counters.table_bytes += bytes;
    return npu_runtime_crc32c(UINT32_MAX, (const void *)rt->native.memory.arena, bytes) == rt->table_crc;
}

/* The slot's read-only bytes in check order: the address-bearing bytes that A
 * covers (NN descriptors, then the command stream), then the descriptor
 * padding and the coefficient allocation that only H adds. */
#define ADDRESS_SPANS 2u
#define READONLY_SPANS 4u
static void readonly_spans(const struct npu_runtime_slot *s, struct npu_runtime_span spans[READONLY_SPANS])
{
    const struct nbg_assembly_buffer *nn = &s->targets[NBG_ALLOC_NN], *coeff = &s->targets[NBG_ALLOC_COEFF];
    uint32_t descriptors = s->layout.buffers[NBG_ALLOC_NN].payload_bytes;
    spans[0] = (struct npu_runtime_span){(uintptr_t)nn->cpu, descriptors};
    spans[1] = (struct npu_runtime_span){s->command.cpu, s->stream_bytes};
    spans[2] = (struct npu_runtime_span){(uintptr_t)nn->cpu + descriptors, nn->bytes - descriptors};
    spans[3] = (struct npu_runtime_span){(uintptr_t)coeff->cpu, coeff->bytes};
}
/* One chunk of the CRC over the read-only spans, continuing the slot's
 * cursor; at the end of the address-bearing spans the running value is the
 * A reference. Returns 1 once every span is covered. */
static int sum_step(struct npu_runtime_slot *s, uint64_t *counter, int record)
{
    struct npu_runtime_span spans[READONLY_SPANS];
    readonly_spans(s, spans);
    for (int chunk = 1;; chunk = 0) {
        while (s->span < READONLY_SPANS && s->offset >= spans[s->span].bytes) {
            if (record && s->span == ADDRESS_SPANS - 1) s->address_crc = s->crc;
            ++s->span; s->offset = 0;
        }
        if (s->span == READONLY_SPANS) return 1;
        if (!chunk) return 0;
        size_t n = spans[s->span].bytes - s->offset;
        if (n > NPU_RUNTIME_READ_CHUNK) n = NPU_RUNTIME_READ_CHUNK;
        s->crc = npu_runtime_crc32c(s->crc, (const void *)(spans[s->span].cpu + s->offset), n);
        *counter += n;
        s->offset += n;
    }
}
int npu_runtime_reference_step(struct npu_runtime *rt, struct npu_runtime_slot *s)
{
    if (!sum_step(s, &rt->counters.reference_bytes, 1)) return 0;
    s->readonly_crc = s->crc;
    return 1;
}
int npu_runtime_check_step(struct npu_runtime *rt, struct npu_runtime_slot *s)
{
    if (!sum_step(s, &rt->counters.readonly_bytes, 0)) return 0;
    return s->crc == s->readonly_crc ? 1 : -1;
}

/* The controller fetches A733_NPU_FETCH_BYTES from the command address at
 * every trigger, so every byte after the stream must still be an END pair. */
int npu_runtime_stream_intact(struct npu_runtime *rt, struct npu_runtime_slot *s)
{
    struct npu_runtime_span spans[READONLY_SPANS];
    readonly_spans(s, spans);
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < ADDRESS_SPANS; ++i) {
        crc = npu_runtime_crc32c(crc, (const void *)spans[i].cpu, spans[i].bytes);
        rt->counters.address_bytes += spans[i].bytes;
    }
    if (crc != s->address_crc || (s->stream_bytes & 7) || s->stream_bytes > A733_NPU_FETCH_BYTES) return 0;
    typedef uint64_t __attribute__((may_alias)) pair;
    const pair *tail = (const pair *)s->command.cpu;
    uint64_t differ = 0;
    for (size_t i = s->stream_bytes / 8; i < A733_NPU_FETCH_BYTES / 8; ++i)
        differ |= tail[i] ^ UINT64_C(0x10000000);
    rt->counters.tail_bytes += A733_NPU_FETCH_BYTES - s->stream_bytes;
    return !differ;
}
