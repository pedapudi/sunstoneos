/* Per-job CPU work of one compiled model on the NPU session runtime.
 *
 * For each network binary graph given, the check decodes it, asks the
 * native assembler for its layout, assembles it into private buffers at the
 * device addresses the session placement would use, and derives what one
 * job costs the CPU on the runtime as it is written today, for a session
 * group that holds only this model:
 *
 *   npu_session_send (SUBMIT): clears the working pool and copies each
 *     input span into its READ buffer (copy_in_bytes, pool_clear_bytes).
 *   publish -> model_intact(s, m, 0): SHA-256 over the whole NN and
 *     coefficient allocations and over the command-stream payload
 *     (digest_bytes); every later 64-bit word of the command allocation must
 *     be an END pair (scan_words).
 *   a733_npu_memory_publish: live_mappings translates every page of the MMU
 *     arena and of every buffer, twice for writable ones (translations);
 *     canonical() reads every root word, every word of every leaf table and
 *     the 3,072 bytes after the root table (table_words, counting those
 *     bytes as 768 words); whole READ buffers are cleaned and whole WRITE
 *     buffers cleaned and invalidated (clean_bytes, clean_invalidate_bytes).
 *   a733_npu_memory_finish: live_mappings again and invalidation of every
 *     WRITE buffer (translations, invalidate_bytes).
 *   npu_session_output: one copy per output tensor (copy_out_bytes).
 *   SUBMIT, WAIT and REAP are three queue round trips.
 *
 * CPU DDR traffic counts each copy's source read and destination write, the
 * digested, scanned and table bytes as reads, and the pool clear as writes.
 * Multiply-accumulates assume each input tensor i, of K_i x R_i elements,
 * feeds one fully connected layer whose output i has N_i x R_i elements.
 * These are derived counts; checks/npu-session.c does not yet measure them.
 *
 * Build on the host:
 *   clang-21 -std=c11 -O1 -Iinclude checks/npu-mimus-cost.c drivers/npu/nbg_model.c \
 *     drivers/npu/nbg_assembly.c drivers/npu/nbg_relocation.c drivers/npu/nbg_checksum.c -o cost
 *   cost MODEL.nb...
 */
#include "nbg_assembly.h"
#include "a733_npu_controller.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* From include/npu_session.h, which needs the FreeRTOS headers. */
#define DEVICE_BASE 0x10000000u
#define BOOTSTRAP_BYTES 524288u
#define PAGE 4096u
#define LEAF_TABLE 16384u
#define LINE 64u

static unsigned char *read_file(const char *path, size_t *bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *data = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > 0 && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)size)) &&
            fread(data, 1, (size_t)size, f) != (size_t)size) { free(data); data = NULL; }
        *bytes = (size_t)size;
    }
    fclose(f);
    return data;
}

static uint64_t pages(uint64_t bytes) { return (bytes + PAGE - 1) / PAGE; }

