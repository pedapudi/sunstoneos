/* CPU command extents must be admitted before any input read or output write. */
#include "a733_display_engine.h"
#include "a733_g2d.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int previous_commands(void *, size_t, const struct a733_g2d_session *);

#define REQUIRE(c) do { if (!(c)) { \
    fprintf(stderr, "line %u: %s\n", __LINE__, #c); return 1; \
} } while (0)

static struct a733_g2d_session session(void)
{
    return (struct a733_g2d_session){.width=64, .height=48, .stride=272,
        .op=A733_G2D_BLIT, .dx=11, .dy=5, .w=28, .h=20, .sx=3, .sy=9};
}

static int overlap(void)
{
    _Alignas(32) uint8_t arena[8192], saved[sizeof(arena)];
    uint8_t *commands=arena+1024;
    /* Session contained in output, session overlapping either edge. */
    const unsigned at[]={1024, 1056, 128, 5112};
    for (unsigned i=0; i<sizeof(at)/sizeof(*at); ++i) {
        memset(arena, 0xa5, sizeof(arena));
        struct a733_g2d_session *s=(void *)(arena+at[i]);
        *s=session();
        memcpy(saved, arena, sizeof(arena));
        REQUIRE(!a733_g2d_commands(commands, 4096, s));
        REQUIRE(!memcmp(arena, saved, sizeof(arena)));
    }
    return 0;
}

static int g2d_valid(void)
{
    _Alignas(32) uint8_t actual[4096], previous[4096];
    struct a733_g2d_session s=session(), saved;
    unsigned cases=0;
    for (unsigned op=A733_G2D_FILL; op<=A733_G2D_SRCOVER_IMAGE; ++op) {
        for (unsigned in=0; in<8; ++in) for (unsigned out=0; out<8; ++out) {
            if (op!=A733_G2D_BLIT && op!=A733_G2D_SCALE && (in || out)) continue;
            s=session(); s.op=op; s.format=in; s.output_format=out;
            if (op==A733_G2D_FILL) { s.sx=s.sy=0; s.color=0x125678ab; }
            if (op==A733_G2D_SRCOVER_SOLID) s.color=0xff4488cc;
            if (op==A733_G2D_SCALE) { s.sw=33; s.sh=27; }
            if (op==A733_G2D_SRCOVER_IMAGE) { s.bx=17; s.by=13; s.alpha=157; }
            saved=s;
            REQUIRE(a733_g2d_geometry_valid(&s));
            REQUIRE(previous_commands(previous, sizeof(previous), &s));
            REQUIRE(a733_g2d_commands(actual, sizeof(actual), &s));
            REQUIRE(!memcmp(actual, previous, sizeof(actual)));
            REQUIRE(!memcmp(&s, &saved, sizeof(s)));
            ++cases;
        }
    }
    /* Adjacent session and command extents are legal in either order. */
    _Alignas(32) uint8_t arena[8192];
    for (unsigned after=0; after<2; ++after) {
        uint8_t *commands=arena+1024;
        struct a733_g2d_session *adjacent=(void *)(after ? commands+4096 : commands-sizeof(s));
        *adjacent=session(); saved=*adjacent;
        REQUIRE(a733_g2d_commands(commands, 4096, adjacent));
        REQUIRE(!memcmp(adjacent, &saved, sizeof(saved)));
    }
    printf("PASS %u unchanged G2D operation/format pages and adjacent sessions\n", cases);
    return 0;
}

static int wrapping(void)
{
    _Alignas(32) uint8_t commands[4096], saved[4096];
    struct a733_g2d_session s=session();
    void *wrapped_page=(void *)(UINTPTR_MAX-(uintptr_t)31);
    const struct a733_g2d_session *wrapped_session=(const void *)(UINTPTR_MAX-(uintptr_t)31);
    memset(commands, 0xa5, sizeof(commands)); memcpy(saved, commands, sizeof(saved));
    REQUIRE(!a733_g2d_commands(wrapped_page, 4096, &s));
    REQUIRE(!a733_g2d_commands(commands, 4096, wrapped_session));
    REQUIRE(!memcmp(commands, saved, sizeof(commands)));
    REQUIRE(!a733_g2d_commands(NULL, 4096, &s));
    REQUIRE(!a733_g2d_commands(commands, 4096, NULL));
    REQUIRE(!a733_g2d_commands(commands, 4095, &s));
    REQUIRE(!a733_g2d_commands(commands+1, 4096, &s));
    REQUIRE(!memcmp(commands, saved, sizeof(commands)));
    return 0;
}

static int rings(int wrapped_only)
{
    _Alignas(4096) uint8_t pages[4*4096], saved[sizeof(pages)];
    const uint64_t base=0x10000000, frame=16384;
    void *wrapped=(void *)(UINTPTR_MAX-(uintptr_t)4095);
    for (unsigned n=2; n<=4; ++n) {
        REQUIRE(!a733_de_ring_build(wrapped, base, frame, n));
        REQUIRE(!a733_de_ring_valid(wrapped, base, frame, n));
    }
    REQUIRE(!a733_de_replay_build(wrapped, base, frame));
    REQUIRE(!a733_de_replay_valid(wrapped, base, frame));
    if (wrapped_only) return 0;
    for (unsigned n=2; n<=4; ++n) {
        memset(pages, 0xa5, sizeof(pages));
        REQUIRE(a733_de_ring_build(pages, base, frame, n));
        for (unsigned slot=0; slot<n; ++slot) {
            uint32_t expected[16]={0};
            expected[0]=(uint32_t)(base+n*frame+slot*4096+32);
            expected[1]=4; expected[2]=1; expected[3]=0x221010;
            expected[8]=(uint32_t)(base+slot*frame);
            REQUIRE(!memcmp(pages+slot*4096, expected, sizeof(expected)));
            for (unsigned i=64; i<4096; ++i) REQUIRE(!pages[slot*4096+i]);
        }
        for (unsigned i=n*4096; i<sizeof(pages); ++i) REQUIRE(pages[i]==0xa5);
        memcpy(saved, pages, sizeof(pages));
        REQUIRE(a733_de_ring_valid(pages, base, frame, n));
        REQUIRE(!memcmp(pages, saved, sizeof(pages)));
        pages[n*4096-1]=1;
        REQUIRE(!a733_de_ring_valid(pages, base, frame, n));
    }
    REQUIRE(a733_de_replay_build(pages, base, frame));
    REQUIRE(a733_de_replay_valid(pages, base, frame));
    for (unsigned n=0; n<=5; ++n) if (n<2 || n>4) {
        memcpy(saved, pages, sizeof(pages));
        REQUIRE(!a733_de_ring_build(pages, base, frame, n));
        REQUIRE(!a733_de_ring_valid(pages, base, frame, n));
        REQUIRE(!memcmp(pages, saved, sizeof(pages)));
    }
    puts("PASS display rings: two to four slots, legacy wrappers, bounds and immutable validation");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc==2 && !strcmp(argv[1], "overlap")) return overlap();
    if (argc==2 && !strcmp(argv[1], "ring-wrap")) return rings(1);
    if (overlap() || wrapping() || g2d_valid() || rings(0)) return 1;
    puts("PASS command admission refuses aliases and wrapping CPU spans before access");
    return 0;
}
