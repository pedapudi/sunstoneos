/* SPDX-License-Identifier: Apache-2.0 */
/* Compares the application helpers that moved onto include/a733_range.h and
 * include/a733_mapping_check.h with the inline code they replaced. Each
 * former form is copied here verbatim apart from the translate callback, and
 * both forms run against the same translation model over many seeds. */
#include "a733_mapping_check.h"
#include "a733_range.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t seed;
static unsigned bad_per_mille;
static unsigned long translations;

static uint64_t mix(uint64_t x)
{
    x ^= x >> 33; x *= UINT64_C(0xff51afd7ed558ccd); x ^= x >> 33;
    x *= UINT64_C(0xc4ceb9fe1a85ec53); x ^= x >> 33; return x;
}

/* Most addresses translate as identity, write-back or Normal-NC with a valid
 * shareability; a few fail or carry a wrong field. Results depend on the
 * address, the direction and the seed. */
static int model(uintptr_t address, int write, struct aarch64_translation *t)
{
    static const unsigned attributes[] = {0x00, 0x04, 0x44, 0xff};
    uint64_t h = mix(seed ^ mix(address) ^ (uint64_t)write * 0x9e3779b97f4a7c15u);
    ++translations;
    unsigned bad = (unsigned)(h % 1000) < bad_per_mille;
    if (bad && (h >> 10 & 3) == 0) return 0;
    t->physical = address ^ (bad && (h >> 10 & 3) == 1 ? 0x1000 : 0);
    t->attributes = attributes[(seed >> 8) & 3];
    if (bad && (h >> 10 & 3) == 2) t->attributes = attributes[h >> 12 & 3];
    t->shareability = (unsigned)(seed >> 4 & 3);
    if (bad && (h >> 10 & 3) == 3) t->shareability = (unsigned)(h >> 14 & 3);
    return 1;
}

static const struct a733_mapping_rule write_outer = {A733_MAPPING_WRITE, 1, {{0x00, A733_SH_OUTER}}, 0, 1};
static const struct a733_mapping_rule read_write_any = {A733_MAPPING_READ_WRITE, 1, {{0x00, A733_SH_ANY}}, 0, 1};
static const struct a733_mapping_rule read_any = {A733_MAPPING_READ, 1, {{0x00, A733_SH_ANY}}, 0, 1};
static const struct a733_mapping_rule write_any = {A733_MAPPING_WRITE, 1, {{0x00, A733_SH_ANY}}, 0, 1};
static const struct a733_mapping_rule wb_read = {A733_MAPPING_READ, 1, {{0xff, A733_SH_INNER}}, 0, 1};
static const struct a733_mapping_rule wb_write = {A733_MAPPING_WRITE, 1, {{0xff, A733_SH_INNER}}, 0, 1};
static const struct a733_mapping_rule nc[2] = {
    {A733_MAPPING_READ, 1, {{0x44, A733_SH_OUTER | A733_SH_INNER}}, 0, 1},
    {A733_MAPPING_WRITE, 1, {{0x44, A733_SH_OUTER | A733_SH_INNER}}, 0, 1}};

/* apps/adc, apps/crypto device(), apps/deinterlace, apps/g2d, apps/h264,
 * apps/h264-encode, apps/hevc, apps/gpu-display/usb.c, apps/hdmi-audio/usb.c */
