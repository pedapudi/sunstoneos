#ifndef SUNSTONE_NPU_FINITE_NETWORKS_H
#define SUNSTONE_NPU_FINITE_NETWORKS_H
#include <stddef.h>
/* The networks of the NPU runtime finite image, in job order: each model with
 * its input tensors and reference outputs concatenated in model order. The
 * build (apps/npu-finite/build.py --runtime) generates the table from the
 * runtime model list and the catalogue artifacts; the same list generates the
 * image's admitted table. */
struct npu_finite_network {
    const unsigned char *model,*input,*reference;
    size_t model_bytes,input_bytes,reference_bytes;
};
#ifdef SUNSTONE_NPU_MODEL_SWITCH
#define NPU_FINITE_NETWORKS 3u
#else
#define NPU_FINITE_NETWORKS 2u
#endif
#define NPU_FINITE_OUTPUT_BYTES 4096u
extern const struct npu_finite_network npu_finite_networks[NPU_FINITE_NETWORKS];
#endif
