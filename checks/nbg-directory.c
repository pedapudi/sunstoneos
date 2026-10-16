#include "nbg_model.h"
#include "nbg_assembly.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char original[1928], image[1993];
static unsigned refused;

static void put(unsigned char *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i));
}

static void refuse(const void *data, size_t bytes)
{
    struct nbg_model m, saved;
    memset(&m, 0xa5, sizeof(m)); memcpy(&saved, &m, sizeof(m));
    assert(nbg_model_open(data, bytes, &m) != NBG_DECODE_OK);
    assert(!memcmp(&m, &saved, sizeof(m)));
    ++refused;
}

static void bad(size_t offset, uint32_t value)
{
    memcpy(image + 1, original, sizeof(original)); put(image + 1 + offset, value);
    refuse(image + 1, sizeof(original));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    assert(fread(original, 1, sizeof(original), f) == sizeof(original));
    assert(fgetc(f) == EOF); fclose(f);
    memcpy(image + 1, original, sizeof(original));
    image[0] = image[1929] = 0x5a;
    struct nbg_model m;
    assert(nbg_model_open(image + 1, sizeof(original), &m) == NBG_DECODE_OK);
    assert(m.header.version == 0x20000 && m.header.target == 0x1000003b);
    assert(m.header.operation_count == 2 && m.header.layer_count == 1);
    assert(m.header.pool_bytes == 0 && m.header.pool_alignment == 64);
    assert(m.header.vip_sram_bytes == 0x18b00 && m.header.flags == 10);
    assert(m.header.nn_bytes == 192 && !strcmp(m.header.name, "conv3x3_NCHW"));
    struct nbg_span span;
    assert(nbg_model_section(&m, NBG_NETWORK_INFO, &span) && span.bytes == 220);
    assert(span.data[80] == 5); /* Retain the ambiguous metadata without using it as a bound. */
    struct nbg_tensor t;
    assert(nbg_model_tensor(&m, NBG_INPUT, 0, &t));
    assert(t.logical_bytes == 192 && t.scale_bits == 0x3c808081 && t.zero_point == 127);
    assert(nbg_model_tensor(&m, NBG_OUTPUT, 0, &t));
    assert(t.logical_bytes == 72 && t.scale_bits == 0x3ca62626 && t.zero_point == 108);
    struct nbg_patch p, saved;
    assert(nbg_model_patch(&m, 4, &p) && p.source == 5 && p.transformed == 1);
    assert(!p.high_destination && !p.high_mask && !p.destination_base);
    memcpy(&saved, &p, sizeof(p));
    assert(!nbg_model_patch(&m, 5, &p) && !memcmp(&p, &saved, sizeof(p)));
    assert(nbg_model_table(&m, NBG_LOAD_STATES_TABLE, 1, &span) && span.bytes == 32);
    assert(!nbg_model_tensor(&m, NBG_INPUT, 0, (void *)&m));
    assert(!nbg_model_section(&m, NBG_NETWORK_INFO, (void *)(image + 1)));
    assert(!memcmp(image + 1, original, sizeof(original)));
    struct nbg_assembly_plan plan, saved_plan;
    memset(&plan, 0xa5, sizeof(plan)); memcpy(&saved_plan, &plan, sizeof(plan));
    assert(!nbg_assembly_requirements(&m, &plan));
    assert(!memcmp(&plan, &saved_plan, sizeof(plan))); /* Decoding cannot admit execution. */

    const struct { size_t at; uint32_t value; } changes[] = {
        {16,1927},{20,1},{24,0},{24,UINT32_MAX},{28,0},{28,24},{32,0},{32,2},
        {36,1},{40,1},{44,1},{48,0},{48,48},{48,UINT32_MAX},{52,0},{52,40},
        {56,0},{56,41},{56,UINT32_MAX},{60,0},{60,41},{60,1},
        {64,0},{64,48},{64,60},{64,1910},{64,UINT32_MAX},{68,1},{72,0},{72,221},
        {76,1},{80,1},{84,1},{88,3},{92,0},{92,1},{96,1},{100,1},
        {632+64,0},{632+68,2},{632+72,2},{632+88,40},{632+92,4},{632+92,16},
        {60+11*44+28,128},{60+8*44+32,40},{60+3*44+32,116},
        {60+4*44+4,880},{60+1*44+12,8},{880,7},{884,0},{880+36,3},
        {880+40,0},{880+40,0x7f800000},{880+40,0xbf800000},
        {1180+32+4,1},{1180+32+8,1},{1180+32+12,2},
        {1180+32+16,5},{1180+32+20,UINT32_MAX},{1436+2*28+12,1},
        {1576,256},{1584+8,72},{1584+12,33}
    };
    for (unsigned i = 0; i < sizeof(changes)/sizeof(changes[0]); ++i)
        bad(changes[i].at, changes[i].value);
    const unsigned ends[] = {48,59,60,631,632,851,852,879,880,991,1103,1180,
                             1243,1435,1575,1599,1855,1927};
    for (unsigned i = 0; i < sizeof(ends)/sizeof(ends[0]); ++i) {
        unsigned n = ends[i]; unsigned char *short_file = malloc(n); assert(short_file);
        memcpy(short_file, original, n); put(short_file + 16, n);
        refuse(short_file, n); free(short_file);
    }
    assert(image[0] == 0x5a && image[1929] == 0x5a);

    /* Relocate and reverse the directory, preserving payload-relative table entries. */
    memcpy(image, original, 60); memset(image + 60, 0, 64);
    memcpy(image + 124, original + 60, sizeof(original) - 60);
    put(image + 16, 1992); put(image + 48, 124);
    for (unsigned i = 0; i < 13; ++i) {
        const unsigned char *old = original + 60 + i * 44;
        unsigned char *now = image + 124 + (12-i) * 44;
        memcpy(now, old, 44);
        uint32_t at = (uint32_t)old[4] | (uint32_t)old[5] << 8;
        put(now + 4, at + 64);
    }
    assert(nbg_model_open(image, 1992, &m) == NBG_DECODE_OK);
    assert(nbg_model_tensor(&m, NBG_OUTPUT, 0, &t) && t.logical_bytes == 72);
    assert(nbg_model_table(&m, NBG_COEFF_TABLE, 0, &span) && span.bytes == 256);

    /* The quantization union has different active members; inactive fields normalize to zero. */
    memcpy(image, original, sizeof(original)); put(image + 880 + 36, 1);
    put(image + 880 + 40, UINT32_MAX);
    assert(nbg_model_open(image, sizeof(original), &m) == NBG_DECODE_OK);
    assert(nbg_model_tensor(&m, NBG_INPUT, 0, &t));
    assert(t.fixed_position == -1 && !t.scale_bits && !t.zero_point);
    put(image + 880 + 36, 0);
    assert(nbg_model_open(image, sizeof(original), &m) == NBG_DECODE_OK);
    assert(nbg_model_tensor(&m, NBG_INPUT, 0, &t));
    assert(!t.fixed_position && !t.scale_bits && !t.zero_point);

    /* Optional pool and AXI requirements occupy separate bounded scalar sections. */
    memcpy(image, original, sizeof(original)); memset(image + 1928, 0, 12);
    put(image + 16, 1940);
    unsigned char *pool = image + 60 + 44, *axi = image + 60 + 2 * 44;
    put(pool, 37); put(pool + 4, 1928); put(pool + 12, 8); put(pool + 32, 8);
    put(axi, 39); put(axi + 4, 1936); put(axi + 12, 4); put(axi + 32, 4);
    put(image + 1928, 4096); put(image + 1936, 1024);
    assert(nbg_model_open(image, 1940, &m) == NBG_DECODE_OK);
    assert(m.header.pool_bytes == 4096 && m.header.axi_sram_bytes == 1024);
    put(image + 1932, 1); refuse(image, 1940); put(image + 1932, 0);
    put(pool + 12, 16); refuse(image, 1940);
    printf("PASS directory decoder: %u atomic refusals, unaligned reads, relocated/reversed directory, quantization union, execution refusal\n", refused);
}
