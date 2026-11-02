#ifndef NPU_GRAPHS_CHAIN_H
#define NPU_GRAPHS_CHAIN_H
#include <stdint.h>
/* The chain study (NPU_GRAPHS_CHAIN): graph g of the model list takes the output of graph g - 1 as its first input, rescaled
 * from the fixed-point position of that output to the one of this input, and the last graph's output is decoded. The data
 * come from an input region of the application, which a packing tool fills; this file is the format of that region. All
 * numbers are little-endian; offsets are bytes from the start of the region. */
#define NPU_CHAIN_MAGIC UINT64_C(0x4e49414843555053)  /* "SPUCHAIN" */
#define NPU_CHAIN_VERSION 1u
#define NPU_CHAIN_MAX_GRAPHS 24u
struct npu_chain_graph {
    int32_t fl_in, fl_out;          /* fractional bits of the first input and of the first output (dynamic fixed point, 16 bits) */
    uint32_t in_bytes, out_bytes;   /* of the first input and the first output tensor */
    uint32_t extra_bytes;           /* of the second input (a constant), 0 for none */
    int32_t reference_fl;           /* fractional bits of the reference, int32 codes (no floating point in the application) */
    uint64_t extra_offset;          /* the constant's bytes */
    uint64_t reference_offset;      /* int32 reference of the first output, in the block of a reference record */
};
struct npu_chain_header {
    uint64_t magic;
    uint32_t version, graphs, records, reference_records;
    uint32_t rows, columns;         /* of the last graph's output: one row for each frame, one column for each token */
    uint32_t reserved;
    uint64_t input_offset, input_stride;         /* record r's first-graph input: input_offset + r * input_stride */
    uint64_t reference_offset, reference_stride; /* reference record r's block: reference_offset + r * reference_stride */
    struct npu_chain_graph graph[NPU_CHAIN_MAX_GRAPHS];
};
/* What the application returns (ss_finish). Times are counter ticks (24 MHz). */
#define NPU_CHAIN_RESULT_MAGIC UINT64_C(0x5453455248435053)
/* Of one graph's output against its reference, in the reference's fixed point (reference_fl): the sums of products shifted
 * right by 16 (the shift cancels in the cosine) and the largest absolute difference in units of 2^-reference_fl. */
struct npu_chain_stat { int64_t dot, aa, bb, max_abs, fl; };
#define NPU_CHAIN_MAX_RECORDS 64u
#define NPU_CHAIN_MAX_STAT_RECORDS 16u
#define NPU_CHAIN_MAX_ROWS 128u
struct npu_chain_result {
    uint64_t magic, version, graphs, records, reference_records, rows;
    uint64_t start, stop;
    uint64_t clock_before[3], clock_after[3];     /* the OS clock record before and after (as the queue study packs it) */
    uint64_t record_ticks[NPU_CHAIN_MAX_RECORDS];     /* from the first input written to the last token decoded, statistics excluded */
    uint64_t wait_ticks[NPU_CHAIN_MAX_RECORDS];       /* inside it: submit to the output read, all graphs (job, cache maintenance) */
    uint64_t rescale_ticks[NPU_CHAIN_MAX_RECORDS];    /* inside it: the rescales between graphs */
    uint64_t decode_ticks[NPU_CHAIN_MAX_RECORDS];     /* inside it: the argmax over the last output */
    struct npu_chain_stat stat[NPU_CHAIN_MAX_STAT_RECORDS][NPU_CHAIN_MAX_GRAPHS];
    uint16_t token[NPU_CHAIN_MAX_RECORDS][NPU_CHAIN_MAX_ROWS];   /* the greedy token of every frame */
};
#endif
