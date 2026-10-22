/* Count the work of local NPU startup and local stop in the existing platform
 * register model. One poll stands for one bootstrap tick, as in the native
 * bootstrap loop. Pass "budget" to also enforce the limits this controller
 * meets; the local stop limits are the design's local-gate budget. */
#define main platform_model_main
#include "npu-platform.c"
#undef main

static unsigned clock_reads, lock_enters;
static int counted_now(void *v, uint64_t *out) { ++clock_reads; return now(v, out); }
static int counted_enter(void *v, uint64_t d) { ++lock_enters; return enter(v, d); }
struct cost { unsigned polls, reads, writes, clock_reads, locks; };
static void begin(struct model *m, struct cost *c)
{
    *c = (struct cost){0, m->reads, m->nwrite, clock_reads, lock_enters};
}
static void end(struct model *m, struct cost *c, unsigned polls)
{
    *c = (struct cost){polls, m->reads - c->reads, m->nwrite - c->writes,
        clock_reads - c->clock_reads, lock_enters - c->locks};
}
static void print(const char *name, const struct cost *c)
{
    printf("%s polls=%u reads=%u writes=%u clock_reads=%u locks=%u\n",
        name, c->polls, c->reads, c->writes, c->clock_reads, c->locks);
}
int main(int argc, char **argv)
{
    int budget = argc == 2 && !strcmp(argv[1], "budget");
    struct model m; setup(&m, 2);
    m.config.deadline_us = m.p.config.deadline_us = 100000000;
    m.p.ops.now_us = counted_now; m.p.ops.enter = counted_enter;
    struct cost start, stop;
    begin(&m, &start);
    enum a733_npu_platform_result r = PENDING; unsigned polls = 0;
    while (r != READY && r != RETAINED && polls < 200) { r = a733_npu_platform_poll(&m.p); ++polls; m.now += 1000; }
    assert(r == READY); end(&m, &start, polls);
    begin(&m, &stop); r = PENDING; polls = 0;
    while (r != READY && r != RETAINED && polls < 50) { r = a733_npu_platform_stop_poll(&m.p); ++polls; m.now += 1000; }
    assert(r == READY); end(&m, &stop, polls);
    print("startup", &start); print("stop", &stop);
    if (budget) {
        /* Acquisition, one locked open pass, the 2 ms settle and the final check. */
        assert(start.polls <= 4 && start.locks <= 3 && start.writes == 12);
        assert(start.clock_reads <= 16 && start.reads <= 60);
        assert(stop.polls == 1 && stop.locks == 1 && stop.writes == 5);
        assert(stop.reads <= 12 && stop.clock_reads <= 6);
        puts("PASS NPU platform startup and local stop within their pass and sample limits");
    }
    return 0;
}
