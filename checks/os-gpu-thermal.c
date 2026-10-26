/* Actual native GPU thermal caller; service replies and scheduler are modeled. */
#include "../apps/os/gpu.c"

struct board_application board;
struct board_provider providers;
volatile unsigned os_check_result;
static unsigned mode, throttled, waiting, sends, closes, opens, pending, polls;
static uint64_t time_us = 100, model_epoch = 1;
static TickType_t extra_ticks;
static unsigned receives, submissions;
static unsigned model_phase = PVR_GPU_SERVICE_READY;
static void check(int ok, unsigned code)
{ if (!ok) { os_check_result = code; __asm__ volatile("brk #0"); for (;;) {} } }
uint64_t a733_smp_counter(void) { return time_us * 24; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(time_us / 1000) + extra_ticks; }
int board_live(void *unused)
{ (void)unused; return !atomic_load(&board.fatal) && time_us < board.session.deadline_us &&
    time_us < atomic_load(&board.live_until); }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)(uintptr_t)(mode == 4 || mode == 11 || mode == 30 ? 2 : 1); }
int os_thermal_throttled(void) { return throttled; }
void os_dispatch_waiting(int value) { waiting = (unsigned)value; }
void vTaskDelay(TickType_t ticks)
{ check(ticks == 1, 2); if (mode == 27) extra_ticks += ticks; else time_us += 1000; }
int pvr_gpu_service_send_epoch(struct pvr_gpu_service *s, uint64_t epoch,
    const struct pvr_gpu_service_request *request, TickType_t wait, uint64_t *ticket)
{
    if (mode == 23) return 0; /* No request entered the service queue. */
    if (mode == 12 && pending) return 0; /* existing service ticket remains owed */
    check(s == &gpu.service && epoch == model_epoch && wait == OS_GPU_WAIT_TICKS && !pending, 3);
    check(board_live(NULL) && request->timeout_us <= OS_GPU_OPEN_US, 4);
    ++sends; pending = request->operation + 1; *ticket = sends;
    if (request->operation == PVR_GPU_SERVICE_CLOSE) {
        check(model_phase == PVR_GPU_SERVICE_READY && request->timeout_us, 5);
        if (mode >= 7) check(gpu.retired && waiting, 26);
        ++closes; model_phase = PVR_GPU_SERVICE_STOPPING; polls = 0;
    } else if (request->operation == PVR_GPU_SERVICE_OPEN) {
        check(model_phase == PVR_GPU_SERVICE_STOPPED && request->timeout_us, 6);
        ++opens; ++model_epoch; model_phase = PVR_GPU_SERVICE_READY;
    } else {
        check(request->operation == PVR_GPU_SERVICE_STATUS && !request->timeout_us, 7);
        if (++polls == 2 && mode != 27) model_phase = PVR_GPU_SERVICE_STOPPED;
    }
    return 1;
}
int pvr_gpu_service_collect(struct pvr_gpu_service *s, uint64_t ticket,
    struct pvr_gpu_service_reply *reply, TickType_t wait)
{
    check(s == &gpu.service && ticket == sends && wait == OS_GPU_WAIT_TICKS && pending, 8);
    if (mode == 22) { time_us += 10000; return 0; }
    if (mode == 3 || mode == 9) { time_us += 200000; return 0; }
    *reply = (struct pvr_gpu_service_reply){.ticket = ticket, .epoch = model_epoch,
        .result = PVR_GPU_SERVICE_OK, .phase = model_phase};
    if (mode == 8) { reply->result = PVR_GPU_SERVICE_BUSY; model_phase = PVR_GPU_SERVICE_READY; }
    if (mode == 10) atomic_store(&board.fatal, 1);
    if (mode == 13 && model_phase == PVR_GPU_SERVICE_STOPPED) ++reply->epoch;
    if (mode == 24) time_us = 1000100;
    if (mode == 28) time_us += 600000;
    pending = 0; return 1;
}
int pvr_mimus_service_receive(struct pvr_mimus_service *s, struct pvr_mimus_record *record, TickType_t wait)
{
    if (mode >= 14) {
        check(s == &gpu.session.component && wait == OS_GPU_WAIT_TICKS, 40);
        ++receives;
        if (mode == 15 || mode == 25 || mode == 28) { time_us += 10000; return 0; }
        if (mode == 20) { extra_ticks += 10; return 0; }
        if (mode == 26) return 0;
        *record = (struct pvr_mimus_record){1, PVR_MIMUS_OK};
        if (mode == 16) time_us = gpu.job_wait.end;
        if (mode == 17) time_us = gpu.job_wait.end + 1;
        if (mode == 18) atomic_store(&board.fatal, 1);
        if (mode == 19) --time_us;
        if (mode == 29) record->result = 99;
        return 1;
    }
    check(s == &gpu.session.component && wait == OS_GPU_WAIT_TICKS && opens, 9);
    *record = (struct pvr_mimus_record){mode == 6 ? 1 : 0, PVR_MIMUS_OK};
    if (mode == 1) throttled = 1;
    if (mode == 2) atomic_store(&board.fatal, 1);
    return 1;
}
int pvr_mimus_service_submit(struct pvr_mimus_service *s, const struct pvr_mimus_request *job)
{ check(mode >= 14 && s == &gpu.session.component && job->rows == 1, 10); ++submissions; return 1; }

