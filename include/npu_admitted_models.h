#ifndef SUNSTONE_NPU_ADMITTED_MODELS_H
#define SUNSTONE_NPU_ADMITTED_MODELS_H
#include <stdint.h>
#include "nbg_assembly.h"

/* The compiled models an image may execute on the NPU. Each image links one
 * table, which tools/npu-session-catalogue.py generates into the image build
 * directory from the model lists of the applications the image contains;
 * every entry comes from the reviewed catalogue tools/npu-session-models.json.
 * The runtime and session drivers admit a model only when its SHA-256 and
 * size match an entry and the decoder and assembler derive the entry's
 * counts and tensor sizes from it. The digest fixes descriptors,
 * coefficients and relocations together; the counts are an independent
 * check of the plan. A caller cannot add an entry at run time. Hash identity
 * does not establish device confinement. */
struct npu_admitted_model {
    unsigned char digest[32];
    uint32_t model_bytes, inputs, outputs;
    uint32_t tensor_bytes[2 * 8u /* the session driver's own table: NPU_SESSION_TENSORS each way (npu_session.h) */];
    uint32_t nn_operations, descriptor_patches, state_patches, command_body_bytes;
};
extern const struct npu_admitted_model npu_admitted_models[];
extern const unsigned npu_admitted_model_count;
#endif
