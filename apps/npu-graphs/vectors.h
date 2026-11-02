#ifndef NPU_GRAPHS_VECTORS_H
#define NPU_GRAPHS_VECTORS_H
#include <stddef.h>
#include <stdint.h>
/* One test graph: its catalogue name, its input tensors concatenated and the simulator's output tensors concatenated. */
struct npu_graph_vector {
    const char *name;
    const uint8_t *input;
    size_t input_bytes;
    const uint8_t *reference;
    size_t reference_bytes;
};
extern const struct npu_graph_vector npu_graph_vectors[];
extern const unsigned npu_graph_count;
/* A graph handed to the OS at run time: its compiled bytes, which no list of the OS image names, and its vectors. */
struct npu_graph_runtime {
    struct npu_graph_vector vector;
    const uint8_t *model;
    size_t model_bytes;
};
extern const struct npu_graph_runtime npu_graph_runtime[];
extern const unsigned npu_graph_runtime_count;
#endif
