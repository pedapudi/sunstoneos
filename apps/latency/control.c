#include "control.h"

static const unsigned cores[LATENCY_CONTROL_COUNT] = {1, 2, 3, 4, 5, 6, 7};
static const unsigned busy_rates[LATENCY_CONTROL_COUNT] = {1000, 2000, 4000, 8000, 10000, 4000, 4000};
static const unsigned idle_rates[LATENCY_CONTROL_COUNT] = {10000, 10000, 10000, 10000, 10000, 10000, 10000};
static const unsigned busy_paths[LATENCY_CONTROL_COUNT] = {3, 3, 3, 3, 3, 3, 3};
static const unsigned idle_paths[LATENCY_CONTROL_COUNT] = {1, 1, 1, 1, 1, 1, 1};
static unsigned started, selected_mode, ready_seen;
#define REQUESTED_MASK UINT32_C(0xfe)

int latency_control_start(unsigned mode)
{
    if (started || mode > LATENCY_CONTROL_IDLE) return 0;
    started = 1;
    selected_mode = mode;
    const unsigned *rates = mode == LATENCY_CONTROL_IDLE ? idle_rates : busy_rates;
    const unsigned *paths = mode == LATENCY_CONTROL_IDLE ? idle_paths : busy_paths;
    return ulSunstoneControlStartPathsMode(cores, rates, paths, LATENCY_CONTROL_COUNT,
                                          SS_CONTROL_HISTOGRAM_ONLY) == LATENCY_CONTROL_COUNT;
}
int latency_control_ready(void)
{
    struct ss_control_progress progress;
    if (!started || !ulSunstoneControlProgressRead(&progress)) return 0;
    ready_seen |= progress.armed_mask;
    return progress.requested_mask == REQUESTED_MASK && progress.armed_mask == REQUESTED_MASK;
}
static unsigned complete_mask(const struct ss_control_progress *progress)
{
    unsigned mask = 0;
    for (unsigned c = 1; c < SS_CONTROL_CORES; ++c)
        if (progress->count[c][0] > LATENCY_CONTROL_MIN_SAMPLES &&
            (selected_mode == LATENCY_CONTROL_IDLE || progress->count[c][1] >= LATENCY_CONTROL_MIN_SAMPLES))
            mask |= 1u << c;
    return mask;
}
int latency_control_complete(void)
{
    struct ss_control_progress progress;
    if (!started || !ulSunstoneControlProgressRead(&progress)) return 0;
    ready_seen |= progress.armed_mask;
    for (unsigned c = 1; c < SS_CONTROL_CORES; ++c) if (progress.errors[c]) return -1;
    return progress.requested_mask == REQUESTED_MASK && complete_mask(&progress) == REQUESTED_MASK;
}
void latency_control_stop(void)
{
    if (started) {
        (void)latency_control_ready();
        vSunstoneControlStop();
    }
}
int latency_control_stopped(void)
{ return started && ulSunstoneControlQuiescent(); }

int latency_control_snapshot(struct latency_control_report *out)
{
    struct ss_control_progress progress;
    if (!out || !latency_control_stopped() || !ulSunstoneControlProgressRead(&progress)) return 0;
    *out = (struct latency_control_report){0};
    out->version = LATENCY_CONTROL_VERSION;
    out->frequency_hz = progress.frequency_hz;
    out->minimum_samples = LATENCY_CONTROL_MIN_SAMPLES;
    out->requested_mask = progress.requested_mask;
    out->mode = selected_mode;
    out->ready_mask = ready_seen;
    out->quiescent_mask = progress.quiescent_mask;
    out->consumer_parked = progress.consumer_parked;
    out->complete_mask = complete_mask(&progress);
    out->flags = SS_CONTROL_HISTOGRAM_ONLY;
    for (unsigned i = 0; i < LATENCY_CONTROL_COUNT; ++i) {
        const unsigned c = cores[i];
        struct latency_control_core *row = &out->core[i];
        row->core = c;
        row->rate_hz = selected_mode == LATENCY_CONTROL_IDLE ? idle_rates[i] : busy_rates[i];
        row->paths = selected_mode == LATENCY_CONTROL_IDLE ? idle_paths[i] : busy_paths[i];
        (void)ulSunstoneControlStatsRead(c, 0, &row->stats[0]);
        if (row->paths & 2u) {
            (void)ulSunstoneControlStatsRead(c, 1, &row->stats[1]);
            struct ss_control_metric metric;
            if (!ulSunstoneControlMetricRead(c, 1, 2, &metric)) return 0;
            row->task_entry_max_ticks = metric.maximum_ticks;
            row->task_entry_p9999_ticks = metric.p9999_upper_ticks;
            if (!ulSunstoneControlMetricRead(c, 1, 3, &metric)) return 0;
            row->task_gap_max_ticks = metric.maximum_ticks;
            row->task_gap_p9999_ticks = metric.p9999_upper_ticks;
        }
        row->published_control = progress.count[c][0];
        row->published_task = progress.count[c][1];
        for (unsigned h = 0; h < LATENCY_CONTROL_HISTOGRAMS; ++h) {
            const unsigned path = h == LATENCY_CONTROL_IRQ_JITTER ? 0 : 1;
            const unsigned kind = h == LATENCY_CONTROL_TASK_LATENCY ? 0 :
                h == LATENCY_CONTROL_TASK_GAP ? 3 : 1;
            const uint32_t *bins = ulSunstoneControlHistogram(c, path, kind);
            if (bins) for (unsigned b = 0; b < SS_CONTROL_BINS; ++b) row->histogram[h][b] = bins[b];
        }
    }
    /* Statistics conversion can publish a representability error. Capture it
     * after the getters, while all data writers remain quiescent. */
    if (!ulSunstoneControlProgressRead(&progress)) return 0;
    for (unsigned i = 0; i < LATENCY_CONTROL_COUNT; ++i) {
        out->core[i].error_flags = progress.errors[cores[i]];
        out->errors |= out->core[i].error_flags;
    }
    out->magic = LATENCY_CONTROL_MAGIC;
    return 1;
}
