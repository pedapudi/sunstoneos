/* The regions the OS runtime gives a compiled NPU graph, from the OS's own decoder and layout (drivers/npu/nbg_model.c,
 * nbg_assembly.c): for each graph file named on the command line one line "read WRITE write WRITE constant CONSTANT", in bytes,
 * where read is the input allocations, write the memory pool and the output allocations, and constant the descriptors, the
 * coefficients and a dynamic input, as need() in drivers/npu/a733_npu_runtime.c sums them. Built and run on the host by
 * tools/npu_graph_layout.py. */
#include "nbg_model.h"
#include "nbg_assembly.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    for (int a = 1; a < argc; ++a) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", argv[a]); return 2; }
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        void *data = n > 0 ? aligned_alloc(64, ((size_t)n + 63) & ~(size_t)63) : NULL;
        if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "cannot read %s\n", argv[a]); return 2; }
        fclose(f);
        struct nbg_model view;
        if (nbg_model_open(data, (size_t)n, &view) != NBG_DECODE_OK) { fprintf(stderr, "%s: the decoder refuses it\n", argv[a]); return 3; }
        const size_t entries = nbg_assembly_layout_entries(&view);
        struct nbg_assembly_layout l = {.buffers = calloc(entries, sizeof(struct nbg_allocation_requirement)), .capacity = (uint32_t)entries};
        if (!entries || !l.buffers || !nbg_assembly_layout(&view, &l)) { fprintf(stderr, "%s: the layout refuses it\n", argv[a]); return 3; }
        size_t constant = l.buffers[NBG_ALLOC_NN].allocation_bytes + l.buffers[NBG_ALLOC_COEFF].allocation_bytes;
        if (l.dynamic) constant += l.buffers[nbg_layout_dynamic_index(&l)].allocation_bytes;
        size_t read = 0, write = l.buffers[NBG_ALLOC_POOL].allocation_bytes;
        for (unsigned i = 0; i < l.inputs; ++i) read += l.buffers[NBG_ALLOC_INPUT + i].allocation_bytes;
        for (unsigned i = 0; i < l.outputs; ++i) write += l.buffers[NBG_ALLOC_INPUT + l.inputs + i].allocation_bytes;
        printf("read %zu write %zu constant %zu\n", read, write, constant);
        free(l.buffers);
        free(data);
    }
    return 0;
}