static int cost(const char *path)
{
    size_t bytes = 0;
    unsigned char *data = read_file(path, &bytes);
    struct nbg_model model;
    struct nbg_assembly_layout layout, assembled;
    if (!data || nbg_model_open(data, bytes, &model) != NBG_DECODE_OK ||
        !nbg_assembly_layout(&model, &layout)) {
        fprintf(stderr, "FAIL %s: not an admissible graph\n", path);
        free(data);
        return 0;
    }
    unsigned count = NBG_ALLOC_INPUT + layout.inputs + layout.outputs;
    struct nbg_assembly_buffer buffers[NBG_LAYOUT_MAX_BUFFERS] = {{0}};
    /* The session placement: the bootstrap stream first, then each nonempty
     * allocation followed by one unmapped page. */
    uint64_t device = DEVICE_BASE + BOOTSTRAP_BYTES + PAGE;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t size = layout.buffers[i].allocation_bytes;
        if (!size) continue;
        buffers[i] = (struct nbg_assembly_buffer){aligned_alloc(PAGE, size), size, (uint32_t)device};
        if (!buffers[i].cpu) {
            fprintf(stderr, "FAIL %s: cannot allocate %u bytes\n", path, size);
            for (unsigned j = 0; j < i; ++j) free(buffers[j].cpu);
            free(data);
            return 0;
        }
        device += size + PAGE;
    }
    int ok = nbg_assemble_layout(&model, buffers, count, &assembled) &&
             !memcmp(&assembled, &layout, sizeof(layout));
    uint64_t roots = ((device - PAGE - 1) >> 24) - (DEVICE_BASE >> 24) + 1;
    uint64_t arena = PAGE + roots * LEAF_TABLE;

    uint64_t copy_in = 0, copy_out = 0, clean = 0, clean_invalidate = 0, macs = 0;
    uint64_t mapped_pages = pages(BOOTSTRAP_BYTES) + 2 * pages(arena);
    for (unsigned i = 0; i < count; ++i) {
        uint64_t size = layout.buffers[i].allocation_bytes;
        int input = i >= NBG_ALLOC_INPUT && i < NBG_ALLOC_INPUT + layout.inputs;
        int write = i == NBG_ALLOC_POOL || (i >= NBG_ALLOC_INPUT && !input);
        mapped_pages += pages(size) * (write ? 2 : 1);
        if (input) { copy_in += layout.buffers[i].payload_bytes; clean += size; }
        if (i >= NBG_ALLOC_INPUT && !input) copy_out += layout.buffers[i].payload_bytes;
        if (write) clean_invalidate += size;
    }
    for (unsigned i = 0; i < layout.inputs && i < layout.outputs; ++i) {
        struct nbg_tensor in, out;
        if (nbg_model_tensor(&model, NBG_INPUT, i, &in) && nbg_model_tensor(&model, NBG_OUTPUT, i, &out))
            macs += (uint64_t)in.sizes[0] * out.sizes[0] * out.sizes[1];
    }
    uint64_t command = layout.buffers[NBG_ALLOC_COMMAND].allocation_bytes;
    uint64_t stream = layout.buffers[NBG_ALLOC_COMMAND].payload_bytes;
    uint64_t pool = layout.buffers[NBG_ALLOC_POOL].allocation_bytes;
    uint64_t digest = layout.buffers[NBG_ALLOC_NN].allocation_bytes +
                      layout.buffers[NBG_ALLOC_COEFF].allocation_bytes + stream;
    uint64_t scan = (command - stream) / 8, table_words = 256 + roots * (LEAF_TABLE / 4) + 3072 / 4;
    uint64_t translations = 2 * mapped_pages;
    uint64_t ddr_read = copy_in + digest + scan * 8 + table_words * 4 + copy_out;
    uint64_t ddr_write = copy_in + pool + copy_out;
    printf("{\"model\": \"%s\", \"assembled\": %s, \"inputs\": %u, \"outputs\": %u, "
           "\"nn_operations\": %u, \"macs\": %llu, \"copy_in_bytes\": %llu, \"pool_clear_bytes\": %llu, "
           "\"copy_out_bytes\": %llu, \"clean_bytes\": %llu, \"clean_invalidate_bytes\": %llu, "
           "\"invalidate_bytes\": %llu, \"maintained_lines\": %llu, \"digest_bytes\": %llu, "
           "\"scan_words\": %llu, \"translations\": %llu, \"table_words\": %llu, \"round_trips\": 3, "
           "\"ddr_read_bytes\": %llu, \"ddr_write_bytes\": %llu, \"coefficient_bytes\": %u, "
           "\"nn_bytes\": %u, \"command_bytes\": %llu, \"stream_bytes\": %llu, \"arena_bytes\": %llu}\n",
           path, ok ? "true" : "false", layout.inputs, layout.outputs, layout.nn_operations,
           (unsigned long long)macs, (unsigned long long)copy_in, (unsigned long long)pool,
           (unsigned long long)copy_out, (unsigned long long)clean, (unsigned long long)clean_invalidate,
           (unsigned long long)clean_invalidate,
           (unsigned long long)((clean + 2 * clean_invalidate) / LINE), (unsigned long long)digest,
           (unsigned long long)scan, (unsigned long long)translations, (unsigned long long)table_words,
           (unsigned long long)ddr_read, (unsigned long long)ddr_write,
           layout.buffers[NBG_ALLOC_COEFF].allocation_bytes, layout.buffers[NBG_ALLOC_NN].allocation_bytes,
           (unsigned long long)command, (unsigned long long)stream, (unsigned long long)arena);
    for (unsigned i = 0; i < count; ++i) free(buffers[i].cpu);
    free(data);
    if (!ok) fprintf(stderr, "FAIL %s: assembly differs from the planned layout\n", path);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s MODEL.nb...\n", argv[0]);
        return 2;
    }
    int ok = 1;
    for (int i = 1; i < argc; ++i) ok &= cost(argv[i]);
    return ok ? 0 : 1;
}