static int old_write_outer(uintptr_t a)
{struct aarch64_translation t;return model(a,1,&t)&&t.physical==a&&t.attributes==0&&t.shareability==2;}
/* apps/camera/capture.c, apps/displayport/native.c mapped() */
static int old_read_write_any(uintptr_t a)
{struct aarch64_translation r,w;return model(a,0,&r)&&model(a,1,&w)&&r.physical==a&&w.physical==a&&r.attributes==0&&w.attributes==0;}
/* apps/hdmi-audio/native.c rd(), apps/gpu-finite/finite.c irq_read() */
static int old_read_any(uintptr_t a)
{struct aarch64_translation r;return model(a,0,&r)&&r.physical==a&&r.attributes==0;}
/* apps/hdmi-audio/native.c wr() */
static int old_write_any(uintptr_t a)
{struct aarch64_translation w;return model(a,1,&w)&&w.physical==a&&!w.attributes;}
/* apps/bluetooth/application.c profile pages, apps/hdmi-audio/native.c PCM pages */
static int old_wb_pages(uintptr_t start, uintptr_t end)
{
    for(uintptr_t page=start;page<end;page+=4096){
        struct aarch64_translation t;
        if(!model(page,0,&t)||t.physical!=page||t.attributes!=0xff||t.shareability!=3)return 0;
    }
    return 1;
}
/* apps/bluetooth/application.c model pages, with the hole */
static int old_model_pages(uintptr_t base, uintptr_t gap, uintptr_t resume, uintptr_t end)
{
    for(uintptr_t page=base;page<end;page+=4096){
        if(page>=gap&&page<resume)continue;
        struct aarch64_translation t;
        if(!model(page,0,&t)||t.physical!=page||t.attributes!=0xff||t.shareability!=3)return 0;
    }
    return 1;
}
static int new_model_pages(uintptr_t base, uintptr_t gap, uintptr_t resume, uintptr_t end)
{
    return a733_mapping_check_stride(model,base,base,gap-base,4096,&wb_read)&&
        a733_mapping_check_stride(model,resume,resume,end-resume,4096,&wb_read);
}
/* apps/crypto/native.c descriptor lines */
static int old_descriptor(uintptr_t cpu, uint64_t physical, size_t bytes)
{
    for(uintptr_t p=cpu;p<cpu+bytes;p+=64){struct aarch64_translation x;
        if(!model(p,1,&x)||x.physical!=physical+p-cpu||x.attributes!=0xff||x.shareability!=3)return 0;}
    return 1;
}
/* apps/hdmi-audio/native.c nc_mapping() walk */
static int old_nc(uintptr_t cpu, uint64_t bus, size_t bytes, int descriptor)
{
    for(size_t off=0;off<bytes;off+=4096){
        struct aarch64_translation t;
        if(!model(cpu+off,descriptor,&t)||
           t.physical!=bus+off||t.attributes!=0x44||(t.shareability!=2&&t.shareability!=3))return 0;
    }
    return 1;
}
/* apps/board/radio.c separate() */
static int old_separate(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);
}

static unsigned long cases, failures;
static void expect(const char *name, int old, int now, uint64_t detail)
{
    ++cases;
    if (old != now) {
        ++failures;
        if (failures < 20) printf("FAIL %s: old %d new %d seed %#llx detail %#llx\n", name, old, now,
                                  (unsigned long long)seed, (unsigned long long)detail);
    }
}

