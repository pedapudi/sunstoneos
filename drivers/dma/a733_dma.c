#include "a733_dma.h"

#define DATA_LIMIT (UINT64_C(1) << 35)
#define DESCRIPTOR_LIMIT UINT64_C(0xfffff800)

_Static_assert(sizeof(struct a733_dma_descriptor) == 64, "descriptor stride");

/* Call only with nonempty, validated ranges that cannot wrap. */
static int overlap(uint64_t a, uint64_t a_bytes, uint64_t b, uint64_t b_bytes)
{
    return a < b + b_bytes && b < a + a_bytes;
}

static uint64_t endpoint_bytes(const struct a733_dma_endpoint *endpoint,
                               uint32_t bytes)
{
    return endpoint->request ? endpoint->width : bytes;
}

static enum a733_dma_result validate_endpoint(
    const struct a733_dma_endpoint *endpoint, uint32_t bytes)
{
    if (endpoint->request > 63 ||
        (endpoint->width != 1 && endpoint->width != 2 && endpoint->width != 4) ||
        (endpoint->burst != 1 && endpoint->burst != 4 &&
         endpoint->burst != 8 && endpoint->burst != 16))
        return A733_DMA_FORMAT;
    if (endpoint->address % endpoint->width || bytes % endpoint->width)
        return A733_DMA_ALIGNMENT;
    if (endpoint->address >= DATA_LIMIT ||
        endpoint_bytes(endpoint, bytes) > DATA_LIMIT - endpoint->address)
        return A733_DMA_ADDRESS;
    return A733_DMA_OK;
}

static uint32_t endpoint_configuration(const struct a733_dma_endpoint *endpoint)
{
    unsigned width = endpoint->width == 4 ? 2 : endpoint->width == 2 ? 1 : 0;
    unsigned burst = endpoint->burst == 16 ? 3 : endpoint->burst == 8 ? 2 :
                     endpoint->burst == 4 ? 1 : 0;
    return endpoint->request | (burst << 6) |
           ((endpoint->request != 0) << 8) | (width << 9);
}

static void put32(uint8_t *destination, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        destination[i] = (uint8_t)(value >> (i * 8));
}

enum a733_dma_result a733_dma_build(
    struct a733_dma_descriptor *output, size_t capacity, uint64_t descriptor_bus,
    const struct a733_dma_segment *segments, size_t count)
{
    if (!output || !segments || !count || count > A733_DMA_MAX_SEGMENTS ||
        capacity < count)
        return A733_DMA_ARGUMENT;
    size_t output_bytes = count * sizeof(*output);
    size_t input_bytes = count * sizeof(*segments);
    uintptr_t output_address = (uintptr_t)output;
    uintptr_t input_address = (uintptr_t)segments;
    if (output_address % 64 || descriptor_bus % 64 ||
        input_address % _Alignof(struct a733_dma_segment))
        return A733_DMA_ALIGNMENT;
    if (output_address > UINTPTR_MAX - output_bytes ||
        input_address > UINTPTR_MAX - input_bytes ||
        descriptor_bus >= DESCRIPTOR_LIMIT ||
        output_bytes > DESCRIPTOR_LIMIT - descriptor_bus)
        return A733_DMA_ADDRESS;
    if (overlap(output_address, output_bytes, input_address, input_bytes))
        return A733_DMA_ALIAS;

    /* Validate every segment before any descriptor mutation. */
    for (size_t i = 0; i < count; ++i) {
        const struct a733_dma_segment *segment = &segments[i];
        if (!segment->bytes || segment->bytes > A733_DMA_MAX_SEGMENT_BYTES ||
            (segment->source.request && segment->destination.request))
            return A733_DMA_FORMAT;
        const struct a733_dma_endpoint *ends[] = {
            &segment->source, &segment->destination
        };
        for (unsigned e = 0; e < 2; ++e) {
            enum a733_dma_result status = validate_endpoint(ends[e], segment->bytes);
            if (status != A733_DMA_OK) return status;
            if (overlap(ends[e]->address, endpoint_bytes(ends[e], segment->bytes),
                        descriptor_bus, output_bytes))
                return A733_DMA_ALIAS;
        }
    }
    /* Prevent dependent copies and overlapping writes across the entire chain.
     * Request-zero memory and fixed port ranges must also be disjoint. */
    for (size_t i = 0; i < count; ++i) {
        const struct a733_dma_endpoint *ends[] = {
            &segments[i].source, &segments[i].destination
        };
        for (unsigned e = 0; e < 2; ++e) {
            if (ends[e]->request) continue;
            for (size_t j = 0; j < count; ++j) {
                const struct a733_dma_endpoint *others[] = {
                    &segments[j].source, &segments[j].destination
                };
                for (unsigned o = 0; o < 2; ++o) {
                    if (i == j && e == o) continue;
                    if (!e && !o && !others[o]->request) continue;
                    if (overlap(ends[e]->address, segments[i].bytes,
                                others[o]->address,
                                endpoint_bytes(others[o], segments[j].bytes)))
                        return A733_DMA_ALIAS;
                }
            }
        }
    }

    for (size_t i = 0; i < count; ++i) {
        const struct a733_dma_segment *segment = &segments[i];
        uint8_t *descriptor = output[i].bytes;
        for (size_t j = 0; j < sizeof(*output); ++j) descriptor[j] = 0;
        put32(descriptor, endpoint_configuration(&segment->source) |
              (endpoint_configuration(&segment->destination) << 16));
        put32(descriptor + 4, (uint32_t)segment->source.address);
        put32(descriptor + 8, (uint32_t)segment->destination.address);
        put32(descriptor + 12, segment->bytes);
        put32(descriptor + 16, 8u |
              (uint32_t)((segment->source.address >> 32) << 11) |
              (uint32_t)((segment->destination.address >> 32) << 15));
        put32(descriptor + 20, i + 1 == count ? (uint32_t)DESCRIPTOR_LIMIT :
              (uint32_t)(descriptor_bus + (i + 1) * sizeof(*output)));
    }
    return A733_DMA_OK;
}
