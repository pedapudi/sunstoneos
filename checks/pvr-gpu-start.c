#include "pvr_gpu_start.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct event { unsigned kind, offset, width; uint64_t value, time; };
struct model {
    uint64_t now, ace_done, release_done;
    unsigned io, fail_at, clock_calls, fail_clock, flag, published, released, aced;
    unsigned event_count, reads_started, auto_ready, floored, frozen;
    uint64_t counter, ace_counter, release_counter, ace_counts, release_counts;
    struct event events[64];
};
static unsigned char *image, *altered;
static size_t image_bytes;
static struct pvr_mips_boot boot;
static unsigned refusals, fault_trials;
static int operation(struct model *m) { return ++m->io != m->fail_at; }
/* Each clock read takes one microsecond, so waits inside a step end. With
 * floored set, each clock read and callback instead takes one 24 MHz counter
 * tick and the clock reports whole microseconds rounded down, as the board
 * clock does. With frozen set, the clock stops. */
static int now(void *ctx, uint64_t *value)
{
    struct model *m = ctx; ++m->clock_calls; ++m->counter;
    if (m->clock_calls == m->fail_clock) return 0;
    *value = m->floored ? 100 + m->counter / 24 : m->now;
    if (!m->frozen) ++m->now;
    return 1;
}
static void event(struct model *m, unsigned kind, unsigned off, unsigned width, uint64_t value)
{
    assert(m->event_count < 64 && m->published && (width == 4 || width == 8));
    m->events[m->event_count++] = (struct event){kind, off, width, value, m->now};
    ++m->counter;
    if (!m->frozen) m->now += 2; /* Callbacks take time; delay origins must follow completion. */
}
static int read_reg(void *ctx, uint32_t off, uint32_t width, uint64_t *value)
{
    struct model *m = ctx;
    if (!operation(m)) return 0;
    *value = off == 0x3800 ? UINT64_C(0xf1234567) : 0;
    event(m, 2, off, width, *value); return 1;
}
static int write_reg(void *ctx, uint32_t off, uint32_t width, uint64_t value)
{
    struct model *m = ctx;
    if (!operation(m)) return 0;
    if (off == 0x100 && value == 0) {
        assert(m->aced && m->now >= m->ace_done + 3); m->released = 1;
        m->ace_counts = m->counter - m->ace_counter;
        assert(!m->floored || m->ace_counts >= 72);
    }
    event(m, 1, off, width, value);
    if (off == 0x38c0) { m->ace_done = m->now; m->ace_counter = m->counter; m->aced = 1; }
    if (off == 0x100 && value == 0) { m->release_done = m->now; m->release_counter = m->counter; }
    return 1;
}
static int started(void *ctx, uint32_t *value)
{
    struct model *m = ctx;
    if (!operation(m)) return 0;
    assert(m->published); ++m->reads_started;
    if (m->released) {
        assert(m->now >= m->release_done + 3);
        if (!m->release_counts) m->release_counts = m->counter - m->release_counter;
        assert(!m->floored || m->release_counts >= 72);
        *value = m->auto_ready ? 1 : m->flag;
    } else *value = m->flag;
    return 1;
}
static int publish(void *ctx)
{
    struct model *m = ctx;
    if (!operation(m)) return 0;
    assert(!m->published && !m->event_count); m->published = 1; return 1;
}
static const struct pvr_gpu_start_ops ops = {now,read_reg,write_reg,started,publish};
static void prepare(struct pvr_gpu_start *s, struct model *m, const void *fw)
{
    memset(s,0,sizeof(*s)); memset(m,0,sizeof(*m)); m->now = 100; m->auto_ready = 1;
    assert(pvr_gpu_start_init(s,fw,image_bytes,&boot,&ops,m));
}
static enum pvr_gpu_start_result run(struct pvr_gpu_start *s, struct model *m)
{
    for (unsigned i = 0; i < 1000; ++i) {
        unsigned io = m->io, reads = m->reads_started;
        enum pvr_gpu_start_result r = pvr_gpu_start_poll(s);
        /* One device access per poll; only the readiness word is re-read. */
        assert(m->io - io <= 1 || m->io - io == m->reads_started - reads);
        if (r != PVR_GPU_START_AGAIN) return r;
        ++m->now;
    }
    assert(0); return PVR_GPU_START_INVALID;
}
static void retained(struct pvr_gpu_start *s, struct model *m, unsigned error)
{
    assert(s->phase == PVR_GPU_START_FAILED && s->error == error);
    unsigned io = m->io, clocks = m->clock_calls, index = s->index;
    assert(pvr_gpu_start_poll(s) == PVR_GPU_START_FAULT);
    assert(pvr_gpu_start_begin(s,10000) == PVR_GPU_START_INVALID);
    assert(pvr_gpu_start_cancel(s) == PVR_GPU_START_INVALID);
    assert(m->io == io && m->clock_calls == clocks && s->index == index); ++fault_trials;
}
static void refuse(const void *fw, size_t bytes, const struct pvr_mips_boot *b,
                   const struct pvr_gpu_start_ops *o)
{
    struct pvr_gpu_start s = {0}, before = s;
    assert(!pvr_gpu_start_init(&s,fw,bytes,b,o,NULL));
    assert(!memcmp(&s,&before,sizeof(s))); ++refusals;
}
static unsigned char *load(const char *path, size_t *bytes)
{
    FILE *f = fopen(path,"rb"); assert(f && !fseek(f,0,SEEK_END));
    long size = ftell(f); assert(size > 0 && !fseek(f,0,SEEK_SET));
    unsigned char *p = malloc((size_t)size); assert(p);
    assert(fread(p,1,(size_t)size,f) == (size_t)size && !fclose(f)); *bytes = (size_t)size; return p;
}
static FILE *output(const char *dir, const char *name)
{
    char path[4096]; int n = snprintf(path,sizeof(path),"%s/%s",dir,name);
    assert(n > 0 && (size_t)n < sizeof(path)); FILE *f = fopen(path,"wb"); assert(f); return f;
}
static void snapshot(const char *dir, unsigned variant, const struct model *m)
{
    char name[64]; snprintf(name,sizeof(name),"trace-%u.json",variant);
    FILE *f = output(dir,name); fprintf(f,"{\"events\":[");
    for (unsigned i = 0; i < m->event_count; ++i) {
        const struct event *e = &m->events[i];
        fprintf(f,"%s{\"kind\":%u,\"offset\":%u,\"width\":%u,\"value\":%llu,\"time\":%llu}",
            i ? "," : "",e->kind,e->offset,e->width,(unsigned long long)e->value,(unsigned long long)e->time);
    }
    fprintf(f,"],\"code\":%llu,\"data\":%llu,\"exceptions\":%llu}\n",
        (unsigned long long)(boot.writes[2].value & ~UINT64_C(4095)),
        (unsigned long long)(boot.writes[6].value & ~UINT64_C(4095)),
        (unsigned long long)(boot.writes[8].value & ~UINT64_C(4095)));
    assert(!fclose(f));
}
static void put64(unsigned char *p, uint64_t value)
{ for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(value >> (i * 8)); }
int main(int argc, char **argv)
{
    assert(argc == 4);
    image = load(argv[1],&image_bytes); altered = malloc(image_bytes); assert(altered);
    size_t n; unsigned char *wrapper = load(argv[2],&n); assert(n == 8 + sizeof(boot.writes));
    /* Saved wrapper evidence is little-endian; decode explicitly. */
    assert(!memcmp(wrapper,"\x24\0\0\0\x0b\0\0\0",8));
    boot.physical_address_bits=36; boot.write_count=11;
    for (unsigned i=0;i<11;++i) {
        uint64_t fields[3]={0}; const unsigned widths[]={4,4,8}; unsigned off=8+i*16;
        for (unsigned j=0;j<3;++j) for (unsigned k=0;k<widths[j];++k)
            fields[j] |= (uint64_t)wrapper[off++] << (k*8);
        boot.writes[i]=(struct pvr_mips_write){(uint32_t)fields[0],(uint32_t)fields[1],fields[2]};
    }
    free(wrapper);
    struct pvr_gpu_start s; struct model m;
    prepare(&s,&m,image); assert(s.count == 29);
    assert(pvr_gpu_start_poll(&s) == PVR_GPU_START_INVALID && !m.io && !m.clock_calls);
    assert(pvr_gpu_start_begin(&s,10000) == PVR_GPU_START_AGAIN);
    assert(run(&s,&m) == PVR_GPU_START_OK && s.phase == PVR_GPU_START_BOOTED);
    unsigned io_count=m.io, calls=m.clock_calls;
    assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_OK && m.io==io_count && m.clock_calls==calls);
    snapshot(argv[3],0,&m);

    /* One synthetic feature variant covers alternate reset masks and SLC branches. */
    memcpy(altered,image,image_bytes);
    altered[122880+48+32/8] &= (unsigned char)~(1u<<(32%8));
    altered[122880+48+48/8] &= (unsigned char)~(1u<<(48%8));
    altered[122880+48+65/8] &= (unsigned char)~(1u<<(65%8));
    altered[122880+48+16/8] &= (unsigned char)~(1u<<(16%8));
    altered[122880+32+11/8] |= (unsigned char)(1u<<(11%8));
    /* SLC line size is parameter 19 in the pinned feature stream. */
    put64(altered+122880+64+19*8,1024);
    prepare(&s,&m,altered); assert(s.count==27);
    assert(pvr_gpu_start_begin(&s,10000)==PVR_GPU_START_AGAIN && run(&s,&m)==PVR_GPU_START_OK);
    snapshot(argv[3],1,&m);
    FILE *f=output(argv[3],"variant.fw"); assert(fwrite(altered,1,image_bytes,f)==image_bytes && !fclose(f));

    /* Fail every distinct device callback in the successful sequence. */
    for (unsigned i=1;i<=io_count;++i) {
        prepare(&s,&m,image); m.fail_at=i;
        assert(pvr_gpu_start_begin(&s,10000)==PVR_GPU_START_AGAIN);
        assert(run(&s,&m)==PVR_GPU_START_FAULT && m.io==i); retained(&s,&m,PVR_GPU_START_IO);
    }
    prepare(&s,&m,image); m.fail_clock=1;
    assert(pvr_gpu_start_begin(&s,10000)==PVR_GPU_START_FAULT && !m.io); retained(&s,&m,PVR_GPU_START_IO);
    prepare(&s,&m,image); m.fail_clock=calls; /* Clock after observed readiness. */
    assert(pvr_gpu_start_begin(&s,10000)==PVR_GPU_START_AGAIN);
    assert(run(&s,&m)==PVR_GPU_START_FAULT); retained(&s,&m,PVR_GPU_START_IO);
    prepare(&s,&m,image);
    assert(pvr_gpu_start_begin(&s,100)==PVR_GPU_START_FAULT && !m.io); retained(&s,&m,PVR_GPU_START_TIMEOUT);
    prepare(&s,&m,image); assert(pvr_gpu_start_begin(&s,10000)==PVR_GPU_START_AGAIN);
    m.now-=2; assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_FAULT && !m.io); retained(&s,&m,PVR_GPU_START_CLOCK);
    for(unsigned bad=1;bad<=2;++bad) {
        prepare(&s,&m,image); m.flag=bad;
        assert(pvr_gpu_start_begin(&s,10000)==PVR_GPU_START_AGAIN && run(&s,&m)==PVR_GPU_START_FAULT);
        assert(!m.event_count); retained(&s,&m,bad==1 ? PVR_GPU_START_STALE : PVR_GPU_START_BAD_FLAG);
    }
    prepare(&s,&m,image); m.auto_ready=0;
    assert(pvr_gpu_start_begin(&s,300)==PVR_GPU_START_AGAIN && run(&s,&m)==PVR_GPU_START_FAULT);
    assert(m.released && m.reads_started>2); retained(&s,&m,PVR_GPU_START_TIMEOUT);
    /* A readiness wait re-reads for at most A733_GPU_SPIN_US per poll. */
    prepare(&s,&m,image); m.auto_ready=0; assert(pvr_gpu_start_begin(&s,10000)==PVR_GPU_START_AGAIN);
    while(s.index+1<s.count) assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_AGAIN);
    unsigned reads=m.reads_started; uint64_t waited=m.now;
    assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_AGAIN && s.phase==PVR_GPU_START_RUNNING);
    assert(m.reads_started-reads>1 && m.now-waited>=A733_GPU_SPIN_US && m.now-waited<=2*A733_GPU_SPIN_US+4);
    m.flag=1; assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_OK);
    /* A delay waits inside its step and ends at the deadline. */
    prepare(&s,&m,image); assert(pvr_gpu_start_begin(&s,10000)==PVR_GPU_START_AGAIN);
    while(!m.aced) assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_AGAIN);
    uint32_t delay_index=s.index;
    assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_AGAIN && s.index==delay_index+1 && m.now>=m.ace_done+3);
    /* A clock that rounds counter ticks down to microseconds still gives
     * each delay at least 3 us between the bracketing callbacks. */
    prepare(&s,&m,image); m.floored=1;
    assert(pvr_gpu_start_begin(&s,10000)==PVR_GPU_START_AGAIN && run(&s,&m)==PVR_GPU_START_OK);
    unsigned ace_counts=(unsigned)m.ace_counts, release_counts=(unsigned)m.release_counts;
    assert(ace_counts>=72 && release_counts>=72);
    /* A stopped clock cannot end a delay or a readiness wait: the poll returns
     * AGAIN without advancing the step and completes once the clock runs. */
    prepare(&s,&m,image); assert(pvr_gpu_start_begin(&s,10000)==PVR_GPU_START_AGAIN);
    while(!m.aced) assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_AGAIN);
    delay_index=s.index; m.frozen=1;
    for(unsigned i=0;i<2;++i) assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_AGAIN && s.index==delay_index);
    m.frozen=0; assert(run(&s,&m)==PVR_GPU_START_OK);
    prepare(&s,&m,image); m.auto_ready=0; assert(pvr_gpu_start_begin(&s,10000)==PVR_GPU_START_AGAIN);
    while(s.index+1<s.count) assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_AGAIN);
    m.frozen=1; assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_AGAIN && s.phase==PVR_GPU_START_RUNNING);
    m.frozen=0; m.flag=1; assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_OK);
    prepare(&s,&m,image); assert(pvr_gpu_start_begin(&s,10000)==PVR_GPU_START_AGAIN);
    while(!m.aced) assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_AGAIN);
    m.now=10000-2; assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_FAULT && !m.released);
    retained(&s,&m,PVR_GPU_START_TIMEOUT);
    prepare(&s,&m,image); assert(pvr_gpu_start_begin(&s,10000)==PVR_GPU_START_AGAIN);
    assert(pvr_gpu_start_poll(&s)==PVR_GPU_START_AGAIN);
    assert(pvr_gpu_start_cancel(&s)==PVR_GPU_START_FAULT); retained(&s,&m,PVR_GPU_START_CANCELLED);

    refuse(NULL,image_bytes,&boot,&ops); refuse(image,image_bytes-1,&boot,&ops);
    refuse(image,image_bytes,NULL,&ops); refuse(image,image_bytes,&boot,NULL);
    struct pvr_gpu_start_ops bad_ops=ops; bad_ops.write=NULL; refuse(image,image_bytes,&boot,&bad_ops);
    struct pvr_mips_boot bad=boot; bad.write_count=10; refuse(image,image_bytes,&bad,&ops);
    bad=boot; bad.physical_address_bits=32; refuse(image,image_bytes,&bad,&ops);
    for(unsigned i=0;i<11;++i) {
        bad=boot; bad.writes[i].offset ^= 4; refuse(image,image_bytes,&bad,&ops);
        bad=boot; bad.writes[i].width ^= 12; refuse(image,image_bytes,&bad,&ops);
    }
    bad=boot; bad.writes[4].value+=4096; refuse(image,image_bytes,&bad,&ops);
    bad=boot; bad.writes[6].value=boot.writes[2].value; refuse(image,image_bytes,&bad,&ops);
    const uint64_t bad_pa[]={12,0x1fc0000c,UINT64_C(0x100000000c),0x100d};
    for(unsigned i=0;i<4;++i) { bad=boot; bad.writes[6].value=bad_pa[i]; refuse(image,image_bytes,&bad,&ops); }
    memcpy(altered,image,image_bytes); altered[122880+32+9/8]&=(unsigned char)~(1u<<(9%8));
    refuse(altered,image_bytes,&boot,&ops);
    memcpy(altered,image,image_bytes); altered[122880+48+71/8]|=(unsigned char)(1u<<(71%8));
    refuse(altered,image_bytes,&boot,&ops);
    const uint64_t bad_line[]={0,513,65536};
    for(unsigned i=0;i<3;++i) {
        memcpy(altered,image,image_bytes); put64(altered+122880+64+19*8,bad_line[i]);
        refuse(altered,image_bytes,&boot,&ops);
    }
    prepare(&s,&m,image); struct pvr_gpu_start before=s;
    assert(!pvr_gpu_start_init(&s,image,image_bytes,&boot,&ops,&m) && !memcmp(&s,&before,sizeof(s)));
    free(altered); free(image);
    printf("PASS GPU startup: reference traces, callback timing, delays of %u and %u counter ticks, stopped clock, %u retained faults and %u atomic refusals\n",
           ace_counts,release_counts,fault_trials,refusals);
}
