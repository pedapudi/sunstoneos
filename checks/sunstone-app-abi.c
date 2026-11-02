/* Host model of the SunstoneOS application calls for hello-compute
 * (checks/sunstone-app-abi.py). It grants the one input region from a file,
 * runs hello_compute_main, and writes the OS result record that ss_finish
 * produces. Device calls refuse; liveness is always 1. */
#include "hello_compute-app.h"
#include "sunstone_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct ss_grant grant;
static unsigned long progress;
static const char *result_path;

const struct ss_grant *ss_grant(void) { return &grant; }
int ss_live(void) { return 1; }
void ss_progress(void) { ++progress; }
int ss_throttled(void) { return 0; }
int ss_clock_ready(void) { return 1; }
int ss_capture(uint32_t sequence, void **pcm, size_t *bytes)
{ (void)sequence; (void)pcm; (void)bytes; return -1; }
int ss_host_recv(struct ss_host_request *request, uint32_t wait_us) { (void)request; (void)wait_us; return 0; }
int ss_host_send(const struct ss_host_reply *reply) { (void)reply; return -1; }
int ss_storage_read(uint64_t sector, uint8_t data[512]) { (void)sector; (void)data; return -1; }
int ss_key_report(struct ss_key_report *report) { (void)report; return -1; }

_Noreturn void ss_fail(uint32_t detail)
{
    fprintf(stderr, "ss_fail %u\n", (unsigned)detail);
    exit(3);
}
_Noreturn void ss_finish(const void *result, size_t bytes)
{
    const struct ss_manifest *m = &hello_compute_manifest;
    if (bytes > m->output_limit) ss_fail(0x100);
    static uint8_t record[sizeof(struct ss_result) + 65536];
    struct ss_result header = {SS_RESULT_MAGIC, SS_RESULT_VERSION, sizeof(struct ss_result) + m->output_limit,
                               0, 0, SS_RESULT_FINISHED, bytes};
    memcpy(record, &header, sizeof(header));
    memcpy(record + sizeof(header), result, bytes);
    FILE *f = fopen(result_path, "wb");
    if (!f || fwrite(record, 1, sizeof(header) + m->output_limit, f) != sizeof(header) + m->output_limit ||
        fclose(f))
        exit(4);
    fprintf(stderr, "progress %lu\n", progress);
    exit(0);
}

int main(int argc, char **argv)
{
    const struct ss_manifest *m = &hello_compute_manifest;
    if (argc != 3 || m->magic != SS_MANIFEST_MAGIC || m->region_count != 1 ||
        m->regions[0].kind != SS_REGION_INPUT) return 2;
    result_path = argv[2];
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    /* One byte more than the region holds detects an oversized input. */
    size_t capacity = (size_t)m->regions[0].bytes;
    uint8_t *data = malloc(capacity + 1);
    size_t n = data ? fread(data, 1, capacity + 1, f) : 0;
    fclose(f);
    if (!data) return 2;
    if (n > capacity) return 5;
    static struct ss_span spans[1];
    spans[0] = (struct ss_span){data, n};
    grant.region_count = 1;
    grant.regions = spans;
    hello_compute_main(&grant);
    return 6;
}
