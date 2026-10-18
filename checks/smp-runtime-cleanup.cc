#define _GNU_SOURCE 1
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "runtime.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <reent.h>

// Reuse after the TLS destructor returns requires this exact kernel branch:
// prvDeleteTCB performs no further TCB access for static-only, non-MPU tasks.
#if configSUPPORT_STATIC_ALLOCATION != 1 || configSUPPORT_DYNAMIC_ALLOCATION != 0 || configENABLE_MPU != 0
#error "Reclamation fixture requires static-only tasks without MPU wrappers"
#endif

extern "C" void runtime_smp_init(void);
extern "C" void __real__reclaim_reent(struct _reent *);
static StaticTask_t controller_state, victim_state;
static StackType_t controller_stack[4096], victim_stack[4096];
static struct _reent *victim_reent, *controller_reent;
static unsigned active_generation, reclaimed, closed, migrated;
static size_t written;

static ssize_t cookie_write(void *, const char *data, size_t bytes)
{
    configASSERT(bytes == 7 && !std::memcmp(data, "pending", 7));
    written += bytes;
    return static_cast<ssize_t>(bytes);
}
static int cookie_close(void *cookie)
{
    configASSERT(cookie == &active_generation);
    configASSERT(__getreent() != victim_reent);
    __atomic_add_fetch(&closed, 1, __ATOMIC_RELEASE);
    return 0;
}
static FILE *open_cookie()
{
    cookie_io_functions_t functions = {};
    functions.write = cookie_write;
    functions.close = cookie_close;
    FILE *stream = fopencookie(&active_generation, "w", functions);
    configASSERT(stream);
    return stream;
}

// The linker substitutes only this observer; the pinned newlib destructor
// executes in full. No victim state or storage is accessed after publication.
extern "C" void __wrap__reclaim_reent(struct _reent *context)
{
    configASSERT(context == victim_reent && context != __getreent());
    configASSERT(__atomic_load_n(&migrated, __ATOMIC_ACQUIRE) == active_generation);
    __real__reclaim_reent(context);
    configASSERT(__atomic_load_n(&closed, __ATOMIC_ACQUIRE) == active_generation + 1);
    __atomic_store_n(&reclaimed, active_generation, __ATOMIC_RELEASE);
}

static void victim(void *argument)
{
    const unsigned generation = static_cast<unsigned>(reinterpret_cast<uintptr_t>(argument));
    struct _reent *context = __getreent();
    configASSERT(errno == 0 && !_REENT_CVTBUF(context) && !_REENT_MP_FREELIST(context));
    configASSERT(context != controller_reent);
    victim_reent = context;
    configASSERT(portGET_CORE_ID() == generation - 1);
    FILE *stream = open_cookie();
    configASSERT(std::fputs("pending", stream) >= 0);
    configASSERT(_REENT_CLEANUP(context));
    _REENT_STDOUT(context) = stream;
    int decimal, sign;
    char *end;
    configASSERT(_dtoa_r(context, 1.0 / 3.0, 3, 48, &decimal, &sign, &end));
    configASSERT(_REENT_MP_RESULT(context) && _REENT_MP_FREELIST(context));
    char words[] = "task,continued";
    configASSERT(!std::strcmp(std::strtok(words, ","), "task"));
    errno = 100 + generation;
    vTaskCoreAffinitySet(nullptr, 1U << (2 - generation));
    vTaskDelay(1);
    configASSERT(portGET_CORE_ID() == 2 - generation && __getreent() == context);
    configASSERT(errno == static_cast<int>(100 + generation));
    configASSERT(!std::strcmp(std::strtok(nullptr, ","), "continued"));
    __atomic_store_n(&migrated, generation, __ATOMIC_RELEASE);
    vTaskDelete(nullptr);
    platform_panic("deleted task resumed", 0);
}

static void controller(void *)
{
    controller_reent = __getreent();
    // Initialize global FILE storage and controller stdio before the baseline.
    FILE *warm = open_cookie();
    configASSERT(std::fputs("pending", warm) >= 0 && std::fclose(warm) == 0);
    configASSERT(closed == 1 && written == 7);
    errno = 71;
    const size_t baseline = mallinfo().uordblks;
    for (unsigned generation = 1; generation <= 2; ++generation) {
        active_generation = generation;
        errno = 71;
        configASSERT(xTaskCreateStaticAffinitySet(victim, "delete", 4096,
            reinterpret_cast<void *>(static_cast<uintptr_t>(generation)), 3,
            victim_stack, &victim_state, 1U << (generation - 1)));
        const TickType_t start = xTaskGetTickCount();
        while (__atomic_load_n(&reclaimed, __ATOMIC_ACQUIRE) != generation) {
            configASSERT(xTaskGetTickCount() - start < 1000);
            vTaskDelay(1);
        }
        configASSERT(errno == 71 && __getreent() == controller_reent);
        configASSERT(mallinfo().uordblks == baseline);
        configASSERT(written == 7 * (generation + 1));
        // A stream after cleanup must obtain and release a dynamic libc lock.
        FILE *reused = fmemopen(nullptr, 64, "w+");
        configASSERT(reused && std::fputs("reuse", reused) >= 0);
        configASSERT(std::fclose(reused) == 0 && mallinfo().uordblks == baseline);
    }
    platform_puts("PASS SMP cleanup: two migrated tasks reclaimed; private stdout closed, heap restored and static task storage reused\n");
    platform_exit(0);
}
extern "C" void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }
extern "C" void kernel_main(void)
{
    platform_init();
    runtime_smp_init();
    runtime_init();
    configASSERT(xTaskCreateStaticAffinitySet(controller, "cleanup", 4096, nullptr, 3,
        controller_stack, &controller_state, 1));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