void os_gpu_thermal_check(unsigned selected)
{
    mode = selected; os_recognizer = (TaskHandle_t)(uintptr_t)1;
    gpu.started = gpu.opened = 1; gpu.epoch = 1; gpu.component = &gpu.session.component;
    board.session.deadline_us = 60000000; atomic_store(&board.live_until, 3000000);
    if (mode >= 14) {
        struct pvr_mimus_request job = {.rows = 1};
        if (mode == 30) {
            struct pvr_mimus_record record;
            check(a733_gpu_mimus_begin(&job) == OS_GPU_CALL_LOST && !gpu.failed && !gpu.job_pending, 57);
            check(a733_gpu_mimus_finish(&record) == OS_GPU_CALL_LOST && !gpu.failed && !submissions, 58);
            gpu.opened = 0;
            check(a733_gpu_mimus_open() == OS_GPU_CALL_LOST && !gpu.failed && !sends, 59);
        } else if (mode == 27) {
            check(!a733_gpu_mimus_retire() && gpu.failed && gpu.retired && gpu.opened && !gpu.parked, 54);
            check(extra_ticks == 1000 && closes == 1 && polls == 999, 55);
        } else if ((mode >= 22 && mode <= 25) || mode == 28) {
            gpu.opened = 0; model_phase = PVR_GPU_SERVICE_STOPPED;
            int result = a733_gpu_mimus_open();
            if (mode == 23) check(result == 0 && !gpu.failed && !gpu.accepted_ticket && !opens, 41);
            else {
                check(result == OS_GPU_CALL_TIMEOUT && gpu.failed && !gpu.opened && gpu.accepted_ticket, 42);
                unsigned before = sends;
                check(a733_gpu_mimus_open() == OS_GPU_CALL_TIMEOUT && sends == before, 43);
                if (mode == 22) check(pending, 44);
                if (mode == 24) check(!pending && !receives, 45);
                if (mode == 28) check(receives == 40 && time_us == 1000100, 56);
            }
        } else {
            check(a733_gpu_mimus_begin(&job) == 1 && gpu.job_pending && submissions == 1, 46);
            if (mode == 21) check(a733_gpu_mimus_begin(&job) == OS_GPU_CALL_LOST && submissions == 1, 47);
            struct pvr_mimus_record record = {0xabc, PVR_MIMUS_REFUSED};
            int result = a733_gpu_mimus_finish(&record);
            if (mode == 14) check(result == 1 && record.sequence == 1 && !gpu.job_pending && !gpu.failed, 48);
            else {
                int expected = mode == 18 || mode == 19 || mode == 21 || mode == 29 ? OS_GPU_CALL_LOST : OS_GPU_CALL_TIMEOUT;
                check(result == expected && gpu.job_pending && gpu.failed && record.sequence == 0xabc, 49);
                unsigned before = receives;
                check(a733_gpu_mimus_finish(&record) == expected && receives == before, 50);
                check(a733_gpu_mimus_begin(&job) == expected && submissions == 1 && !closes, 51);
                if (mode == 15 || mode == 20) check(receives == 200, 52);
                if (mode == 26) check(receives == 4004, 53);
            }
        }
        os_check_result = 1; __asm__ volatile("brk #0"); for (;;) {}
    }
    if (mode == 0) {
        for (unsigned i = 0; i < 2; ++i) {
            throttled = 1;
            check(a733_gpu_mimus_thermal() == 0 && gpu.parked && !gpu.opened && !waiting, 11);
            throttled = 0;
            check(a733_gpu_mimus_thermal() == 1 && !gpu.parked && gpu.opened && !waiting, 12);
        }
        check(opens == 2 && closes == 2 && gpu.epoch == 3, 13);
    } else if (mode == 1 || mode == 2 || mode == 6) {
        gpu.opened = 0; gpu.parked = 1; model_phase = PVR_GPU_SERVICE_STOPPED;
        int result = a733_gpu_mimus_thermal();
        check(opens == 1, 14);
        if (mode == 1) check(result == 0 && closes == 1 && gpu.parked && !waiting, 15);
        else {
            check(result == -1 && !closes && gpu.failed && waiting, 16);
            unsigned before = sends;
            atomic_store(&board.fatal, 0); throttled = 0;
            check(a733_gpu_mimus_thermal() == -1 && sends == before, 17);
            check(a733_gpu_mimus_open() < 0 && sends == before, 24);
            const struct pvr_mimus_request job = {0};
            check(a733_gpu_mimus_begin(&job) == -1, 18);
        }
    } else if (mode == 3) {
        throttled = 1;
        check(a733_gpu_mimus_thermal() == -1 && sends == 1 && closes == 1 && gpu.failed, 19);
        check(time_us == 1000100 && waiting, 20);
    } else if (mode == 4) {
        check(a733_gpu_mimus_thermal() == -1 && !sends, 21);
    } else if (mode == 5) {
        atomic_store(&board.live_until, time_us);
        check(a733_gpu_mimus_thermal() == -1 && !sends, 22);
        atomic_store(&board.live_until, 3000000);
        check(a733_gpu_mimus_thermal() == -1 && !sends && gpu.failed, 25);
    } else if (mode >= 7 && mode <= 13) {
        if (mode == 12) pending = PVR_GPU_SERVICE_STATUS + 1;
        const int retired = a733_gpu_mimus_retire();
        const unsigned before = sends;
        if (mode == 11) {
            check(!retired && !gpu.retired && !gpu.failed && !sends && !waiting, 27);
        } else if (mode == 7) {
            check(retired && gpu.retired && gpu.parked && !gpu.opened && !gpu.failed && !waiting, 28);
            check(closes == 1 && !opens && !pending && model_phase == PVR_GPU_SERVICE_STOPPED, 29);
            throttled = 1; check(a733_gpu_mimus_thermal() == 0, 30);
            throttled = 0; check(a733_gpu_mimus_thermal() == 0 && !a733_gpu_mimus_open(), 31);
            const struct pvr_mimus_request job = {0};
            check(a733_gpu_mimus_begin(&job) == 0 && a733_gpu_mimus_retire() == 1 && sends == before, 32);
        } else {
            check(!retired && gpu.retired && gpu.failed && waiting && !gpu.parked && gpu.opened, 33);
            check(closes == (mode == 12 ? 0u : 1u) && !opens, 34);
            if (mode == 9) check(time_us == 1000100 && pending, 35);
            if (mode == 12) check(pending == PVR_GPU_SERVICE_STATUS + 1 && !sends, 36);
            atomic_store(&board.fatal, 0); atomic_store(&board.live_until, 3000000);
            check(!a733_gpu_mimus_retire() && a733_gpu_mimus_open() < 0 && a733_gpu_mimus_thermal() == -1, 37);
            const struct pvr_mimus_request job = {0};
            check(a733_gpu_mimus_begin(&job) == -1 && sends == before, 38);
        }
    } else check(0, 23);
    os_check_result = 1; __asm__ volatile("brk #0"); for (;;) {}
}
