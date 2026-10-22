/*
 * Run one NPU network binary graph (NBG) in the SDK simulator for a sequence
 * of input vectors on one prepared graph.
 *
 * Built and run by tools/npu-conformance.py inside the pinned compiler image.
 * The harness generates npu-conformance-bindings.h from the model's catalogue
 * tensor descriptors and compiles it into this file. The ovxlib calls follow
 * the program that the compiler generated for the 8x8 convolution.
 *
 * Usage: runner MODEL.nb JOB_DIRECTORY JOB_COUNT
 *
 * For each job j, the runner reads JOB_DIRECTORY/j/input<i>.bin, fills every
 * output tensor with a poison byte, copies the inputs, runs the graph, and
 * writes JOB_DIRECTORY/j/output<o>.bin and JOB_DIRECTORY/j/readback<i>.bin,
 * the input tensor's bytes read back after the run. The read-back shows that
 * the input tensor still held the vector after the run; it does not show which
 * bytes the simulated engine read. JOB_DIRECTORY/times receives the time of
 * graph setup and verification and each job's run time in nanoseconds; the
 * first run also completes the graph's preparation. Any failure stops the
 * runner with a nonzero exit status.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vsi_nn_pub.h"

struct npu_conformance_tensor {
    uint32_t rank;
    vsi_size_t size[4];
    vsi_nn_type_e type;
    float scale;
    int32_t zero_point;
    size_t bytes;
};

#include "npu-conformance-bindings.h"

#define TENSORS (NPU_CONFORMANCE_INPUTS + NPU_CONFORMANCE_OUTPUTS)
#define POISON 0x5a

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int fail(const char *what, const char *detail)
{
    fprintf(stderr, "npu-conformance-runner: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
    return 1;
}

static int read_exact(const char *path, uint8_t *data, size_t bytes)
{
    FILE *f = fopen(path, "rb");
    int extra;
    size_t got;
    if (!f)
        return fail("cannot open", path);
    got = fread(data, 1, bytes, f);
    extra = fgetc(f);
    fclose(f);
    if (got != bytes || extra != EOF)
        return fail("input file has the wrong size", path);
    return 0;
}

static int write_exact(const char *path, const uint8_t *data, size_t bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return fail("cannot create", path);
    if (fwrite(data, 1, bytes, f) != bytes || fclose(f) != 0)
        return fail("cannot write", path);
    return 0;
}

static vsi_nn_tensor_t *bound(vsi_nn_graph_t *graph, uint32_t index)
{
    return vsi_nn_GetTensor(graph, index < NPU_CONFORMANCE_INPUTS
                                       ? graph->input.tensors[index]
                                       : graph->output.tensors[index - NPU_CONFORMANCE_INPUTS]);
}

static int run_job(vsi_nn_graph_t *graph, const char *root, unsigned long job, uint8_t *buffer,
                   uint64_t *elapsed)
{
    char path[4096];
    uint32_t i;
    uint64_t start;

    for (i = NPU_CONFORMANCE_INPUTS; i < TENSORS; i++) {
        memset(buffer, POISON, npu_conformance_tensors[i].bytes);
        if (vsi_nn_CopyDataToTensor(graph, bound(graph, i), buffer) != VSI_SUCCESS)
            return fail("cannot poison an output tensor", NULL);
    }
    for (i = 0; i < NPU_CONFORMANCE_INPUTS; i++) {
        snprintf(path, sizeof(path), "%s/%lu/input%u.bin", root, job, i);
        if (read_exact(path, buffer, npu_conformance_tensors[i].bytes))
            return 1;
        if (vsi_nn_CopyDataToTensor(graph, bound(graph, i), buffer) != VSI_SUCCESS)
            return fail("cannot copy input", path);
    }
    start = now_ns();
    if (vsi_nn_RunGraph(graph) != VSI_SUCCESS)
        return fail("graph run failed", NULL);
    *elapsed = now_ns() - start;
    for (i = 0; i < TENSORS; i++) {
        uint8_t *data = vsi_nn_ConvertTensorToData(graph, bound(graph, i));
        int status;
        if (!data)
            return fail("cannot read a tensor", NULL);
        if (i < NPU_CONFORMANCE_INPUTS)
            snprintf(path, sizeof(path), "%s/%lu/readback%u.bin", root, job, i);
        else
            snprintf(path, sizeof(path), "%s/%lu/output%u.bin", root, job, i - NPU_CONFORMANCE_INPUTS);
        status = write_exact(path, data, npu_conformance_tensors[i].bytes);
        vsi_nn_Free(data);
        if (status)
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    vsi_nn_context_t context = NULL;
    vsi_nn_graph_t *graph = NULL;
    vsi_nn_node_t *node;
    vsi_nn_tensor_id_t ids[TENSORS];
    uint8_t *buffer = NULL;
    size_t largest = 0;
    unsigned long jobs, job;
    char *end, path[4096];
    FILE *times = NULL;
    uint64_t start, elapsed;
    uint32_t i;
    int status = 1;

    if (argc != 4)
        return fail("usage: runner MODEL.nb JOB_DIRECTORY JOB_COUNT", NULL);
    errno = 0;
    jobs = strtoul(argv[3], &end, 10);
    if (errno || *end || jobs == 0)
        return fail("invalid job count", argv[3]);
    snprintf(path, sizeof(path), "%s/times", argv[2]);
    times = fopen(path, "w");
    if (!times)
        return fail("cannot create", path);

    start = now_ns();
    context = vsi_nn_CreateContext();
    graph = context ? vsi_nn_CreateGraph(context, TENSORS, 1) : NULL;
    if (!graph) {
        fail("cannot create the graph", NULL);
        goto done;
    }
    vsi_nn_SetGraphInputs(graph, NULL, NPU_CONFORMANCE_INPUTS);
    vsi_nn_SetGraphOutputs(graph, NULL, NPU_CONFORMANCE_OUTPUTS);
    node = vsi_nn_AddNode(graph, VSI_NN_OP_NBG, NPU_CONFORMANCE_INPUTS, NPU_CONFORMANCE_OUTPUTS, NULL);
    if (!node) {
        fail("cannot add the NBG node", NULL);
        goto done;
    }
    node->nn_param.nbg.type = VSI_NN_NBG_FILE;
    node->nn_param.nbg.url = argv[1];
    for (i = 0; i < TENSORS; i++) {
        const struct npu_conformance_tensor *t = &npu_conformance_tensors[i];
        vsi_nn_tensor_attr_t attr;
        memset(&attr, 0, sizeof(attr));
        memcpy(attr.size, t->size, sizeof(t->size));
        attr.dim_num = t->rank;
        attr.dtype.vx_type = t->type;
        attr.dtype.qnt_type = VSI_NN_QNT_TYPE_AFFINE_ASYMMETRIC;
        attr.dtype.scale = t->scale;
        attr.dtype.zero_point = t->zero_point;
        ids[i] = vsi_nn_AddTensor(graph, VSI_NN_TENSOR_ID_AUTO, &attr, NULL);
        if (ids[i] == VSI_NN_TENSOR_ID_NA) {
            fail("cannot add a tensor", NULL);
            goto done;
        }
        if (vsi_nn_GetElementNum(vsi_nn_GetTensor(graph, ids[i])) * vsi_nn_TypeGetBytes(t->type) != t->bytes) {
            fail("tensor size differs from the catalogue", NULL);
            goto done;
        }
        if (t->bytes > largest)
            largest = t->bytes;
        if (i < NPU_CONFORMANCE_INPUTS) {
            node->input.tensors[i] = ids[i];
            graph->input.tensors[i] = ids[i];
        } else {
            node->output.tensors[i - NPU_CONFORMANCE_INPUTS] = ids[i];
            graph->output.tensors[i - NPU_CONFORMANCE_INPUTS] = ids[i];
        }
    }
    if (vsi_nn_SetupGraph(graph, FALSE) != VSI_SUCCESS || vsi_nn_VerifyGraph(graph) != VSI_SUCCESS) {
        fail("graph preparation failed", NULL);
        goto done;
    }
    fprintf(times, "prepare %llu\n", (unsigned long long)(now_ns() - start));
    buffer = malloc(largest);
    if (!buffer) {
        fail("out of memory", NULL);
        goto done;
    }
    for (job = 0; job < jobs; job++) {
        if (run_job(graph, argv[2], job, buffer, &elapsed))
            goto done;
        fprintf(times, "job %lu %llu\n", job, (unsigned long long)elapsed);
    }
    status = 0;
done:
    free(buffer);
    if (graph)
        vsi_nn_ReleaseGraph(&graph);
    if (context)
        vsi_nn_ReleaseContext(&context);
    if (fclose(times) != 0)
        status = fail("cannot write job times", NULL);
    return status;
}
