#ifndef NPU_GRAPHS_RECEIPT_H
#define NPU_GRAPHS_RECEIPT_H
#include <stdint.h>
#define NPU_GRAPHS_MAGIC UINT64_C(0x5048415247554e50) /* "PNUGRAPH" little-endian */
#define NPU_GRAPHS_BYTES 1728u
#define NPU_GRAPHS_MAX 16u
/* Status of one graph. */
enum { NPU_GRAPH_MATCH, NPU_GRAPH_MISMATCH, NPU_GRAPH_REFUSED, NPU_GRAPH_FAILED, NPU_GRAPH_TIMEOUT, NPU_GRAPH_NOT_RUN,
       NPU_GRAPH_FAULT /* the job failed with an NPU MMU exception and the NPU was recovered */ };
struct npu_graph_record {
    uint64_t index, input_bytes, output_bytes, status;
    uint64_t first_difference; /* byte offset of the first differing output byte (mismatch only); the faulting address (fault) */
    uint64_t observed, expected; /* those two bytes; the MMU status and the canary CRC-32 (fault) */
    uint64_t ticks;            /* counter ticks from the first ss_npu_run call to its completion */
};
struct npu_graph_receipt {
    uint64_t magic, version, bytes, count, completed, matched, sdk_closed, start, stop, reserved[7];
    struct npu_graph_record graph[NPU_GRAPHS_MAX];
    /* Over the whole output: the number of differing bytes and the largest difference of 16-bit signed elements.
     * For a fault: the service's progress marks and the reply wait window in milliseconds. */
    uint32_t differing_bytes[NPU_GRAPHS_MAX], max_error[NPU_GRAPHS_MAX];
    uint8_t padding[NPU_GRAPHS_BYTES - 16 * 8 - NPU_GRAPHS_MAX * 64 - 2 * 4 * NPU_GRAPHS_MAX];
};
_Static_assert(sizeof(struct npu_graph_record) == 64, "graph record");
_Static_assert(sizeof(struct npu_graph_receipt) == NPU_GRAPHS_BYTES, "graph receipt");
#endif
