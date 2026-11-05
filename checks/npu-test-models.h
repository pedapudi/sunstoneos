#ifndef SUNSTONE_CHECKS_NPU_TEST_MODELS_H
#define SUNSTONE_CHECKS_NPU_TEST_MODELS_H
/* The models a host check links (tools/npu-test-models.py generates the tables). The OS image links none: this is test data. */
#include "npu.h"
extern const struct npu_runtime_model os_npu_models[];
extern const unsigned os_npu_model_count,os_npu_client_entry;
extern const struct ss_npu_model os_npu_model_info[];
struct os_npu_vector { const uint8_t *data; size_t bytes; };
extern const struct os_npu_vector os_npu_inputs[],os_npu_references[];
extern const unsigned os_npu_input_count,os_npu_output_count;
#endif