int main(void)
{
    static const uint64_t edges[] = {0, 1, 2, 3, 4, 0x1000, 0x1001, UINT64_MAX / 2, UINT64_MAX - 4096,
                                     UINT64_MAX - 1, UINT64_MAX};
    unsigned long passes[16] = {0};
    for (unsigned s = 0; s < 4096; ++s) {
        seed = mix(s + 1);
        bad_per_mille = s % 4 == 0 ? 0 : s % 4 == 1 ? 5 : s % 4 == 2 ? 50 : 400;
        uintptr_t a = 0x02000000 + (uintptr_t)(mix(seed) % 0x100000) * 4;
        int r;
        r = a733_mapping_check(model, a, a, &write_outer); expect("write-outer", old_write_outer(a), r, a); passes[0] += r;
        r = a733_mapping_check(model, a, a, &read_write_any); expect("read-write-any", old_read_write_any(a), r, a); passes[1] += r;
        r = a733_mapping_check(model, a, a, &read_any); expect("read-any", old_read_any(a), r, a); passes[2] += r;
        r = a733_mapping_check(model, a, a, &write_any); expect("write-any", old_write_any(a), r, a); passes[3] += r;
        uintptr_t start = 0x43000000 + (uintptr_t)(mix(seed ^ 1) % 64) * 4096;
        size_t pages = 1 + (size_t)(mix(seed ^ 2) % 12);
        r = a733_mapping_check_stride(model, start, start, pages * 4096, 4096, &wb_read);
        expect("wb-pages", old_wb_pages(start, start + pages * 4096), r, start); passes[4] += r;
        size_t pcm = 1 + (size_t)(mix(seed ^ 3) % (6 * 4096));
        r = a733_mapping_check_stride(model, 0x41f10000, 0x41f10000, pcm, 4096, &wb_read);
        expect("pcm-pages", old_wb_pages(0x41f10000, 0x41f10000 + pcm), r, pcm); passes[5] += r;
        uintptr_t base = 0x44000000 + (uintptr_t)(mix(seed ^ 4) % 4) * 4096;
        r = new_model_pages(base, base + 5 * 4096, base + 9 * 4096, base + 12 * 4096);
        expect("model-pages", old_model_pages(base, base + 5 * 4096, base + 9 * 4096, base + 12 * 4096), r, base);
        passes[6] += r;
        uintptr_t cpu = 0x41000000 + (uintptr_t)(mix(seed ^ 5) % 256) * 64;
        uint64_t physical = cpu + (mix(seed ^ 6) % 2 ? 0 : 0x100000);
        r = a733_mapping_check_stride(model, cpu, physical, 4096, 64, &wb_write);
        expect("descriptor", old_descriptor(cpu, physical, 4096), r, cpu); passes[7] += r;
        int descriptor = (int)(mix(seed ^ 7) & 1);
        size_t bytes = descriptor ? 4096 : 4096 * (1 + (size_t)(mix(seed ^ 8) % 8));
        uint64_t bus = cpu + (mix(seed ^ 9) % 2 ? 0 : 0x2000);
        r = a733_mapping_check_stride(model, cpu, bus, bytes, 4096, &nc[descriptor]);
        expect("nc", old_nc(cpu, bus, bytes, descriptor), r, bytes); passes[8] += r;
    }
    /* Deadline sampler: refuse a backward sample or one at or after the
     * deadline, and store the sample only on admission. */
    for (unsigned i = 0; i < sizeof(edges) / sizeof(*edges); ++i)
        for (unsigned j = 0; j < sizeof(edges) / sizeof(*edges); ++j)
            for (unsigned k = 0; k < sizeof(edges) / sizeof(*edges); ++k) {
                uint64_t t = edges[i], old_last = edges[j], new_last = edges[j], deadline = edges[k];
                int old = 1;
                if (t < old_last || t >= deadline) old = 0; else old_last = t;
                expect("sampler", old, a733_deadline_admit(t, &new_last, deadline), t);
                expect("sampler-stored", 1, old_last == new_last, t);
                uint64_t old_out = 7, new_out = 7, delta = edges[k];
                int sum = 1;
                if (t > UINT64_MAX - delta) sum = 0; else old_out = t + delta;
                expect("deadline-sum", sum, a733_deadline_after(t, delta, &new_out), t);
                expect("deadline-sum-stored", 1, old_out == new_out, t);
                for (unsigned l = 0; l < sizeof(edges) / sizeof(*edges); ++l)
                    expect("separate", old_separate((void *)(uintptr_t)edges[i], (size_t)edges[j],
                                                    (void *)(uintptr_t)edges[k], (size_t)edges[l]),
                           a733_ranges_disjoint((void *)(uintptr_t)edges[i], (size_t)edges[j],
                                                (void *)(uintptr_t)edges[k], (size_t)edges[l]), edges[l]);
            }
    printf("%lu cases, %lu failures, %lu translations; accepted:", cases, failures, translations);
    for (unsigned i = 0; i < 9; ++i) printf(" %lu", passes[i]);
    printf("\n");
    /* Every mapping form must be exercised on both outcomes. */
    for (unsigned i = 0; i < 9; ++i)
        if (!passes[i] || passes[i] == 4096) { printf("FAIL form %u saw one outcome only\n", i); return 1; }
    return failures != 0;
}
